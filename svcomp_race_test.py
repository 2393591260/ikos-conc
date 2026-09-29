#!/usr/bin/env python3
"""Run IKOS race analysis over the SV-COMP no-data-race benchmark set.

Walks the sv-benchmarks `c/` concurrency directories, picks every task whose
`.yml` carries an ACTIVE `no-data-race.prp` property, runs IKOS on its
`input_files`, and reports a confusion matrix (SAFE = no race, RACE = race).

Defaults to the SV-COMP 2026 frozen set (/home/ruan/sv-benchmarks-2026,
git worktree at tag `svcomp26-freeze`). The rolling sv-benchmarks `main`
(/home/ruan/sv-benchmarks) is kept for later optimization sweeps.

Unlike yaml_ab_test.py (curated clean-sv-benchmarks subset), this reads the
official `format_version: '2.0'` task definitions and runs on the `.i`
preprocessed files the competition tools actually consume.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

VERDICT_SAFE = "Safe"
VERDICT_RACE = "Race Found"
VERDICT_UNKNOWN = "Unknown"
RE_SAFE = re.compile(r"\bThe program is SAFE\b")
RE_RACE = re.compile(r"\bpotential data race\b", re.IGNORECASE)
RE_UNSAFE = re.compile(r"\bThe program is (definitely |potentially )?UNSAFE\b")
RE_UNKNOWN = re.compile(r"\bThe program is UNKNOWN\b")

# concurrency directories in sv-benchmarks/c that carry no-data-race tasks
CONCURRENCY_DIRS = [
    "pthread", "pthread-atomic", "pthread-ext", "pthread-lit", "ldv-races",
    "ldv-linux-3.14-races", "pthread-complex", "pthread-driver-races",
    "pthread-C-DAC", "pthread-divine", "pthread-nondet", "goblint-regression",
    "pthread-deagle", "pthread-race-challenges", "pthread-memsafety",
    "libvsync", "pthread-theta", "pthread-wmm", "weaver",
]


def parse_task(yml: Path):
    """Return (input_path, expected, data_model) for an ACTIVE no-data-race task."""
    text = yml.read_text(errors="replace")
    # find the active (uncommented) no-data-race property block
    in_ndr = False
    expected = None
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("- property_file:") or s.startswith("property_file:"):
            in_ndr = "no-data-race" in s
            continue
        if in_ndr and s.startswith("expected_verdict:"):
            if "true" in s:
                expected = VERDICT_SAFE
            elif "unknown" in s:
                expected = VERDICT_UNKNOWN
            else:
                expected = VERDICT_RACE
            in_ndr = False
    if expected is None:
        return None
    # input_files
    m = re.search(r"input_files:\s*'([^']+)'", text)
    if not m:
        return None
    src = yml.parent / m.group(1)
    if not src.is_file():
        return None
    # data model (ILP32/LP64), default LP64
    data_model = "LP64"
    dm = re.search(r"data_model:\s*(\w+)", text)
    if dm:
        data_model = dm.group(1)
    return src, expected, data_model


def classify(output: str, returncode: int, timed_out: bool) -> str:
    if timed_out:
        return "Error (timeout)"
    if returncode != 0:
        return f"Error (exit {returncode})"
    # Check the AUTHORITATIVE verdict line first. The per-check
    # "potential data race" text appears in BOTH race and unknown outputs, so
    # it must only be a fallback.
    if RE_UNSAFE.search(output):
        return VERDICT_RACE
    if RE_UNKNOWN.search(output):
        return VERDICT_UNKNOWN
    if RE_SAFE.search(output):
        return VERDICT_SAFE
    if RE_RACE.search(output):
        return VERDICT_RACE
    return "Error (unrecognized)"


def run_one(src: Path, ikos: Path, timeout_sec: int, out_db: Path, concurrency: str,
            data_model: str):
    start = time.perf_counter()
    timed_out = False
    returncode = -1
    out = ""
    machine = ["-m", "32" if data_model == "ILP32" else "64"]
    try:
        proc = subprocess.run(
            [str(ikos), "--analyses=race", f"--concurrency={concurrency}"] +
            machine +
            ["-o", str(out_db), str(src)],
            capture_output=True, text=True, timeout=timeout_sec)
        returncode = proc.returncode
        out = proc.stdout + proc.stderr
    except subprocess.TimeoutExpired as exc:
        timed_out = True
        out = (exc.stdout or "") + (exc.stderr or "")
    return classify(out, returncode, timed_out), time.perf_counter() - start


def main(argv) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("svbench", type=Path, nargs="?",
                    default=Path("/home/ruan/sv-benchmarks-2026"),
                    help="sv-benchmarks checkout root (default: 2026 freeze)")
    ap.add_argument("--ikos", type=Path,
                    default=Path("/home/ruan/ikos-conc/install/bin/ikos"))
    ap.add_argument("--timeout", type=int, default=30)
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--concurrency", default="auto", choices=["auto", "on", "off"])
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--only-fail", action="store_true", help="print only FN/FP/err")
    args = ap.parse_args(argv)

    tasks = []
    for d in CONCURRENCY_DIRS:
        d = args.svbench / "c" / d
        if not d.is_dir():
            continue
        for yml in sorted(d.glob("*.yml")):
            t = parse_task(yml)
            if t:
                tasks.append((yml, *t))
    tasks.sort(key=lambda t: str(t[0]))
    if args.limit:
        tasks = tasks[: args.limit]
    print(f"Found {len(tasks)} no-data-race tasks under {args.svbench}", file=sys.stderr)

    results = []
    def work(t):
        yml, src, expected, data_model = t
        # unique output.db per task avoids the cwd/output.db collision under
        # parallel runs
        out_db = Path(tempfile.gettempdir()) / f"ikos_{os.getpid()}_{hash(str(src)) & 0xffffffff}.db"
        v, el = run_one(src, args.ikos, args.timeout, out_db, args.concurrency,
                        data_model)
        try:
            out_db.unlink(missing_ok=True)
        except OSError:
            pass
        return (str(src), expected, data_model, v, el)

    done = 0
    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(work, t): t for t in tasks}
        for fut in as_completed(futs):
            path, expected, data_model, v, el = fut.result()
            results.append((path, expected, data_model, v))
            done += 1
            tag = "" if v == expected else "  <-- MISMATCH"
            print(f"RESULT {path} expected={expected} got={v}{tag}", file=sys.stderr)

    # confusion matrix, split by data model (ILP32 vs LP64)
    def confusion(rows):
        tp = fp = tn = fn = err = unknown = 0
        for path, expected, dm, v in rows:
            if v.startswith("Error"):
                err += 1
            elif v == VERDICT_UNKNOWN:
                if expected == VERDICT_UNKNOWN:
                    unknown += 1
                else:
                    err += 1
            elif expected == VERDICT_RACE and v == VERDICT_RACE:
                tp += 1
            elif expected == VERDICT_SAFE and v == VERDICT_RACE:
                fp += 1
            elif expected == VERDICT_SAFE and v == VERDICT_SAFE:
                tn += 1
            elif expected == VERDICT_RACE and v == VERDICT_SAFE:
                fn += 1
        scored = tp + fp + tn + fn
        prec = tp / (tp + fp) if (tp + fp) else 0.0
        rec = tp / (tp + fn) if (tp + fn) else 0.0
        return dict(tp=tp, fp=fp, tn=tn, fn=fn, err=err, unknown=unknown,
                    precision=prec, recall=rec)

    total = confusion(results)
    ilp32 = confusion([r for r in results if r[2] == "ILP32"])
    lp64 = confusion([r for r in results if r[2] == "LP64"])
    for label, m in [("TOTAL", total), ("ILP32 (32-bit)", ilp32),
                     ("LP64 (64-bit)", lp64)]:
        n = m["tp"] + m["fp"] + m["tn"] + m["fn"] + m["err"] + m["unknown"]
        print(f"\n=== IKOS no-data-race [{label}] ({n} tasks) ===")
        print(f"  TP={m['tp']} FP={m['fp']} TN={m['tn']} FN={m['fn']} "
              f"err={m['err']} unknown={m['unknown']}")
        print(f"  Precision={m['precision']:.3f}  Recall={m['recall']:.3f}")

    if args.only_fail:
        for path, expected, dm, v in results:
            if v != expected:
                print(f"  {expected}->{v}  {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
