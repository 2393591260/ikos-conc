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
import gzip
import hashlib
import json
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "scripts"))

VERDICT_SAFE = "Safe"
VERDICT_RACE = "Race Found"
VERDICT_UNKNOWN = "Unknown"
VERDICT_LINES = {
    "The program is SAFE": VERDICT_SAFE,
    "The program is definitely UNSAFE": VERDICT_RACE,
    "The program is potentially UNSAFE": VERDICT_UNKNOWN,
    "The program is UNKNOWN": VERDICT_UNKNOWN,
}

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
    # Per-check diagnostics cannot substitute for a complete verdict line.
    output = re.sub(r"\x1b\[[0-9;]*m", "", output)
    verdicts = {VERDICT_LINES[line] for line in output.splitlines()
                if line in VERDICT_LINES}
    if len(verdicts) != 1:
        return "Error (conflicting verdicts)" if verdicts else "Error (unrecognized)"
    return verdicts.pop()


def run_one(src: Path, ikos: Path, timeout_sec: int, out_db: Path, concurrency: str,
            data_model: str, demote_races: bool, *, report_path=None,
            bmc_deadline_ms=None, bmc_solver_timeout_ms=None, command=None,
            cancel_event=None):
    start = time.perf_counter()
    timed_out = False
    returncode = -1
    out = ""
    machine = ["-m", "32" if data_model == "ILP32" else "64"]
    demote = ["--demote-race-to-unknown"] if demote_races else []
    options = []
    if report_path is not None:
        options += ["--format=json", "--report-file=" + str(report_path)]
    for name, value in (("bmc-deadline-ms", bmc_deadline_ms),
                        ("bmc-solver-timeout-ms", bmc_solver_timeout_ms)):
        if value is not None:
            options.append(f"--{name}={value}")
    proc = subprocess.Popen(
        list(command or [str(ikos)]) +
        ["--analyses=race", f"--concurrency={concurrency}"] + machine + demote +
        options + ["-o", str(out_db), str(src)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        start_new_session=(os.name == "posix"))

    def stop():
        try:
            if os.name == "posix":
                os.killpg(proc.pid, signal.SIGKILL)
            else:
                proc.kill()
        except ProcessLookupError:
            pass

    cancelled = False
    try:
        while True:
            remaining = timeout_sec - (time.perf_counter() - start)
            cancelled = cancel_event is not None and cancel_event.is_set()
            if remaining <= 0 or cancelled:
                timed_out = not cancelled
                stop()
                stdout, stderr = proc.communicate()
                break
            try:
                stdout, stderr = proc.communicate(
                    timeout=min(remaining, 0.2) if cancel_event is not None else remaining)
                break
            except subprocess.TimeoutExpired:
                continue
    except BaseException:
        stop()
        proc.communicate()
        raise
    returncode = proc.returncode
    out = stdout.decode(errors="replace") + stderr.decode(errors="replace")
    if cancelled:
        return "Error (cancelled)", time.perf_counter() - start
    return classify(out, returncode, timed_out), time.perf_counter() - start


def confusion(rows):
    tp = fp = tn = fn = err = unknown = 0
    race_population = safe_population = 0
    for path, expected, dm, verdict in rows:
        race_population += expected == VERDICT_RACE
        safe_population += expected == VERDICT_SAFE
        if verdict.startswith("Error"):
            err += 1
        elif verdict == VERDICT_UNKNOWN:
            unknown += 1
        elif expected == VERDICT_RACE and verdict == VERDICT_RACE:
            tp += 1
        elif expected == VERDICT_SAFE and verdict == VERDICT_RACE:
            fp += 1
        elif expected == VERDICT_SAFE and verdict == VERDICT_SAFE:
            tn += 1
        elif expected == VERDICT_RACE and verdict == VERDICT_SAFE:
            fn += 1
    return dict(tasks=len(rows), tp=tp, fp=fp, tn=tn, fn=fn, err=err, unknown=unknown,
                race_population=race_population, safe_population=safe_population,
                precision=tp / (tp + fp) if tp + fp else 0.0,
                race_coverage=tp / race_population if race_population else 0.0,
                safe_coverage=tn / safe_population if safe_population else 0.0)


def file_sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, sort_keys=True, indent=2) + "\n")
    temporary.replace(path)


