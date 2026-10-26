#!/usr/bin/env python3
"""Check exact later access targets and legacy helper targets with strict replay."""
import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "scripts"))
from analyzer.python.ikos.race_result import validate_race_result
from ikos_runtime import runtime_command
from validate_bmc_execution import check, digest_json

POSITIVE = {0, 4, 5, 7, 8, 9, 10, 12, 14}
# Direct accesses, actual helper invocation changes, atomic tails, main/worker
# and completion modes. No filename or fixture label enters production search.
CASES = [(0, 1, 1, False), (0, 2, 2, False), (0, 2, 3, False),
         (0, 3, 2, True), (4, 3, 2, False), (4, 2, 3, True),
         (5, 2, 3, False), (7, 3, 2, False), (8, 2, 3, False),
         (9, 2, 2, False), (10, 3, 1, False), (12, 3, 3, False),
         (14, 3, 2, False)]
CASES += [(case, 2, 2, False) for case in (1, 2, 3, 6, 11, 13, 15)]


def target_details(program, certificate):
    traces = {trace["id"]: trace for trace in certificate["traces"]}
    result = []
    for target in certificate["targets"]:
        trace = traces[target["trace"]]
        index = target["step"]
        step = trace["steps"][index]
        assert program["instructions"][step["statement"]]["kind"] in ("load", "store")
        ordinal = sum(prior["statement"] == step["statement"] and
                      "synthetic_event" not in prior
                      for prior in trace["steps"][:index + 1])
        result.append((trace["id"], trace["entry"], index, ordinal))
    return result


def omit_unreferenced_step(certificate, trace_id, removed):
    """Delete an actual step while preserving every surviving event reference."""
    changed = copy.deepcopy(certificate)
    original_traces = {trace["id"]: trace for trace in certificate["traces"]}
    changed_traces = {trace["id"]: trace for trace in changed["traces"]}
    del changed_traces[trace_id]["steps"][removed]
    references = 0

    def remap_event(owner, trace_key, step_key):
        nonlocal references
        target_trace, target_step = owner[trace_key], owner[step_key]
        assert target_trace in original_traces
        assert 0 <= target_step < len(original_traces[target_trace]["steps"])
        if target_trace == trace_id:
            # This mutation omits execution, not a demanded writer or creator.
            assert target_step != removed, ("removed step is referenced", target_trace, removed)
            owner[step_key] -= target_step > removed
        updated = owner[step_key]
        assert 0 <= updated < len(changed_traces[target_trace]["steps"])
        assert (changed_traces[target_trace]["steps"][updated]["statement"] ==
                original_traces[target_trace]["steps"][target_step]["statement"])
        references += 1

    def visit(value):
        if isinstance(value, dict):
            if "trace" in value and "step" in value:
                remap_event(value, "trace", "step")
            if "creator_trace" in value and "creator_step" in value:
                remap_event(value, "creator_trace", "creator_step")
            for child in value.values():
                visit(child)
        elif isinstance(value, list):
            for child in value:
                visit(child)

    visit(changed)
    assert references >= 2, "target references were not validated"
    return changed


