#!/usr/bin/env python3
"""Goblint-regression YAML A/B test (off vs on, relational interference).

Walks all .yml files under a directory, classifies each into the right
analysis (race / prover) based on the property_file field, then runs the
binary in both modes and reports confusion matrices + the delta in FP
introduced by switching on the relational backend.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import time
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Optional


VERDICT_SAFE = "Safe"
VERDICT_RACE = "Race Found"
VERDICT_UNKNOWN = "Unknown"
RE_SAFE = re.compile(r"\bThe program is SAFE\b")
RE_RACE = re.compile(r"\bpotential data race\b", re.IGNORECASE)
RE_UNKNOWN = re.compile(r"\bThe program is UNKNOWN\b")


def expected_verdict_from_yaml(yml: Path) -> Optional[str]:
    if not yml.is_file():
        return None
    text = yml.read_text(errors="replace")
    prp = ""
    for line in text.splitlines():
        # List items strip to "- property_file: ..." — match the key,
        # not the line start.
        s = line.strip()
        if s.startswith("- "):
            s = s[2:].strip()
        if s.startswith("property_file:"):
            prp = s.split(":", 1)[1].strip()
            break
    if "unreach-call" in prp:
        # prover-only property: we don't compute Safe/Race; skip
        return "UNREACH"
    if "no-data-race" not in prp:
        return None
    if "expected_verdict: true" in text:
        return VERDICT_SAFE
    if "expected_verdict: false" in text:
        return VERDICT_RACE
    if "expected_verdict: unknown" in text:
        return VERDICT_UNKNOWN
    return None


def source_from_yaml(yml: Path) -> Optional[Path]:
    text = yml.read_text(errors="replace")
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("input_files:"):
            payload = s.split(":", 1)[1].strip().strip("'\"")
            cand = yml.parent / payload
            if cand.is_file():
                # Ground truth is the .c source: the .i snapshots in the
                # corpus are stale and analyze differently from .c under
                # the same pipeline (verified 2026-09-08).
                c_alt = cand.with_suffix(".c")
                if c_alt.is_file():
                    return c_alt
                return cand
            if not cand.is_absolute():
                cand = (yml.parent / payload).resolve()
            if cand.is_file():
                return cand
            for sfx in (".c", ".i"):
                alt = cand.with_suffix(sfx)
                if alt.is_file():
                    return alt
    return None


def classify(output: str, returncode: int, timed_out: bool) -> str:
    if timed_out:
        return "Error (timeout)"
    if returncode != 0:
        return f"Error (exit {returncode})"
    # "The program is UNKNOWN" must take precedence over the per-check
    # "potential data race" text (which also appears in unknown reports).
    if RE_UNKNOWN.search(output):
        return VERDICT_UNKNOWN
    if RE_RACE.search(output):
        return VERDICT_RACE
    if RE_SAFE.search(output):
        return VERDICT_SAFE
    return "Error (unrecognized)"


def run_one(source: Path, ikos: Path, mode: str, timeout_sec: int):
    start = time.perf_counter()
    timed_out = False
    returncode = -1
    out_text = ""
    args = [
        str(ikos),
        "--analyses=race",
        "--concurrency=auto",
        str(source),
    ]
    try:
        proc = subprocess.run(args, capture_output=True, text=True, timeout=timeout_sec)
        returncode = proc.returncode
        out_text = proc.stdout + proc.stderr
    except subprocess.TimeoutExpired as exc:
        timed_out = True
        out_text = (exc.stdout or b"") + (exc.stderr or b"")
        if isinstance(out_text, bytes):
            out_text = out_text.decode("utf-8", errors="replace")
    elapsed = time.perf_counter() - start
    verdict = classify(out_text, returncode, timed_out)
    return verdict, elapsed


@dataclass
class Result:
    path: str
    expected: Optional[str]
    off: str = ""
    on: str = ""


def main(argv) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("dir", type=Path)
    ap.add_argument("--ikos", type=Path, default=Path("/tmp/ikos-rel"))
    ap.add_argument("--timeout", type=int, default=15)
    ap.add_argument("--modes", default="off,on")
    ap.add_argument("--limit", type=int, default=0, help="0 = no limit")
    args = ap.parse_args(argv)

    modes = [m.strip() for m in args.modes.split(",") if m.strip()]
    ymls = sorted(args.dir.rglob("*.yml"))
    if args.limit:
        ymls = ymls[: args.limit]
    print(f"Found {len(ymls)} YAML files under {args.dir}")

    results: list[Result] = []
    for i, yml in enumerate(ymls, 1):
        src = source_from_yaml(yml)
        if src is None:
            continue
        expected = expected_verdict_from_yaml(yml)
        if expected is None or expected == "UNREACH":
            continue
        verdicts = {}
        for mode in modes:
            v, _ = run_one(src, args.ikos, mode, args.timeout)
            verdicts[mode] = v
        results.append(Result(path=str(src), expected=expected, **verdicts))
        print(f"RESULT {src} expected={expected} on={verdicts['on']}",
              file=sys.stderr)
        if i % 25 == 0:
            print(f"  ... processed {i}/{len(ymls)}", file=sys.stderr)

    # Aggregate confusion matrix per mode
    def metrics(results, mode):
        tp = fp = tn = fn = unscored = errors = unknown = 0
        for r in results:
            if r.expected is None:
                unscored += 1
                continue
            v = getattr(r, mode)
            if v.startswith("Error"):
                errors += 1
                continue
            if v == VERDICT_UNKNOWN:
                if r.expected == VERDICT_UNKNOWN:
                    unknown += 1  # expected-unknown matched
                else:
                    errors += 1  # tool gave up on a definite-verdict task
                continue
            if r.expected == VERDICT_RACE and v == VERDICT_RACE:
                tp += 1
            elif r.expected == VERDICT_SAFE and v == VERDICT_RACE:
                fp += 1
            elif r.expected == VERDICT_SAFE and v == VERDICT_SAFE:
                tn += 1
            elif r.expected == VERDICT_RACE and v == VERDICT_SAFE:
                fn += 1
            else:
                unscored += 1
        scored = tp + fp + tn + fn
        prec = tp / (tp + fp) if (tp + fp) else 0.0
        rec = tp / (tp + fn) if (tp + fn) else 0.0
        return dict(tp=tp, fp=fp, tn=tn, fn=fn, unscored=unscored,
                    errors=errors, unknown=unknown, scored=scored,
                    precision=prec, recall=rec)

    print("\n=== Per-mode confusion matrix (race property only) ===")
    summary = {}
    for mode in modes:
        m = metrics(results, mode)
        summary[mode] = m
        print(f"  [{mode}]  TP={m['tp']:3d} FP={m['fp']:3d}  "
              f"TN={m['tn']:3d} FN={m['fn']:3d}  "
              f"err={m['errors']:3d} unknown={m['unknown']:3d}  "
              f"Precision={m['precision']:.3f}  Recall={m['recall']:.3f}")
    if len(modes) >= 2:
        off_m, on_m = summary[modes[0]], summary[modes[1]]
        print(f"\nFP delta ({modes[1]} − {modes[0]}): {on_m['fp'] - off_m['fp']:+d}")
        print(f"FN delta ({modes[1]} − {modes[0]}): {on_m['fn'] - off_m['fn']:+d}")

    # Hardcore FP list: yml expected true (Safe), on-mode verdict Race
    if "on" in summary:
        fps_on = [r for r in results
                  if r.expected == VERDICT_SAFE and getattr(r, "on") == VERDICT_RACE]
        fps_on.sort(key=lambda r: r.path)
        print(f"\n=== Relational FP list ({len(fps_on)} files; on=Race, expected=Safe) ===")
        for r in fps_on[:80]:
            print(f"  {r.path}")
        if len(fps_on) > 80:
            print(f"  ... and {len(fps_on) - 80} more")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
