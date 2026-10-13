#!/usr/bin/env python3
"""Check concrete create occurrences, argument identity, and damaged evidence."""

import argparse
from collections import Counter
import copy
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "scripts"))
import svcomp_race_test as suite
from sweep_bmc_execution import validate_report
from validate_bmc_execution import check, digest_json

RACY = {0, 2, 4, 8, 9, 11, 14, 16, 18}
INVALID_PREFIX = {12, 13, 15}
MISALIGNED_BASELINE = "dd68ce473f87f9c8def006d369be48b639edfca7b442a416077d5568f4adb9a2"
NAMES = (
    "second-argument", "different-entries-disjoint", "same-entry-same-argument",
    "same-entry-disjoint", "dormant-unsupported-child", "exclusive-creates",
    "single-instance", "common-lock", "first-and-third", "second-and-third",
    "unreachable-second-create", "array-output-loop", "out-of-bounds-output",
    "misaligned-output", "bounded-index-output", "invalid-dormant-output",
    "loop-distinct-arguments", "loop-disjoint-control",
    "nested-typed-output",
)


def wrong_creator(report):
    """Rebind a selected later instance to an actual earlier create of its entry."""
    results = []
    for row in report.get("reports", []):
        info = row["info"]
        replay = info.get("counterexample")
        if replay is None:
            continue
        startup = next((t for t in replay["traces"] if t["id"] == "startup"), None)
        if startup is None:
            continue
        program = replay["program"]
        for trace in replay["traces"]:
            if trace.get("creator_trace") != "startup":
                continue
            selected = trace["creator_step"]
            for index, step in enumerate(startup["steps"][:selected]):
                inst = program["instructions"][step["statement"]]
                operands = inst.get("operands", [])
                if not (inst["kind"] == "call" and len(operands) == 5 and
                        operands[0].get("id") in ("pthread_create", "ar.pthread.create") and
                        operands[3].get("id") == trace["entry"]):
                    continue
                # In case 0 the earlier A is passed &x, the selected A gets &y.
                certificate = copy.deepcopy(replay)
                original = certificate.pop("program")
                certificate["program_sha256"] = digest_json(original)
                child = next(t for t in certificate["traces"] if t["id"] == trace["id"])
                child["creator_step"] = index
                result = check(original, certificate)
                results.append(dict(trace=trace["id"], entry=trace["entry"],
                                    from_step=selected, to_step=index, **result))
                assert result["status"] == "REJECT", result
                return results
    return results


def damaged_output(report):
    """New output metadata must agree with the address actually evaluated in AR."""
    for row in report.get("reports", []):
        replay = row["info"].get("counterexample")
        if replay is None:
            continue
        for trace in replay["traces"]:
            for index, step in enumerate(trace["steps"]):
                inst = replay["program"]["instructions"][step["statement"]]
                operands = inst.get("operands", [])
                if not ("encoded_address" in step and inst["kind"] == "call" and
                        operands and operands[0].get("id") in ("pthread_create", "ar.pthread.create")):
                    continue
                results = []
                for field in ("offset", "width", "alignment", "object"):
                    certificate = copy.deepcopy(replay)
                    program = certificate.pop("program")
                    certificate["program_sha256"] = digest_json(program)
                    changed = next(t for t in certificate["traces"] if t["id"] == trace["id"])["steps"][index]
                    address = changed["encoded_address"]
                    if field == "object":
                        address["object"]["id"] += "-wrong-output-object"
                    else:
                        address[field] = int(address[field]) + 1
                    result = check(program, certificate)
                    results.append(dict(trace=trace["id"], step=index, field=field, **result))
                    assert result["status"] == "REJECT", result
                return results
    return []


