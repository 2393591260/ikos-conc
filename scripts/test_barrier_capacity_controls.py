#!/usr/bin/env python3
"""Review-only capacity-one/two barrier controls; integration runs are required."""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

FALSE = {0, 1, 3, 5, 14, 15, 16, 18, 19}
SAFE = {2, 4}
CASES = tuple(range(21))


def save_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


def named_call(inst, name):
    operands = inst.get("operands", [])
    return (inst.get("kind") == "call" and operands and
            operands[0].get("kind") == "function" and operands[0].get("id") == name)


def checked(check, program, certificate, expected, description, reason=None):
    result = check(program, certificate)
    assert result["status"] == expected, (description, result)
    if reason is not None:
        assert result.get("reason") == reason, (description, result)
    return result


def count_two_program(program):
    changed = copy.deepcopy(program)
    inits = [i for i in changed["instructions"].values()
             if named_call(i, "pthread_barrier_init")]
    assert len(inits) == 1
    arg = inits[0]["operands"][3]
    inits[0]["operands"][3] = dict(kind="integer", value="2", type=arg["type"])
    return changed


def append_actual_worker_wait(program, trace):
    """Extend a straight-line actual worker CFG; never invent its future call."""
    instructions = program["instructions"]
    blocks = program["functions"][trace["entry"]]["blocks"]
    assert trace["steps"] and "synthetic_event" not in trace["steps"][-1]
    last = trace["steps"][-1]["statement"]
    block = instructions[last]["block"]
    assert block in blocks
    pc = blocks[block]["statements"].index(last) + 1
    appended = []
    visited_empty = set()
    for _ in range(128):
        current = blocks[block]
        if pc == len(current["statements"]):
            successors = current["successors"]
            assert len(successors) == 1, ("worker suffix is not straight-line", block)
            block, pc = successors[0], 0
            assert (block, pc) not in visited_empty, "worker suffix cycle"
            visited_empty.add((block, pc))
            continue
        sid = current["statements"][pc]
        pc += 1
        inst = instructions[sid]
        assert inst["kind"] != "return", "worker returned before its genuine wait"
        step = dict(statement=sid, clock="0")
        trace["steps"].append(step)
        appended.append(sid)
        if named_call(inst, "pthread_barrier_wait"):
            trace["steps"].append(dict(statement=sid, clock="0",
                                       synthetic_event="barrier_departure"))
            return appended
        # Fixture has no helper call or branch-dependent choice in this suffix.
        # New values are independently evaluated by replay, never guessed here.
        assert inst["kind"] in {"assign", "unary", "binary", "pointer_shift",
                                "compare", "load", "store", "allocate"}, (
                                    "unsupported actual worker suffix", sid, inst)
    raise AssertionError("worker suffix exceeds review bound")


def verify_overlapping_generations(program, cert, check, digest_json, source, folder):
    """Two real cap1 arrivals occur before either generation's first departure."""
    extended = copy.deepcopy(cert)
    traces = {t["id"]: t for t in extended["traces"]}
    assert set(traces) == {"a", "b"}
    main = next(t for t in traces.values() if t["entry"] == "main")
    child = next(t for t in traces.values() if t["entry"] != "main")
    main_positions = [i for i, s in enumerate(main["steps"])
                      if s.get("synthetic_event") == "barrier_departure"]
    assert len(main_positions) == 1
    dep = main_positions[0]
    assert dep > 0 and named_call(
        program["instructions"][main["steps"][dep - 1]["statement"]],
        "pthread_barrier_wait")
    assert not any(s.get("synthetic_event") == "barrier_departure" for s in child["steps"])
    child_target = next(t for t in extended["targets"] if t["trace"] == child["id"])
    main_target = next(t for t in extended["targets"] if t["trace"] == main["id"])
    assert child_target["step"] == len(child["steps"]) - 1
    assert main_target["step"] >= dep + 1
    assert all(program["instructions"][s["statement"]]["kind"] != "return"
               for s in main["steps"])
    appended = append_actual_worker_wait(program, child)
    assert child["steps"][-1].get("synthetic_event") == "barrier_departure"
    sequence = (main["steps"][:dep] + child["steps"][:-1] +
                main["steps"][dep:] + child["steps"][-1:])
    assert len(sequence) == sum(len(t["steps"]) for t in traces.values())
    for clock, step in enumerate(sequence):
        step["clock"] = str(clock)
    accepted = checked(check, program, extended, "ACCEPT", "cap1 overlapping generations")
    # Same schedule, separate trusted-program oracle: cap2 orders child-pre/main-post.
    cap2 = count_two_program(program)
    contrasted = copy.deepcopy(extended)
    contrasted["program_sha256"] = digest_json(cap2)
    rejected = checked(check, cap2, contrasted, "REJECT",
                       "cap2 pre/post ordering contrast",
                       "targets ordered by pthread synchronization")
    save_json(folder / "cap1-overlap-certificate.json", extended)
    save_json(folder / "cap2-contrast-program.json", cap2)
    save_json(folder / "cap2-contrast-certificate.json", contrasted)
    raw = source.read_text()
    initializer = "int capacity = 1;"
    assert raw.count(initializer) == 1
    variant = raw.replace(initializer, "int capacity = 2;", 1)
    variant_path = folder / "cap2-contrast-source-illustration.c"
    variant_path.write_text(variant)
    save_json(folder / "generation-oracle.json", dict(
        original_input_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
        original_program_sha256=cert["program_sha256"],
        appended_real_worker_statements=appended,
        targets=extended["targets"],
        cap1=accepted, cap2=rejected,
        cap2_program_sha256=contrasted["program_sha256"],
        cap2_source_illustration_sha256=hashlib.sha256(variant_path.read_bytes()).hexdigest(),
        cap2_identity_scope="Constructed trusted-AR oracle and illustrative changed source; "
                            "not a reimported analyzer report or source-witness acceptance."))
    return 2