def validate_execution(execution, case, a_count, b_count):
    certificate = copy.deepcopy(execution)
    program = certificate.pop("program")
    certificate["program_sha256"] = digest_json(program)
    accepted = check(program, certificate)
    assert accepted["status"] == "ACCEPT", accepted
    details = target_details(program, certificate)
    if case == 12:
        assert sorted(row[3] for row in details) == [1, 3], details
        traces = {trace["id"]: trace for trace in certificate["traces"]}
        selected = [traces[row[0]] for row in details]
        assert all(trace["entry"] == "worker_same" for trace in selected)
        assert len({(t["creator_trace"], t["creator_step"]) for t in selected}) == 2
        creator = traces[selected[0]["creator_trace"]]
        creates = []
        for i, step in enumerate(creator["steps"]):
            inst = program["instructions"][step["statement"]]
            if (inst["kind"] == "call" and inst.get("operands") and
                    inst["operands"][0].get("kind") == "function" and
                    inst["operands"][0].get("id") in ("pthread_create", "ar.pthread.create")):
                creates.append(i)
        assert len(creates) == 3, creates
        assert {trace["creator_step"] for trace in selected} == {creates[0], creates[2]}
    else:
        expected = {"worker_left": a_count, "worker_right": b_count}
        if case in (7, 8, 9):
            expected["main"] = b_count
        elif case == 10:
            expected = {"main": 3, "worker_left": 1}
        assert all(ordinal == expected[entry] for _, entry, _, ordinal in details), (
            case, details, expected)
    checks = 1
    def reject(broken, description):
        nonlocal checks
        result = check(program, broken)
        assert result["status"] == "REJECT", (case, description, result)
        checks += 1
        return result

    traces = {trace["id"]: trace for trace in certificate["traces"]}
    # Changing a target to another genuine race can be valid. These mutations
    # instead select an earlier occurrence known to access a disjoint cell.
    for trace_id, _, index, ordinal in details:
        if ordinal <= 1:
            continue
        statement = traces[trace_id]["steps"][index]["statement"]
        earlier = next(i for i, step in enumerate(traces[trace_id]["steps"][:index])
                       if step["statement"] == statement and "synthetic_event" not in step)
        broken = copy.deepcopy(certificate)
        next(t for t in broken["targets"] if t["trace"] == trace_id)["step"] = earlier
        reject(broken, "earlier disjoint target")
        broken = omit_unreferenced_step(certificate, trace_id, earlier)
        rejected = reject(broken, "omitted earlier memory occurrence with valid references")
        assert rejected["reason"] == "wrong function/statement occurrence", rejected
    for trace in certificate["traces"]:
        if trace["entry"] == "main":
            continue
        broken = copy.deepcopy(certificate)
        changed = next(t for t in broken["traces"] if t["id"] == trace["id"])
        changed["creator_step"] += 1
        reject(broken, "wrong dynamic create step")
        break
    value_mutated = source_mutated = False
    for trace in certificate["traces"]:
        for i, step in enumerate(trace["steps"]):
            inst = program["instructions"][step["statement"]]
            if (not value_mutated and "encoded_result" in step and
                    inst["kind"] in ("load", "binary", "unary")):
                broken = copy.deepcopy(certificate)
                changed = next(t for t in broken["traces"] if t["id"] == trace["id"])
                changed["steps"][i]["encoded_result"] = str(int(step["encoded_result"]) + 1)
                reject(broken, "wrong executed value")
                value_mutated = True
            if not source_mutated and step.get("read_from"):
                broken = copy.deepcopy(certificate)
                changed = next(t for t in broken["traces"] if t["id"] == trace["id"])
                changed["steps"][i]["read_from"] = [
                    {"trace": trace["id"], "step": i}] * len(step["read_from"])
                reject(broken, "read from the load itself")
                source_mutated = True
    assert value_mutated, (case, "missing demanded-value corruption")
    if case not in (12,):
        assert source_mutated, (case, "missing source corruption")
    if case == 14:
        # Two consecutive startup stores and the competing write all have the
        # same value. Only the later startup store (or the actual worker write)
        # can supply the target read: the earlier store is always overwritten.
        read_target = next(target for target in certificate["targets"]
                           if program["instructions"][traces[target["trace"]]["steps"]
                              [target["step"]]["statement"]]["kind"] == "load")
        read = traces[read_target["trace"]]["steps"][read_target["step"]]
        if "encoded_result" in read:
            assert int(read["encoded_result"]) == a_count, read
        overwritten = []
        for index, step in enumerate(traces["startup"]["steps"]):
            inst = program["instructions"][step["statement"]]
            if (inst["kind"] == "store" and not inst.get("abstract_initializer") and
                    step.get("encoded_address") == read.get("encoded_address") and
                    inst["operands"][1].get("kind") == "integer" and
                    int(inst["operands"][1]["value"]) == a_count):
                overwritten.append(index)
        assert len(overwritten) == 2, (overwritten, read)
        sources = [{"trace": "startup", "step": overwritten[0]}] * len(read["read_from"])
        assert sources != read["read_from"]
        broken = copy.deepcopy(certificate)
        next(t for t in broken["traces"] if t["id"] == read_target["trace"])[
            "steps"][read_target["step"]]["read_from"] = sources
        rejected = reject(broken, "same-valued overwritten actual write as read source")
        assert rejected["reason"] == "SMT read source differs from actual last write", rejected
    if case in (5, 9):
        for trace in certificate["traces"]:
            if trace["id"] == "startup":
                continue
            assert program["instructions"][trace["steps"][-1]["statement"]]["kind"] == "return"
            broken = copy.deepcopy(certificate)
            next(t for t in broken["traces"] if t["id"] == trace["id"])["steps"].pop()
            reject(broken, "missing atomic root or auxiliary Return")
    if case in (8, 9):
        auxiliary = [t for t in certificate["traces"] if t["id"].startswith("aux_")]
        assert auxiliary, (case, "completion path did not retain the joined auxiliary")
        broken = copy.deepcopy(certificate)
        next(t for t in broken["traces"] if t["id"] == auxiliary[0]["id"])["steps"].pop()
        reject(broken, "missing actual auxiliary Return")
        broken = copy.deepcopy(certificate)
        broken["traces"] = [t for t in broken["traces"] if t["id"] != auxiliary[0]["id"]]
        reject(broken, "missing joined auxiliary trace")
    if case == 4:
        found = False
        for trace in certificate["traces"]:
            for i, step in enumerate(trace["steps"][:-1]):
                if program["instructions"][step["statement"]]["kind"] == "return":
                    broken = omit_unreferenced_step(certificate, trace["id"], i)
                    rejected = reject(broken, "missing earlier helper Return with valid references")
                    assert rejected["reason"] == "wrong function/statement occurrence", rejected
                    found = True
                    break
            if found:
                break
        assert found, "helper invocation boundary not retained"
    return checks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ikos", type=Path, default=ROOT / "install/bin/ikos")
    parser.add_argument("--bits", choices=("32", "64", "both"), default="both")
    parser.add_argument("--case", type=int, action="append", help="Run only these fixture cases")
    args = parser.parse_args()
    fixture = (ROOT / "analyzer/test/regression/bmc-correctness/later-accesses.c").read_text()
    total = 0
    with tempfile.TemporaryDirectory(prefix="ikos-later-accesses-") as temp:
        work = Path(temp)
        for bits in (("32", "64") if args.bits == "both" else (args.bits,)):
            for case, a_count, b_count, wide in CASES:
                if args.case and case not in args.case:
                    continue
                source, report = work / "input.c", work / "report.json"
                source.write_text(f"#define CASE {case}\n#define A_COUNT {a_count}\n"
                                  f"#define B_COUNT {b_count}\n" +
                                  ("#define WIDE\n" if wide else "") + fixture)
                report.unlink(missing_ok=True)
                command = runtime_command(args.ikos.resolve()) + [
                    "--analyses=race", "--concurrency=auto",
                    "--partitioning=guards", "--demote-race-to-unknown", "-m" + bits,
                    "--bmc-deadline-ms=20000", "--bmc-solver-timeout-ms=1000",
                    "--format=json", "--report-file=" + str(report),
                    "-o", str(work / "output.db"), str(source)]
                environment = dict(os.environ, IKOS_BMC_RECOVER="1", IKOS_BMC_DIAGNOSTICS="1")
                process = subprocess.run(command,
                    cwd=work, env=environment,
                    capture_output=True, text=True, timeout=60)
                assert process.returncode == 0 and report.is_file(), (
                    case, bits, process.stdout, process.stderr)
                data = json.loads(report.read_text())
                metadata = validate_race_result(data)
                executions = [row["info"]["counterexample"] for row in data["reports"]
                              if "counterexample" in row.get("info", {})]
                expected = case in POSITIVE
                assert (metadata["verdict"] == "FALSE") == expected and bool(executions) == expected, (
                    case, bits, a_count, b_count, metadata, process.stdout, process.stderr)
                if expected:
                    for execution in executions:
                        total += validate_execution(execution, case, a_count, b_count)
                    if case == 0 and max(a_count, b_count) > 1:
                        counts = [row.get("info", {}).get("bmc", {}).get("counts", {})
                                  for row in data["reports"]]
                        assert any(c.get("later_access_confirmed", 0) > 0 for c in counts), counts
                total += 1
                print(f"later-access case={case} bits={bits} A={a_count} B={b_count} "
                      f"wide={wide}: {metadata['verdict']}", flush=True)
    print(f"later-access: {total} analyzer/replay assertions passed")


if __name__ == "__main__":
    main()