def run(args, case, variant, bits, mode):
    folder = args.output / f"{bits}-{case}-{variant}-{mode}"
    folder.mkdir()
    source = Path(__file__).with_name("instances.c").resolve()
    env = dict(os.environ)
    for key in ("IKOS_BMC_RECOVER", "IKOS_BMC_DIAGNOSTICS", "IKOS_BMC_DISABLE_AI_FACTS",
                "IKOS_BMC_DISABLE_AI_BOUNDS", "IKOS_BMC_DISABLE_AI_NO_WRAP",
                "IKOS_BMC_DISABLE_AI_SINGLE_EXECUTION"):
        env.pop(key, None)
    if mode == "bmc":
        env.update(IKOS_BMC_RECOVER="1", IKOS_BMC_DIAGNOSTICS="1")
    report_path = folder / "report.json"
    command = [str(args.ikos.resolve()), "--analyses=race", "--concurrency=auto",
               "--demote-race-to-unknown", "-m", str(bits), f"-DCASE={case}",
               f"-DVARIANT={variant}", "--format=json", "--report-file=" + str(report_path),
               "-o", str(folder / "output.db"), str(source)]
    process = subprocess.Popen(command, cwd=folder, env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, start_new_session=True)
    timed_out = False
    try:
        output, _ = process.communicate(timeout=30)
    except subprocess.TimeoutExpired:
        timed_out = True
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        output, _ = process.communicate()
    text = output.decode(errors="replace")
    (folder / "run.log").write_text(text)
    verdict = suite.classify(text, process.returncode, timed_out)
    report = json.loads(report_path.read_text()) if report_path.exists() else {}
    report["reports"] = [r for r in report.get("reports", []) if isinstance(r.get("info"), dict)]
    executions = validate_report(report)
    diagnostics = Counter()
    for group in report["reports"]:
        diagnostics.update(group["info"].get("bmc", {}).get("counts", {}))
    mutations = wrong_creator(report) if case in (0, 16) and verdict == suite.VERDICT_RACE else []
    output_mutations = damaged_output(report) if case in (11, 14, 16, 18) and verdict == suite.VERDICT_RACE else []
    known_baseline_violation = (args.phase == "baseline" and args.analyzer_sha256 == MISALIGNED_BASELINE and
                                case == 13 and mode == "bmc" and
                                verdict == suite.VERDICT_RACE)
    row = dict(case=case, name=NAMES[case], variant=variant, bits=bits, mode=mode,
               verdict=verdict, returncode=process.returncode, timed_out=timed_out,
               expected=None if case in INVALID_PREFIX else "Race Found" if case in RACY else "Safe", command=command,
               diagnostics=dict(diagnostics), executions=executions, mutations=mutations,
               output_mutations=output_mutations, known_baseline_violation=known_baseline_violation)
    (folder / "result.json").write_text(json.dumps(row, indent=2) + "\n")
    print(folder.name, NAMES[case], verdict, flush=True)
    assert process.returncode == 0 and not timed_out, row
    assert verdict in (suite.VERDICT_RACE, suite.VERDICT_SAFE, suite.VERDICT_UNKNOWN), row
    if not known_baseline_violation:
        assert verdict != (suite.VERDICT_SAFE if case in RACY else suite.VERDICT_RACE), row
    if verdict == suite.VERDICT_RACE:
        if known_baseline_violation:
            assert executions and all(e["status"] == "REJECT" and e["reason"] == "misaligned access" for e in executions), row
        else:
            assert executions and all(e["status"] == "ACCEPT" for e in executions), row
    if mode == "bmc" and case in RACY - {14, 16, 18} and (args.phase == "candidate" or case == 2):
        assert verdict == suite.VERDICT_RACE, row
    if args.phase == "candidate" and mode == "bmc" and case == 0:
        assert mutations, "No later selected create available for argument-identity mutation"
    if args.phase == "candidate" and mode == "bmc" and case in (11, 14, 16, 18) and verdict == suite.VERDICT_RACE:
        assert output_mutations, "No independently bound create output metadata"
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ikos", type=Path, default=ROOT / "install/bin/ikos")
    parser.add_argument("--analyzer", type=Path, default=ROOT / "install/bin/ikos-analyzer")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--phase", choices=("baseline", "candidate"), default="candidate")
    args = parser.parse_args()
    args.analyzer_sha256 = hashlib.sha256(args.analyzer.read_bytes()).hexdigest()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    rows = []
    for bits in (32, 64):
        for case, variant in [(c, 0) for c in range(len(NAMES))] + [(0, 1)]:
            for mode in ("ai", "bmc"):
                rows.append(run(args, case, variant, bits, mode))
    assert hashlib.sha256(args.analyzer.read_bytes()).hexdigest() == args.analyzer_sha256, "Analyzer changed during regression"
    (args.output / "results.json").write_text(json.dumps(rows, indent=2) + "\n")
    summary = dict(runs=len(rows), phase=args.phase,
                   analyzer_sha256=args.analyzer_sha256,
                   source_sha256=hashlib.sha256(Path(__file__).with_name("instances.c").read_bytes()).hexdigest(),
                   verdicts=dict(Counter(r["verdict"] for r in rows)),
                   executions=sum(len(r["executions"]) for r in rows),
                   rejected_mutations=sum(len(r["mutations"]) for r in rows),
                   rejected_output_mutations=sum(len(r["output_mutations"]) for r in rows),
                   known_baseline_violations=sum(r["known_baseline_violation"] for r in rows),
                   scope="Scalar and typed main stack-array handles; candidate FALSE must be AR ACCEPT; old misaligned-output rejection recorded explicitly; no source witness claim")
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
