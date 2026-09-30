#!/usr/bin/env python3
"""Run CPAchecker (svcomp26--datarace, -32) on IKOS's 108 FP files.

For each task IKOS reports RACE but the benchmark expects SAFE, run CPAchecker
and record its verdict. The interesting set is CPAchecker=TRUE (correctly
proves SAFE) — those are FP files a different tool CAN solve and we can't.
"""
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

CPA = "/home/ruan/CPAchecker-4.2.2-unix/CPAchecker-4.2.2-unix/scripts/cpa.sh"
BENCH = "/home/ruan/sv-benchmarks-2026/c"
RE_RESULT = re.compile(r"Verification result:\s*(\w+)")

# Clean CPAchecker's ./output dir once at start so each run gets a fresh one
# (CPAchecker reuses/overwrites it; removing avoids stale-state confusion).


def run_one(rel, timeout):
    path = Path(BENCH) / rel
    t0 = time.time()
    timed_out = False
    try:
        p = subprocess.run(
            ["bash", CPA, "-svcomp26--datarace", "-32", str(path)],
            capture_output=True, text=True, timeout=timeout)
        out = p.stdout + p.stderr
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or "") + (e.stderr or "")
        timed_out = True
    m = RE_RESULT.search(out)
    verdict = "TIMEOUT" if timed_out else (m.group(1) if m else "ERROR")
    return rel, verdict, time.time() - t0


def main():
    fps = [l.strip() for l in Path("/tmp/fp_list.txt").read_text().splitlines()
           if l.strip()]
    print(f"Running CPAchecker on {len(fps)} FP files (4 parallel, timeout 120s)...",
          file=sys.stderr)
    results = []
    with ThreadPoolExecutor(max_workers=4) as ex:
        futs = {ex.submit(run_one, rel, 120): rel for rel in fps}
        for fut in as_completed(futs):
            rel, verdict, el = fut.result()
            results.append((rel, verdict, el))
            print(f"[{len(results)}/{len(fps)}] {rel} -> CPA={verdict} "
                  f"({el:.0f}s)", file=sys.stderr)
    results.sort(key=lambda r: r[0])
    from collections import Counter
    c = Counter(v for _, v, _ in results)
    print(f"\n=== CPAchecker verdicts on IKOS's {len(fps)} FP files ===")
    for v, n in c.most_common():
        print(f"  {v}: {n}")
    print("\n=== CPAchecker=TRUE (solves the FP, IKOS doesn't) ===")
    for rel, v, el in results:
        if v == "TRUE":
            print(f"  {rel}")
    # also write the TRUE list to a file for follow-up
    with open("/tmp/cpa_solves_fp.txt", "w") as f:
        for rel, v, el in results:
            if v == "TRUE":
                f.write(rel + "\n")


if __name__ == "__main__":
    main()