def collect_report(report_path, verdict, evidence_dir, key):
    """After analysis stops, parse/hash a raw report and compress selected rows."""
    result = dict(report_status="missing", executions=[])
    if not report_path.is_file():
        return result
    result["report_sha256"] = file_sha256(report_path)
    try:
        report = json.loads(report_path.read_text())
        if not isinstance(report, dict) or not isinstance(report.get("reports"), list):
            raise ValueError("report must contain a reports array")
        result["report_status"] = "parsed"
        if verdict != VERDICT_RACE:
            return result
        selected = []
        for index, row in enumerate(report["reports"]):
            info = row.get("info", {})
            if "confirmed_pair_id" not in info:
                continue
            replay = info.get("counterexample")
            if replay is None:
                replay = next((c["replay"] for c in info.get("bmc", {}).get("candidates", [])
                               if c.get("pair_id") == info["confirmed_pair_id"] and
                               "replay" in c), None)
            selected_info = {name: value for name, value in info.items() if name != "bmc"}
            selected_info["counterexample"] = replay
            selected.append(dict(row, original_group=index, info=selected_info))
        artifact = evidence_dir / (key + ".selected.json.gz")
        with gzip.open(artifact, "wt", encoding="utf-8") as stream:
            json.dump(dict(reports=selected), stream, separators=(",", ":"))
        result.update(evidence=artifact.name, evidence_sha256=file_sha256(artifact))
    except (OSError, ValueError, TypeError, KeyError, AttributeError) as exc:
        result.update(report_status="invalid", report_error=str(exc))
    return result


