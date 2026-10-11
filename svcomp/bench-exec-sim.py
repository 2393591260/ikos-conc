#!/usr/bin/env python3
"""Exercise the real BenchExec tool API over frozen race tasks.

Uses per-task working directories and a process-group wall timeout. It does
not replace official cgroup resource accounting or witness validation. Run
with a Python environment containing BenchExec and PyYAML. Raw logs default
to a temporary directory; keep compact summary.json/results.json as evidence."""

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import signal
import tempfile
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed

import yaml
from benchexec import util
from benchexec.tools.template import BaseTool2

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
import svcomp_race_test as base

spec = importlib.util.spec_from_file_location(
    "ikos_conc_current", ROOT / "benchexec/tools/ikos-conc.py"
)
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "svbench", type=Path, nargs="?", default=Path("/home/ruan/sv-benchmarks-2026")
    )
    ap.add_argument("--ikos", type=Path, default=ROOT / "install/bin/ikos")
    ap.add_argument(
        "--out",
        type=Path,
        help="Raw logs and JSON evidence; default: a new temporary directory",
    )
    ap.add_argument("--timeout", type=int, default=30)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args()
    if args.out is None:
        args.out = Path(tempfile.mkdtemp(prefix="ikos-adapter-sweep-"))
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    wrapper_hash = sha(args.ikos.resolve().parent / "svcomp_witness.py")
    bench = args.svbench.resolve()
    tasks = []
    for directory in base.CONCURRENCY_DIRS:
        for path in sorted((bench / "c" / directory).glob("*.yml")):
            parsed = base.parse_task(path)
            if parsed:
                tasks.append((path, *parsed))
    tasks.sort(key=lambda t: str(t[0]))
    if args.limit:
        tasks = tasks[: args.limit]

    def work(item):
        task_yaml, source, expected, model = item
        source = source.resolve()
        data = yaml.safe_load(task_yaml.read_text())
        prop = next(
            p
            for p in data["properties"]
            if p["property_file"].endswith("no-data-race.prp")
        )
        property_file = (task_yaml.parent / prop["property_file"]).resolve()
        task = BaseTool2.Task.with_files(
            [str(source)], property_file=str(property_file), options=data["options"]
        )
        tool = adapter.Tool()
        command = tool.cmdline(str(args.ikos.resolve()), [], task, None)
        workdir = args.out / "raw" / (source.stem + "-" + sha(source)[:8])
        workdir.mkdir(parents=True, exist_ok=True)
        start = time.perf_counter()
        timed_out = False
        proc = subprocess.Popen(
            command,
            cwd=workdir,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            start_new_session=True,
        )
        try:
            stdout, stderr = proc.communicate(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            os.killpg(proc.pid, signal.SIGKILL)
            stdout, stderr = proc.communicate()
        elapsed = time.perf_counter() - start
        output = stdout + stderr
        (workdir / "output.log").write_text(output)
        code = (
            util.ProcessExitCode.create(value=proc.returncode)
            if proc.returncode >= 0
            else util.ProcessExitCode.create(signal=-proc.returncode)
        )
        run = BaseTool2.Run(
            command,
            code,
            BaseTool2.RunOutput(output.splitlines(True)),
            "walltime" if timed_out else None,
        )
        verdict = tool.determine_result(run)
        observed = {
            "true": base.VERDICT_SAFE,
            "false": base.VERDICT_RACE,
            "unknown": base.VERDICT_UNKNOWN,
        }.get(verdict, "Error (timeout)" if timed_out else "Error (unrecognized)")
        if proc.returncode != 0 and not timed_out:
            observed = "Error (exit %d)" % proc.returncode
        witness = workdir / "witness.yml"
        return dict(
            task=str(source.relative_to(bench)),
            yaml=str(task_yaml.relative_to(bench)),
            expected=expected,
            model=model,
            source_sha256=sha(source),
            yaml_sha256=sha(task_yaml),
            property_sha256=sha(property_file),
            command=command,
            returncode=proc.returncode,
            timed_out=timed_out,
            adapter_verdict=verdict,
            observed=observed,
            wall_seconds=round(elapsed, 4),
            output_bytes=len(output.encode()),
            witness_sha256=sha(witness) if witness.exists() else None,
            raw_directory=str(workdir),
        )

    rows = []
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        for future in as_completed([executor.submit(work, t) for t in tasks]):
            rows.append(future.result())
            if len(rows) % 100 == 0:
                print("completed", len(rows), "/", len(tasks), flush=True)
    if sha(args.ikos.resolve().parent / "svcomp_witness.py") != wrapper_hash:
        raise RuntimeError(
            "Wrapper changed during the sweep; results cannot identify one version"
        )
    rows.sort(key=lambda r: r["task"])
    counts = dict(tp=0, fp=0, tn=0, fn=0, unknown=0, error=0)
    for r in rows:
        v, e = r["observed"], r["expected"]
        if v.startswith("Error"):
            key = "error"
        elif v == base.VERDICT_UNKNOWN or e == base.VERDICT_UNKNOWN:
            key = "unknown"
        elif v == base.VERDICT_RACE:
            key = "tp" if e == v else "fp"
        else:
            key = "tn" if e == v else "fn"
        counts[key] += 1
    summary = dict(
        counts=counts,
        tasks=len(rows),
        timeout=args.timeout,
        jobs=args.jobs,
        isolation="per-task working directory and process-group wall timeout; no cgroup accounting",
        analyzer_sha256=sha(args.ikos.parent / "ikos-analyzer"),
        wrapper_sha256=wrapper_hash,
        adapter_sha256=sha(ROOT / "benchexec/tools/ikos-conc.py"),
        max_output_bytes=max((r["output_bytes"] for r in rows), default=0),
    )
    (args.out / "results.json").write_text(json.dumps(rows, indent=2) + "\n")
    (args.out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
