#!/usr/bin/env python3
"""Run goblint (full race-analysis suite) on IKOS's 108 FP files.

For each task IKOS reports RACE but the benchmark expects SAFE, run goblint
and record its verdict. The interesting set is goblint=SAFE (unsafe==0) —
those are FP files a reference tool CAN solve and IKOS can't.
"""
import re
import subprocess
import sys
import time
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

GOBLINT = "/home/ruan/goblint-analyzer/goblint"
BENCH = "/home/ruan/sv-benchmarks-2026/c"
RE_UNSAFE = re.compile(r"unsafe:\s*(\d+)")

# Full race-analysis suite approximating goblint's svcomp25.json, but keeping
# preprocessing ON so .c files parse (svcomp25.json sets pre.enabled=false and
# crashes on .c). Analyses: mutexEvents/mutex/threadid/threadflag/thread/
# threadJoins/mhp/region/var_eq/symb_locks/expRelation.
CONFIG = [
    "--enable", "ana.sv-comp.functions",
    "--set", "ana.activated[+]", "mutexEvents",
    "--set", "ana.activated[+]", "mutex",
    "--set", "ana.activated[+]", "threadid",
    "--set", "ana.activated[+]", "threadflag",
    "--set", "ana.activated[+]", "thread",
    "--set", "ana.activated[+]", "threadJoins",
    "--set", "ana.activated[+]", "mhp",
    "--set", "ana.activated[+]", "region",
    "--set", "ana.activated[+]", "var_eq",
    "--set", "ana.activated[+]", "symb_locks",
    "--set", "ana.activated[+]", "expRelation",
    "--set", "exp.region-offsets", "true",
]


def run_one(rel, timeout):
    path = Path(BENCH) / rel
    t0 = time.time()
    timed_out = False
    try:
        p = subprocess.run(
            [GOBLINT] + CONFIG + [str(path)],
            capture_output=True, text=True, timeout=timeout)
        out = p.stdout + p.stderr
    except subprocess.TimeoutExpired as e:
        out = ((e.stdout or b"").decode(errors="replace") +
               (e.stderr or b"").decode(errors="replace"))
        timed_out = True
    m = RE_UNSAFE.search(out)
    if timed_out:
        verdict = "TIMEOUT"
    elif m is None:
        verdict = "ERROR"
    else:
        verdict = "SAFE" if int(m.group(1)) == 0 else "RACE"
    return rel, verdict, time.time() - t0


def main():
    fps = [l.strip() for l in Path("/tmp/fp_list.txt").read_text().splitlines()
           if l.strip()]
    print(f"Running goblint on {len(fps)} FP files (4 parallel, timeout 120s)...",
          file=sys.stderr)
    results = []
    with open("/tmp/goblint_all_results.txt", "w") as inc:
        with ThreadPoolExecutor(max_workers=4) as ex:
            futs = {ex.submit(run_one, rel, 120): rel for rel in fps}
            for fut in as_completed(futs):
                rel, verdict, el = fut.result()
                results.append((rel, verdict, el))
                inc.write(f"{rel}\t{verdict}\t{el:.0f}\n")
                inc.flush()
                print(f"[{len(results)}/{len(fps)}] {rel} -> goblint={verdict} "
                      f"({el:.0f}s)", file=sys.stderr)
    results.sort(key=lambda r: r[0])
    c = Counter(v for _, v, _ in results)
    print(f"\n=== goblint verdicts on IKOS's {len(fps)} FP files ===")
    for v, n in c.most_common():
        print(f"  {v}: {n}")
    print("\n=== goblint=SAFE (solves the FP, IKOS doesn't) ===")
    for rel, v, el in results:
        if v == "SAFE":
            print(f"  {rel}")
    with open("/tmp/goblint_solves_fp.txt", "w") as f:
        for rel, v, el in results:
            if v == "SAFE":
                f.write(rel + "\n")


if __name__ == "__main__":
    main()