def main(argv) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("svbench", type=Path, nargs="?",
                    default=Path("/home/ruan/sv-benchmarks-2026"),
                    help="sv-benchmarks checkout root (default: 2026 freeze)")
    ap.add_argument("--ikos", type=Path,
                    default=Path(__file__).resolve().parent / "install/bin/ikos")
    ap.add_argument("--timeout", type=int, default=30)
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--concurrency", default="auto", choices=["auto", "on", "off"])
    ap.add_argument("--demote", action="store_true",
                    help="pass --demote-race-to-unknown (races become UNKNOWN)")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--only-fail", action="store_true", help="print only FN/FP/err")
    ap.add_argument("--evidence-dir", type=Path,
                    help="new directory for same-run JSON/selected AR evidence; local report-mode timing, not official performance")
    ap.add_argument("--bmc-deadline-ms", type=int,
                    help="forward the existing BMC deadline (0 disables confirmation)")
    ap.add_argument("--bmc-solver-timeout-ms", type=int,
                    help="forward the existing positive per-query SMT timeout")
    args = ap.parse_args(argv)
    if args.timeout <= 0 or args.jobs <= 0 or args.limit < 0:
        ap.error("timeout/jobs must be positive and limit nonnegative")
    for name, minimum in (("bmc_deadline_ms", 0), ("bmc_solver_timeout_ms", 1)):
        value = getattr(args, name)
        if value is not None and not minimum <= value <= 4294967295:
            ap.error(f"--{name.replace('_', '-')} must be in [{minimum}, 4294967295]")

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

    records = []
    cancel_event = threading.Event()
    command = None
    summary = None
    manifest = {}
    evidence_dir = args.evidence_dir
    if evidence_dir:
        from ikos_runtime import runtime_identity, verify_runtime

        evidence_dir = evidence_dir.resolve()
        evidence_dir.mkdir(parents=True, exist_ok=False)
        raw_dir = evidence_dir / ".raw"
        raw_dir.mkdir()
        summary = dict(status="running", complete=False, tasks_planned=len(tasks),
                       tasks_collected=0, execution_checks_passed=False,
                       scope="Local same-run selected AR execution checks; not stage acceptance or official source-witness validation",
                       timing_scope="Local wall time with JSON reporting overhead; not official CPU/performance results",
                       options=dict(concurrency=args.concurrency, demote=args.demote,
                                    jobs=args.jobs, wall_timeout_seconds=args.timeout,
                                    bmc_deadline_ms=args.bmc_deadline_ms,
                                    bmc_solver_timeout_ms=args.bmc_solver_timeout_ms),
                       budget_note="null BMC options use the identified wrapper defaults")
        write_json(evidence_dir / "summary.json", summary)
        validator_files = [Path(__file__).resolve().parent / "scripts" / name for name in
                           ("sweep_bmc_execution.py", "validate_bmc_execution.py")]
        try:
            summary["runtime"] = runtime_identity(args.ikos)
            command = summary["runtime"]["command"]
            summary["validator_start"] = {p.name: file_sha256(p) for p in validator_files}
            summary["runner_sha256"] = file_sha256(__file__)
            for yml, src, expected, data_model in tasks:
                manifest[str(yml)] = dict(task=str(yml.relative_to(args.svbench)),
                                         input=str(src.resolve()), input_sha256=file_sha256(src),
                                         task_sha256=file_sha256(yml), expected=expected,
                                         data_model=data_model)
                key = hashlib.sha256(manifest[str(yml)]["task"].encode()).hexdigest()[:20]
                manifest[str(yml)]["raw_report"] = ".raw/" + key + ".json"
            summary["manifest_sha256"] = hashlib.sha256(json.dumps(
                list(manifest.values()), sort_keys=True).encode()).hexdigest()
            write_json(evidence_dir / "summary.json", summary)
        except (OSError, RuntimeError) as exc:
            summary.update(status="failed", failure=str(exc))
            write_json(evidence_dir / "summary.json", summary)
            print(str(exc), file=sys.stderr)
            return 2

    def work(t):
        yml, src, expected, data_model = t
        record = dict(manifest[str(yml)]) if evidence_dir else dict(
            input=str(src), expected=expected, data_model=data_model)
        with tempfile.TemporaryDirectory(prefix="ikos-race-") as temporary:
            folder = Path(temporary)
            report_path = evidence_dir / record["raw_report"] if evidence_dir else None
            try:
                verdict, elapsed = run_one(
                    src, args.ikos, args.timeout, folder / "output.db", args.concurrency,
                    data_model, args.demote, report_path=report_path,
                    bmc_deadline_ms=args.bmc_deadline_ms,
                    bmc_solver_timeout_ms=args.bmc_solver_timeout_ms,
                    command=command, cancel_event=cancel_event)
                record.update(verdict=verdict, elapsed_seconds=elapsed)
            except OSError as exc:
                record.update(verdict="Error (runner)", runner_error=str(exc))
        return record

    exit_code = 0
    def terminate(signum, frame):
        raise KeyboardInterrupt

    previous_sigterm = signal.signal(signal.SIGTERM, terminate)
    try:
        with ThreadPoolExecutor(max_workers=args.jobs) as ex:
            futs = {}
            try:
                for task in tasks:
                    futs[ex.submit(work, task)] = task
                for fut in as_completed(futs):
                    record = fut.result()
                    records.append(record)
                    path, expected, verdict = (record[k] for k in ("input", "expected", "verdict"))
                    tag = "" if verdict == expected else "  <-- MISMATCH"
                    print(f"RESULT {path} expected={expected} got={verdict}{tag}", file=sys.stderr)
                    if evidence_dir:
                        with (evidence_dir / "results.jsonl").open("a") as stream:
                            stream.write(json.dumps(record, sort_keys=True) + "\n")
            except BaseException:
                cancel_event.set()
                for future in futs:
                    future.cancel()
                raise
        if evidence_dir:
            summary["complete"] = len(records) == len(tasks)
            for yml, src, expected, data_model in tasks:
                item = manifest[str(yml)]
                if file_sha256(src) != item["input_sha256"] or file_sha256(yml) != item["task_sha256"]:
                    raise RuntimeError(f"task/input changed during scan: {yml}")
            from sweep_bmc_execution import validate_report
            # Parsing, hashing, compression and replay start after all analyzers.
            for record in records:
                report_path = evidence_dir / record["raw_report"]
                record.update(collect_report(report_path, record["verdict"],
                                             evidence_dir, report_path.stem))
                if record.get("evidence"):
                    artifact = evidence_dir / record["evidence"]
                    if file_sha256(artifact) != record["evidence_sha256"]:
                        raise RuntimeError(f"selected evidence changed: {artifact.name}")
                    with gzip.open(artifact, "rt", encoding="utf-8") as stream:
                        selected = json.load(stream)
                    record["executions"] = validate_report(selected)
                    for check in record["executions"]:
                        check["group"] = selected["reports"][check["group"]]["original_group"]
            summary["validator_end"] = {p.name: file_sha256(p) for p in validator_files}
            if summary["validator_start"] != summary["validator_end"]:
                raise RuntimeError("execution validator changed during scan/checks")
            verify_runtime(summary["runtime"])
            summary["runtime_unchanged"] = True
            races = [r for r in records if r["verdict"] == VERDICT_RACE]
            summary["false_tasks"] = len(races)
            summary["selected_executions"] = sum(len(r["executions"]) for r in races)
            summary["execution_checks_passed"] = all(
                r.get("report_status") == "parsed" and r["executions"] and
                all(check["status"] == "ACCEPT" for check in r["executions"]) for r in races)
            summary["reports_complete"] = all(r.get("report_status") == "parsed" for r in records
                                               if not r["verdict"].startswith("Error"))
            summary["status"] = "completed"
            if not tasks or not summary["execution_checks_passed"] or not summary["reports_complete"]:
                summary["status"] = "failed"
                exit_code = 2
            if summary["status"] == "completed":
                for record in records:
                    (evidence_dir / record["raw_report"]).unlink(missing_ok=True)
                    record.pop("raw_report")
                raw_dir.rmdir()
    except KeyboardInterrupt:
        exit_code = 130
        if summary is not None:
            summary.update(status="interrupted", execution_checks_passed=False,
                           failure="interrupted before evidence collection/checks finished")
    except Exception as exc:
        if not evidence_dir:
            raise
        exit_code = 2
        summary.update(status="failed", execution_checks_passed=False, failure=str(exc))
        print(str(exc), file=sys.stderr)
    finally:
        signal.signal(signal.SIGTERM, previous_sigterm)
        if evidence_dir:
            summary["tasks_collected"] = len(records)
            summary["validator_end"] = {p.name: file_sha256(p) if p.is_file() else None
                                        for p in validator_files}
            if summary.get("validator_start") != summary["validator_end"]:
                summary.update(status="failed", execution_checks_passed=False,
                               failure="execution validator changed during scan/checks")
                exit_code = 2
            summary["counts"] = confusion([(r["input"], r["expected"], r["data_model"], r["verdict"]) for r in records])
            write_json(evidence_dir / "results.json", sorted(records, key=lambda r: r["task"]))
            write_json(evidence_dir / "summary.json", summary)

    results = [(r["input"], r["expected"], r["data_model"], r["verdict"]) for r in records]

    total = confusion(results)
    ilp32 = confusion([r for r in results if r[2] == "ILP32"])
    lp64 = confusion([r for r in results if r[2] == "LP64"])
    for label, m in [("TOTAL", total), ("ILP32 (32-bit)", ilp32),
                     ("LP64 (64-bit)", lp64)]:
        n = m["tasks"]
        print(f"\n=== IKOS no-data-race [{label}] ({n} tasks) ===")
        print(f"  TP={m['tp']} FP={m['fp']} TN={m['tn']} FN={m['fn']} "
              f"err={m['err']} unknown={m['unknown']}")
        print(f"  Precision={m['precision']:.3f}  "
              f"Race coverage={m['tp']}/{m['race_population']} ({m['race_coverage']:.3f})  "
              f"Safe coverage={m['tn']}/{m['safe_population']} ({m['safe_coverage']:.3f})")

    if args.only_fail:
        for path, expected, dm, v in results:
            if v != expected:
                print(f"  {expected}->{v}  {path}")
    return exit_code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
