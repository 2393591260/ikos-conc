#!/usr/bin/env python3
"""Run Dartagnan (SV-COMP 2026 no-data-race) on IKOS's non-TN files, save TP list.

Consumes the stderr of `svcomp_race_test.py` (RESULT lines) to know which files
IKOS did NOT classify as Safe (TN), then runs the local Dartagnan 4.3.1 on each
and records the verdict. Outputs:
  <out>_verdicts.txt   path | ikos=<v> | dartagnan=<v>   (FAIL/PASS/UNKNOWN/CRASH/TIMEOUT)
  <out>_tp.txt         the TP list (dartagnan==FAIL), annotated with IKOS verdict
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

DARTAGNAN_HOME = Path("/home/ruan/dartagnan-svcomp2026/dartagnan")
PRP = (Path.home() / "sv-benchmarks-current/c/properties/no-data-race.prp").resolve()

RE_RESULT = re.compile(r"^RESULT (.+) expected=(.+) got=(.+?)(?:  <-- MISMATCH)?$")
RE_DART = re.compile(r"^Result:\s*(\w+)", re.M)

_counter = 0
_lock = threading.Lock()


def next_outdir():
    global _counter
    with _lock:
        _counter += 1
        n = _counter
    return Path("/tmp") / f"dt3m_{os.getpid()}_{n}"


def parse_ikos_non_tn(ikos_log: Path):
    """Return [(path, ikos_verdict)] for files IKOS did not call Safe."""
    rows = []
    for line in ikos_log.read_text(errors="replace").splitlines():
        m = RE_RESULT.match(line)
        if m and m.group(3) != "Safe":
            rows.append((m.group(1), m.group(3)))
    return rows


def run_dartagnan(program, timeout):
    outdir = next_outdir()
    outdir.mkdir(parents=True, exist_ok=True)
    env = {**os.environ,
           "DAT3M_HOME": str(DARTAGNAN_HOME),
           "DAT3M_OUTPUT": str(outdir),
           "CFLAGS": "-fgnu89-inline"}
    cmd = ["java", "-jar", "svcomp/target/svcomp.jar",
           "--program.processing.skipAssertionsOfType=USER",
           "cat/svcomp.cat", f"--svcomp.property={PRP}", program]
    try:
        p = subprocess.run(cmd, cwd=str(DARTAGNAN_HOME), env=env,
                           capture_output=True, text=True, timeout=timeout)
        m = RE_DART.search(p.stdout + p.stderr)
        verdict = m.group(1) if m else "CRASH"
    except subprocess.TimeoutExpired:
        verdict = "TIMEOUT"
    finally:
        shutil.rmtree(outdir, ignore_errors=True)
    return verdict


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("ikos_log", type=Path, help="stderr of svcomp_race_test.py")
    ap.add_argument("--out", type=Path, default=Path("docs/artifacts/dartagnan_nonTN"))
    ap.add_argument("--jobs", type=int, default=6)
    ap.add_argument("--timeout", type=int, default=60)
    args = ap.parse_args(argv)

    rows = parse_ikos_non_tn(args.ikos_log)
    print(f"{len(rows)} non-TN (non-Safe) files to run through Dartagnan", file=sys.stderr)

    results = []
    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(run_dartagnan, path, args.timeout): (path, ikos)
                for path, ikos in rows}
        for fut in as_completed(futs):
            path, ikos = futs[fut]
            d = fut.result()
            results.append((path, ikos, d))
            print(f"{ikos:14s} -> {d:8s}  {path}", file=sys.stderr, flush=True)

    results.sort(key=lambda r: r[0])
    vpath = args.out.with_name(args.out.name + "_verdicts.txt")
    tpath = args.out.with_name(args.out.name + "_tp.txt")
    with open(vpath, "w") as f:
        for path, ikos, d in results:
            f.write(f"{path}|ikos={ikos}|dartagnan={d}\n")
    tp = [(p, i) for p, i, d in results if d == "FAIL"]
    with open(tpath, "w") as f:
        for p, i in tp:
            f.write(f"{p}|ikos={i}\n")
    from collections import Counter
    c = Counter(d for _, _, d in results)
    print(f"\nDartagnan verdicts: {dict(c)}", file=sys.stderr)
    print(f"TP (dartagnan FAIL) = {len(tp)} -> {tpath}", file=sys.stderr)
    print(f"  of which IKOS=Unknown (lost targets) = "
          f"{sum(1 for _, i in tp if i.startswith('Unknown'))}", file=sys.stderr)
    print(f"Full verdicts -> {vpath}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
