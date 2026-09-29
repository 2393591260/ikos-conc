#!/usr/bin/env python3
"""Cross-validate IKOS against CPAchecker on the SV-COMP no-data-race tasks.

For each task, runs both tools and compares:
  * verdict (TRUE=no race / FALSE=race / UNKNOWN)
  * the reported race location (first conflicting access's line)

CPAchecker config: `svcomp26--datarace` (the exact config SV-COMP 2026 used
for C.no-data-race), data model forced to 64-bit with `-64`.

Usage:
    python3 svcomp/compare_tools.py [--limit N] [--timeout S] [task ...]

With no task list, reads a small built-in sample; with a task list, runs them.
Outputs a CSV + a markdown divergence summary.
"""

import argparse
import csv
import json
import os
import re
import subprocess
import sys
import tempfile
import time

CPA = "/home/ruan/CPAchecker-4.2.2-unix/CPAchecker-4.2.2-unix/scripts/cpa.sh"
IKOS = "/home/ruan/ikos-conc/install/bin/ikos"
BENCH = "/home/ruan/sv-benchmarks-2026/c"

RE_CPA_RESULT = re.compile(r"Verification result:\s*(\w+)")
RE_CPA_RACE_LINE = re.compile(r"data race in line\s+(\d+)", re.IGNORECASE)


def cpa_run(path, timeout):
    """Return (verdict, race_line) from CPAchecker. verdict in {TRUE,FALSE,UNKNOWN}."""
    t0 = time.time()
    try:
        p = subprocess.run(
            ["bash", CPA, "-svcomp26--datarace", "-64", path],
            capture_output=True, text=True, timeout=timeout)
        out = p.stdout + p.stderr
        timed_out = False
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or "") + (e.stderr or "")
        timed_out = True
    m = RE_CPA_RESULT.search(out)
    verdict = "UNKNOWN" if timed_out else (m.group(1) if m else "ERROR")
    line = None
    lm = RE_CPA_RACE_LINE.search(out)
    if lm:
        line = int(lm.group(1))
    return verdict, line, time.time() - t0


def ikos_run(path, timeout):
    """Return (verdict, first_race_locs) from IKOS. verdict in {TRUE,FALSE,UNKNOWN}."""
    db = tempfile.mktemp(suffix=".db")
    rep = tempfile.mktemp(suffix=".json")
    t0 = time.time()
    try:
        p = subprocess.run(
            [IKOS, "--analyses=race", "--concurrency=auto",
             "--format=json", "--report-file=" + rep, "-o", db, path],
            capture_output=True, text=True, timeout=timeout)
        out = p.stdout
        timed_out = False
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or "") + (e.stderr or "")
        timed_out = True
    if timed_out:
        verdict = "UNKNOWN"
    elif "The program is definitely UNSAFE" in out:
        verdict = "FALSE"
    elif "The program is SAFE" in out:
        verdict = "TRUE"
    elif "The program is UNKNOWN" in out:
        verdict = "UNKNOWN"
    else:
        verdict = "ERROR"
    locs = None
    try:
        with open(rep) as f:
            data = json.load(f)
        for r in data.get("reports", []):
            info = r.get("info", {})
            a = info.get("access_a", {})
            b = info.get("access_b", {})
            if "line" in a and "line" in b:
                locs = (a["line"], b["line"])
                break
    except (OSError, json.JSONDecodeError):
        pass
    for f in (db, rep, db + "-wal", db + "-shm"):
        try:
            os.remove(f)
        except OSError:
            pass
    return verdict, locs, time.time() - t0


