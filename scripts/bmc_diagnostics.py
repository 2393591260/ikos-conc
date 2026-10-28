#!/usr/bin/env python3
"""Retain per-task BMC reports and summarize observed (overlapping) blockers."""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import svcomp_race_test as suite


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--svbench", type=Path,
                        default=suite.DEFAULT_SVBENCH)
    parser.add_argument("--ikos", type=Path, default=ROOT / "install/bin/ikos")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--input", type=Path, action="append",
                        help="restrict to these task inputs; default: established full scan")
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--timeout", type=int, default=30)
    args = parser.parse_args()
    args.svbench = args.svbench.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    wanted = {p.resolve() for p in args.input or []}
    tasks = []
    for directory in suite.CONCURRENCY_DIRS:
        for yml in sorted((args.svbench / "c" / directory).glob("*.yml")):
            task = suite.parse_task(yml)
            if task and (not wanted or task[0].resolve() in wanted):
                tasks.append((yml, *task))
    if not tasks:
        parser.error("no matching tasks")
    env = dict(os.environ, IKOS_BMC_RECOVER="1", IKOS_BMC_DIAGNOSTICS="1")

    def run(task):
        yml, source, expected, model = task
        key = str(yml.relative_to(args.svbench))
        folder = args.output / (yml.stem + "-" + hashlib.sha256(key.encode()).hexdigest()[:12])
        folder.mkdir()
        report, db = folder / "report.json", folder / "output.db"
        command = [str(args.ikos.resolve()), "--analyses=race", "--concurrency=auto",
                   "--demote-race-to-unknown", "-m", "32" if model == "ILP32" else "64",
                   "--format=json", "--report-file=" + str(report.resolve()),
                   "-o", str(db.resolve()), str(source.resolve())]
        started = time.monotonic()
        proc = subprocess.Popen(command, env=env, cwd=folder, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, start_new_session=True)
        timed_out = False
        try:
            output, _ = proc.communicate(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            output, _ = proc.communicate()
        elapsed = time.monotonic() - started
        text = output.decode(errors="replace")
        (folder / "analyzer.log").write_text(text)
        verdict = suite.classify(text, proc.returncode, timed_out)
        counts, first = Counter(), Counter()
        candidates = 0
        parsed = False
        if report.exists():
            try:
                data = json.loads(report.read_text())
                parsed = True
                for rep in data.get("reports", []):
                    diagnostic = rep.get("info", {}).get("bmc", {})
                    counts.update(diagnostic.get("counts", {}))
                    candidates += len(diagnostic.get("candidates", []))
                    candidates += diagnostic.get("omitted_candidates", 0)
                    # Deferred groups keep the earlier search evidence separately;
                    # aggregate counts already include every attempted pass.
                    for pair in (diagnostic.get("first_pass_candidates", []) +
                                 diagnostic.get("previous_activation_candidates", []) +
                                 diagnostic.get("candidates", [])):
                        if pair.get("first_blocker"):
                            first[pair["first_blocker"]] += 1
            except (ValueError, TypeError):
                pass
        if not counts:
            counts["no_bmc_diagnostics" if parsed else "report_unavailable"] = 1
        for path in folder.glob("output.db*"):
            path.unlink()
        row = dict(task=key, input=str(source), expected=expected, data_model=model,
                   verdict=verdict, seconds=elapsed, returncode=proc.returncode,
                   counts=dict(counts), first_blockers=dict(first), candidates=candidates,
                   artifacts=str(folder.relative_to(args.output)), command=command)
        (folder / "result.json").write_text(json.dumps(row, indent=2) + "\n")
        return row

    rows = []
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = [executor.submit(run, task) for task in tasks]
        for future in as_completed(futures):
            row = future.result()
            rows.append(row)
            print(f"{len(rows)}/{len(tasks)} {row['task']}: {row['verdict']}", flush=True)
    rows.sort(key=lambda row: row["task"])
    blockers, matrix = {}, Counter({k: 0 for k in ("TP", "FP", "TN", "FN", "error", "unknown")})
    for row in rows:
        verdict, expected = row["verdict"], row["expected"]
        if verdict.startswith("Error"):
            matrix["error"] += 1
        elif verdict == suite.VERDICT_UNKNOWN:
            matrix["unknown"] += 1
            for reason in row["counts"]:
                bucket = "racy" if expected == suite.VERDICT_RACE else "safe" if expected == suite.VERDICT_SAFE else "unscored"
                blockers.setdefault(reason, Counter())[bucket] += 1
        elif expected in (suite.VERDICT_RACE, suite.VERDICT_SAFE):
            matrix[("T" if verdict == expected else "F") +
                   ("P" if verdict == suite.VERDICT_RACE else "N")] += 1
    summary = dict(tasks=len(rows), matrix=dict(matrix), unknown_tasks_by_observed_reason=blockers,
                   note="Reasons overlap; counts are observed blockers, not recoverable TP estimates. "
                        "First blockers describe recorded traversal order; later stages behind failed gates were not explored.",
                   timeout=args.timeout, jobs=args.jobs, environment={"IKOS_BMC_RECOVER": "1", "IKOS_BMC_DIAGNOSTICS": "1"})
    (args.output / "results.json").write_text(json.dumps(rows, indent=2) + "\n")
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))
    return int(matrix["FP"] != 0 or matrix["FN"] != 0)


if __name__ == "__main__":
    raise SystemExit(main())