def verify(info, check, digest_json, case, source, bits, ikos, folder,
           verify_source_binding, build_witness, WitnessProjectionUnsupported):
    cert = copy.deepcopy(info["counterexample"])
    program = cert.pop("program")
    cert["program_sha256"] = digest_json(program)
    checked(check, program, cert, "ACCEPT", "original selected execution")
    folder.mkdir()
    save_json(folder / "original-program.json", program)
    save_json(folder / "original-certificate.json", cert)
    count = 1 + verify_source_binding(program, cert, source, "barrier-capacity-" + str(case))
    # This uses the new actual report and its real generated input. Only a
    # declared projection limit is caught; corrupt binding is a hard failure.
    projection = {}
    try:
        witness = build_witness(str(source), info, "ILP32" if bits == 32 else "LP64", str(ikos))
        assert witness
        save_json(folder / "source-witness-projection.json", witness)
        projection = dict(status="PROJECTED", official_source_validation=False)
    except WitnessProjectionUnsupported as exc:
        projection = dict(status="UNSUPPORTED", reason=str(exc), official_source_validation=False)
    save_json(folder / "source-binding.json", dict(
        input=str(source.resolve()),
        input_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
        program_sha256=cert["program_sha256"],
        cli_actual_input="ACCEPT", cli_corrupt_input_hash="REJECT",
        projection=projection))
    count += 1
    departures = [(t["id"], i) for t in cert["traces"]
                  for i, s in enumerate(t["steps"])
                  if s.get("synthetic_event") == "barrier_departure"]
    if case in (0, 1, 3, 5, 18, 19):
        assert len(departures) == 1, departures
        altered = count_two_program(program)
        damaged = copy.deepcopy(cert)
        damaged["program_sha256"] = digest_json(altered)
        checked(check, altered, damaged, "REJECT", "cap2 missing real arrival",
                "barrier departure before all arrivals")
        save_json(folder / "cap2-one-arrival-program.json", altered)
        save_json(folder / "cap2-one-arrival-certificate.json", damaged)
        count += 1
    if case in (0, 5, 18, 19):
        # Change the actual source nondet, not an invented count annotation.
        # Remove derived value assertions so replay must recompute them itself.
        changed = copy.deepcopy(cert)
        choices = []
        for trace in changed["traces"]:
            for step in trace["steps"]:
                inst = program["instructions"][step["statement"]]
                if named_call(inst, "__VERIFIER_nondet_int"):
                    choices.append(step)
                else:
                    step.pop("encoded_result", None)
        assert len(choices) == 1 and int(choices[0]["encoded_result"]) == 1
        choices[0]["encoded_result"] = "2"
        checked(check, program, changed, "REJECT", "actual nondet count2 missing arrival",
                "barrier departure before all arrivals")
        save_json(folder / "actual-source-count2-certificate.json", changed)
        count += 1
    for name, pos in departures:
        bad = copy.deepcopy(cert)
        trace = next(t for t in bad["traces"] if t["id"] == name)
        trace["steps"].pop(pos)
        checked(check, program, bad, "REJECT", "missing departure")
        bad = copy.deepcopy(cert)
        trace = next(t for t in bad["traces"] if t["id"] == name)
        trace["steps"][pos]["encoded_result"] = "0"
        checked(check, program, bad, "REJECT", "fabricated departure API result")
        count += 2
    if case == 1:
        count += verify_overlapping_generations(program, cert, check, digest_json, source, folder)
    if case == 15:
        assert len(departures) == 2
        ordered = copy.deepcopy(cert)
        main = next(t for t in ordered["traces"] if t["entry"] == "main")
        child = next(t for t in ordered["traces"] if t["entry"] != "main")
        assert not any(program["instructions"][s["statement"]]["kind"] == "return"
                       for s in main["steps"])
        for clock, step in enumerate(main["steps"] + child["steps"]):
            step["clock"] = str(clock)
        checked(check, program, ordered, "ACCEPT", "cap1 separated generations post/post")
        save_json(folder / "cap1-serialized-generations-certificate.json", ordered)
        count += 1
    return count, projection


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--repo", type=Path, default=Path("/home/ruan/ikos-conc"))
    p.add_argument("--ikos", type=Path, required=True)
    p.add_argument("--case", type=int, choices=CASES, action="append")
    p.add_argument("--bits", type=int, choices=(32, 64), action="append")
    p.add_argument("--evidence-dir", type=Path)
    p.add_argument("--fixture", type=Path)
    a = p.parse_args()
    sys.path[:0] = [str(a.repo), str(a.repo / "scripts")]
    from ikos_runtime import runtime_command, runtime_identity, verify_runtime
    from validate_bmc_execution import check, digest_json
    from analyzer.python.ikos.race_result import validate_race_result
    from test_bmc_startup_provider import verify_source_binding
    from svcomp_witness import build_witness, WitnessProjectionUnsupported
    identity = runtime_identity(a.ikos)
    checker_files = [a.repo / "scripts" / "validate_bmc_execution.py",
                     a.repo / "scripts" / "test_bmc_startup_provider.py",
                     a.repo / "svcomp_witness.py"]
    checker_hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in checker_files}
    fixture = a.fixture or Path(__file__).with_name("barrier-capacity-controls.c")
    if not fixture.is_file() and a.fixture is None:
        fixture = a.repo / "analyzer/test/regression/bmc-values/barrier-capacity-controls.c"
    body = fixture.read_text()
    env = {k: v for k, v in os.environ.items()
           if not k.startswith("IKOS_") and k not in ("PYTHONHOME", "PYTHONPATH")}
    env["IKOS_BMC_RECOVER"] = "1"
    temp = tempfile.TemporaryDirectory(prefix="ikos-barrier-capacity-draft-") if a.evidence_dir is None else None
    folder = a.evidence_dir or Path(temp.name)
    folder.mkdir(parents=True, exist_ok=True)
    checks, rows = 0, []
    save_json(folder / "identity-start.json", dict(runtime=identity, checker_files=checker_hashes))
    for bits in a.bits or (32, 64):
        for wide in (False, True):
            for case in a.case or CASES:
                work = folder / f"case{case}-{bits}-wide{int(wide)}"
                work.mkdir()
                source = work / "renamed.c"
                source.write_text(f"#define CASE {case}\n" +
                                  ("#define WIDE\n" if wide else "") + body)
                report = work / "report.json"
                command = runtime_command(a.ikos) + [
                    "--analyses=race", "--concurrency=auto", "--partitioning=guards",
                    "--demote-race-to-unknown", "--bmc-deadline-ms=20000",
                    "--bmc-solver-timeout-ms=1000", f"-m{bits}", "--format=json",
                    "--report-file=" + str(report), "-o", str(work / "out.db"), str(source)]
                save_json(work / "command.json", dict(command=command,
                    environment={"IKOS_BMC_RECOVER": "1"},
                    input_sha256=hashlib.sha256(source.read_bytes()).hexdigest()))
                proc = subprocess.run(command, env=env, capture_output=True, text=True, timeout=60)
                (work / "stdout.txt").write_text(proc.stdout)
                (work / "stderr.txt").write_text(proc.stderr)
                assert proc.returncode == 0, (case, bits, wide, proc.stderr)
                data = json.loads(report.read_text())
                metadata = validate_race_result(data)
                infos = [r.get("info", {}) for r in data.get("reports", [])
                         if r.get("info", {}).get("counterexample")]
                projections = []
                if case in FALSE:
                    assert metadata["verdict"] == "FALSE" and infos, (case, bits, wide, data)
                    for index, info in enumerate(infos):
                        n, projection = verify(info, check, digest_json, case, source, bits, a.ikos,
                                               work / f"execution-{index}", verify_source_binding,
                                               build_witness, WitnessProjectionUnsupported)
                        checks += n
                        projections.append(projection)
                else:
                    assert not infos, (case, bits, wide, data)
                    if case in SAFE:
                        assert metadata["verdict"] == "SAFE", (case, bits, wide, data)
                    elif case == 20:
                        # The guard after the real init binds the same dynamic
                        # count to2. Neither one-arrival cap1 nor a fresh symbol
                        # can justify FALSE. A future independent SAFE proof is
                        # allowed; record its result for the reviewer.
                        assert metadata["verdict"] in ("UNKNOWN", "SAFE"), (case, bits, wide, data)
                        assert metadata.get("coverage") == "Checked", (case, bits, wide, metadata)
                    else:
                        assert metadata["verdict"] == "UNKNOWN", (case, bits, wide, data)
                checks += 1
                row = dict(case=case, bits=bits, wide=wide, verdict=metadata["verdict"],
                           executions=len(infos), source_projections=projections)
                if case == 20:
                    row["required_review"] = ("Check the saved report still exercises the dynamic count "
                        "candidate, or identify the independent SAFE proof; bounded UNSAT is not SAFE.")
                rows.append(row)
                save_json(folder / "summary.json", dict(runtime=identity, checker_files=checker_hashes,
                                                       rows=rows, assertions=checks))
                print(json.dumps(row), flush=True)
    verify_runtime(identity)
    assert checker_hashes == {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                              for path in checker_files}, "checker/source helper changed during test"
    print("PASS", checks)


if __name__ == "__main__":
    main()