def classify(exp, ikos, cpa):
    """Return a human-readable divergence tag."""
    ikos_is_race = (ikos == "FALSE")
    cpa_is_race = (cpa == "FALSE")
    if ikos_is_race and cpa_is_race:
        return "both-RACE (agree)"
    if not ikos_is_race and not cpa_is_race:
        return "both-non-RACE"
    if ikos_is_race and not cpa_is_race:
        return "IKOS=RACE / CPA=" + cpa + " (IKOS FP or CPA FN)"
    if not ikos_is_race and cpa_is_race:
        return "IKOS=" + ikos + " / CPA=RACE (CPA FP or IKOS FN)"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tasks", nargs="*")
    ap.add_argument("--timeout", type=int, default=90)
    ap.add_argument("--out-csv", default="/tmp/compare_tools.csv")
    args = ap.parse_args()

    # default sample: mix of TP / FP / TN across goblint-regression
    tasks = args.tasks or [
        "goblint-regression/04-mutex_01-simple_rc.c",
        "goblint-regression/06-symbeq_10-equ_rc.c",
        "goblint-regression/09-regions_01-list_rc.c",
        "goblint-regression/04-mutex_02-simple_nr.c",
        "goblint-regression/04-mutex_04-munge_nr.c",
        "goblint-regression/04-mutex_05-lockfuns.c",
        "goblint-regression/06-symbeq_15-list_entry_nr.i",
        "goblint-regression/09-regions_06-ptra_nr.i",
        "goblint-regression/28-race_reach_82-list_racefree.i",
        "goblint-regression/13-privatized_40-traces-ex-6_true.c",
        "pthread-race-challenges/value-barrier.i",
        "pthread-race-challenges/per-thread-array-index.i",
    ]

    rows = []
    for t in tasks:
        path = os.path.join(BENCH, t)
        if not os.path.isfile(path):
            print(f"SKIP (missing): {t}", file=sys.stderr)
            continue
        # expected verdict from sibling .yml
        yml = path[:-2] + ".yml" if path.endswith((".c", ".i")) else None
        exp = "?"
        if yml and os.path.isfile(yml):
            txt = open(yml, errors="replace").read()
            if "no-data-race" in txt:
                if "expected_verdict: true" in txt:
                    exp = "TRUE"
                elif "expected_verdict: false" in txt:
                    exp = "FALSE"
        ikos, ikos_locs, ikos_t = ikos_run(path, args.timeout)
        cpa, cpa_line, cpa_t = cpa_run(path, args.timeout)
        rows.append({
            "task": t, "expected": exp,
            "ikos": ikos, "ikos_locs": ikos_locs, "ikos_time": round(ikos_t, 1),
            "cpa": cpa, "cpa_line": cpa_line, "cpa_time": round(cpa_t, 1),
            "tag": classify(exp, ikos, cpa),
        })
        print(f"[{len(rows)}/{len(tasks)}] {t}: IKOS={ikos}{ikos_locs} CPA={cpa}@{cpa_line} ({rows[-1]['tag']})",
              file=sys.stderr)

    # CSV
    with open(args.out_csv, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=[
            "task", "expected", "ikos", "ikos_locs", "ikos_time",
            "cpa", "cpa_line", "cpa_time", "tag"])
        w.writeheader()
        for r in rows:
            r2 = dict(r)
            r2["ikos_locs"] = str(r2["ikos_locs"])
            w.writerow(r2)

    # markdown summary
    print("\n# IKOS vs CPAchecker 对比（样本 %d 个）\n" % len(rows))
    print("| task | 期望 | IKOS | IKOS 位置 | CPAchecker | CPA 位置 | 分歧 |")
    print("|---|---|---|---|---|---|---|")
    for r in rows:
        il = f"{r['ikos_locs'][0]},{r['ikos_locs'][1]}" if r['ikos_locs'] else "-"
        print(f"| {r['task']} | {r['expected']} | {r['ikos']} | {il} | {r['cpa']} | {r['cpa_line'] or '-'} | {r['tag']} |")

    # aggregate
    from collections import Counter
    tags = Counter(r["tag"] for r in rows)
    print("\n## 汇总\n")
    for tag, n in tags.most_common():
        print(f"- {tag}: {n}")
    agree_race = tags.get("both-RACE (agree)", 0)
    ikos_race_cpa_not = sum(1 for r in rows if r["tag"].startswith("IKOS=RACE"))
    print(f"\n结论: IKOS 报 RACE 而 CPAchecker 不报的有 {ikos_race_cpa_not} 个（候选 FP）;")


if __name__ == "__main__":
    main()
