#!/usr/bin/env python3
"""Run a pinned local Dartagnan on every UNKNOWN in a saved IKOS baseline."""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def classify(output, returncode, timed_out):
    if timed_out:
        return "TIMEOUT"
    if returncode != 0:
        return "ERROR"
    matches = re.findall(r"^Result:\s*(FAIL|PASS|UNKNOWN)\s*$", output, re.M)
    return matches[-1] if matches else "ERROR"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--baseline", type=Path, required=True)
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--dartagnan", type=Path,
                    default=Path("/home/ruan/dartagnan-svcomp2026/dartagnan"))
    ap.add_argument("--svbench", type=Path,
                    default=Path("/home/ruan/sv-benchmarks-2026"))
    ap.add_argument("--jobs", type=int, default=3)
    ap.add_argument("--timeout", type=int, default=90)
    ap.add_argument("--java-heap", choices=["1g", "2g", "4g"], default="2g",
                    help="heap cap for both launcher and engine JVMs")
    ap.add_argument("--resume", action="store_true")
    args = ap.parse_args()
    if args.jobs < 1 or args.timeout < 1:
        ap.error("jobs and timeout must be positive")
    tasks = [row for row in json.loads(args.baseline.read_text())
             if row["verdict"] == "Unknown"]
    if not tasks or len({r['task'] for r in tasks}) != len(tasks):
        ap.error("empty or duplicate task list")
    root = args.dartagnan.resolve()
    config = dict(baseline_sha256=digest(args.baseline), timeout=args.timeout,
                  launcher_sha256=digest(root / "svcomp/target/svcomp.jar"),
                  library_sha256=digest(root / "svcomp/target/libs/dartagnan-4.3.1.jar"),
                  cat_sha256=digest(root / "cat/svcomp.cat"),
                  property_sha256=digest(args.svbench / "c/properties/no-data-race.prp"),
                  launcher_java_heap=args.java_heap, engine_java_heap=args.java_heap,
                  skip_assertions="USER", tasks=len(tasks))
    args.output = args.output.resolve()
    if args.resume:
        if json.loads((args.output / "config.json").read_text()) != config:
            ap.error("resume configuration changed")
    else:
        args.output.mkdir(parents=True, exist_ok=False)
        (args.output / "config.json").write_text(json.dumps(config, indent=2) + "\n")

    def run(task):
        key = task["task"]
        folder = args.output / (Path(key).stem + "-" + hashlib.sha256(key.encode()).hexdigest()[:12])
        folder.mkdir(exist_ok=args.resume)
        result_path = folder / "result.json"
        source_hash = digest(task["input"])
        yaml_hash = digest(args.svbench / key)
        if result_path.exists():
            saved = json.loads(result_path.read_text())
            if saved["input_sha256"] != source_hash or saved["yaml_sha256"] != yaml_hash:
                raise ValueError("task changed since prior run: " + key)
            return saved
        bits = {"ILP32": "32", "LP64": "64"}[task["data_model"]]
        env = dict(os.environ, DAT3M_HOME=str(root), DAT3M_OUTPUT=str(folder),
                   CFLAGS="-fgnu89-inline -m" + bits,
                   JAVA_TOOL_OPTIONS="-Xmx" + args.java_heap)
        command = ["java", "-Xmx" + args.java_heap, "-jar", "svcomp/target/svcomp.jar",
                   "--program.processing.skipAssertionsOfType=USER", "cat/svcomp.cat",
                   "--svcomp.property=" + str(args.svbench / "c/properties/no-data-race.prp"),
                   task["input"]]
        started = time.monotonic()
        timed_out = False
        with (folder / "stdout.log").open("w") as stdout, (folder / "stderr.log").open("w") as stderr:
            proc = subprocess.Popen(command, cwd=root, env=env, stdout=stdout,
                                    stderr=stderr, start_new_session=True)
            try:
                returncode = proc.wait(timeout=args.timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                returncode = proc.wait()
        output = (folder / "stdout.log").read_text(errors="replace")
        errors = (folder / "stderr.log").read_text(errors="replace")
        row = dict(task=key, input=task["input"], input_sha256=source_hash,
                   yaml_sha256=yaml_hash, expected=task["expected"],
                   data_model=task["data_model"], ikos_before=task["verdict"],
                   ikos_before_blockers=task["counts"], reference=classify(output, returncode, timed_out),
                   returncode=returncode, seconds=time.monotonic() - started,
                   command=command, environment={k: env[k] for k in ("DAT3M_HOME", "DAT3M_OUTPUT", "CFLAGS", "JAVA_TOOL_OPTIONS")},
                   artifacts=str(folder.relative_to(args.output)),
                   reference_reason=re.findall(r"^Reason:\s*(.*)$", output, re.M),
                   error_tail=errors[-1600:] if returncode else "")
        temporary = result_path.with_suffix(".tmp")
        temporary.write_text(json.dumps(row, indent=2) + "\n")
        temporary.replace(result_path)
        return row

    rows = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for future in as_completed([pool.submit(run, task) for task in tasks]):
            row = future.result()
            rows.append(row)
            print(f"{len(rows)}/{len(tasks)} {row['reference']} {row['task']}", flush=True)
    rows.sort(key=lambda row: row["task"])
    (args.output / "results.json").write_text(json.dumps(rows, indent=2) + "\n")
    summary = dict(tasks=len(rows), outcomes=dict(Counter(r["reference"] for r in rows)),
                   fail_by_label=dict(Counter(r["expected"] for r in rows if r["reference"] == "FAIL")),
                   note="PASS, UNKNOWN, TIMEOUT and ERROR are distinct. FAIL is not exact IKOS model validation.")
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    raise SystemExit(main())
