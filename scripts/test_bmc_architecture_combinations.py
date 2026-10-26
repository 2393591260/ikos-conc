#!/usr/bin/env python3
"""Cross Stage2/4/5/6 contracts; no generic C-completeness claim."""
import argparse
import contextlib
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

# name,CASE,A occurrence,B occurrence,wide,expected kind
SPECS = [
    ("later-provider-2x3", 0, 2, 3, False, "positive"),
    ("later-provider-3x2-wide", 0, 3, 2, True, "positive"),
    ("provider-zero", 1, 2, 3, False, "negative"),
    ("provider-no-return", 2, 3, 2, False, "negative"),
    ("later-provider-common-lock", 3, 2, 3, False, "negative"),
    ("provider-wrong-join", 4, 3, 2, False, "negative"),
    ("provider-overwritten", 5, 2, 3, False, "negative"),
    ("later-live2", 6, 2, 3, False, "positive"),
    ("later-free-old-wide", 7, 3, 2, True, "positive"),
    ("later-live3-helper-wide", 8, 3, 3, True, "positive"),
    ("later-dead-old-alias", 9, 2, 3, False, "negative"),
    ("later-different-generations", 10, 3, 2, False, "negative"),
    ("later-postspawn-free", 11, 2, 3, False, "negative"),
    ("provider-heap-boundary", 12, 2, 3, False, "boundary"),
    ("mutex-exclusion-remaining-race", 13, 2, 3, False, "positive"),
    ("window-exclusion-remaining-race", 14, 1, 1, False, "positive"),
    ("later-provider-helper-2x3", 15, 2, 3, False, "positive"),
    ("later-provider-helper-3x2-wide", 15, 3, 2, True, "positive"),
    ("later-provider-atomic-3x2", 16, 3, 2, False, "positive"),
    ("later-provider-same-entry-3x3", 17, 3, 3, False, "positive"),
    ("provider-atomic-no-opposite-return", 18, 3, 2, False, "negative"),
    ("provider-same-entry-disjoint", 19, 3, 3, False, "negative"),
    ("window-premise-unmet-real-race", 20, 1, 1, False, "positive"),
]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_combined(execution, spec, bits, APIs, source):
    _, case, a_count, b_count, wide, _ = spec
    check, digest_json, Replay, target_details, omit_step, verify_provider = APIs
    certificate = copy.deepcopy(execution)
    program = certificate.pop("program")
    certificate["program_sha256"] = digest_json(program)
    replay = Replay(program, certificate)
    accepted = replay.run()
    assert accepted["status"] == "ACCEPT", (spec, accepted)
    assert program["pointer_bits"] == bits
    traces = {trace["id"]: trace for trace in certificate["traces"]}
    details = target_details(program, certificate)
    expected = {"worker_a": a_count, "worker_b": b_count}
    if case in (14, 20):
        expected = {"worker_window": 1}
    elif case == 17:
        expected = {"worker_same": 3}
    assert len(details) == 2 and all(
        ordinal == expected[entry] for _, entry, _, ordinal in details), (spec, details)
    target_traces = [traces[row[0]] for row in details]
    assert len({(t["creator_trace"], t["creator_step"]) for t in target_traces}) == 2
    instructions = program["instructions"]
    # A retained main Return ends the C process; never justify later children.
    main_traces = [trace for trace in traces.values() if trace["entry"] == "main"]
    assert len(main_traces) == 1, (spec, traces.keys())
    main_returns = [step for step in main_traces[0]["steps"]
                    if instructions[step["statement"]]["kind"] == "return"]
    for terminal in main_returns:
        assert "clock" in terminal, "main Return requires a timed trace"
        assert all(int(step["clock"]) < int(terminal["clock"])
                   for trace in target_traces for step in trace["steps"]), spec
    for trace in target_traces:
        creator = traces[trace["creator_trace"]]["steps"][trace["creator_step"]]
        inst = instructions[creator["statement"]]
        assert inst["kind"] == "call" and inst["operands"][0]["id"] in (
            "pthread_create", "ar.pthread.create"), (spec, creator)
    result = dict(executions=1, mutations=0, later_targets=0, heap_objects=0,
                  provider_models=0, cross_generation_rf=0)

    def reject(changed, label):
        checked = check(program, changed)
        assert checked["status"] == "REJECT", (spec, label, checked)
        result["mutations"] += 1

    def step_in(changed, target):
        ts = {trace["id"]: trace for trace in changed["traces"]}
        return ts[target["trace"]]["steps"][target["step"]]

    # Exact later targets and remapped omission exercise dynamic identity/cache
    # keys, rather than merely checking that some FALSE was found.
    for tid, _, index, ordinal in details:
        if ordinal <= 1:
            continue
        statement = traces[tid]["steps"][index]["statement"]
        earlier = next(i for i, item in enumerate(traces[tid]["steps"][:index])
                       if item["statement"] == statement and "synthetic_event" not in item)
        changed = copy.deepcopy(certificate)
        next(t for t in changed["targets"] if t["trace"] == tid)["step"] = earlier
        reject(changed, "earlier same-statement disjoint access")
        changed = omit_step(certificate, tid, earlier)
        reject(changed, "omitted earlier actual memory occurrence, references remapped")
        result["later_targets"] += 1
    changed = copy.deepcopy(certificate)
    tid = target_traces[0]["id"]
    next(t for t in changed["traces"] if t["id"] == tid)["creator_step"] = (
        target_traces[1]["creator_step"])
    reject(changed, "target borrowed another actual create generation")

    if case <= 5 or case == 12 or 15 <= case <= 19:
        entries = {"worker_same"} if case == 17 else {"worker_a", "worker_b"}
        result["mutations"] += verify_provider(
            execution, 0, allow_target_joins=True, target_entries=entries, source=source) - 1
        result["provider_models"] += 1
        provider = traces["provider"]
        main = traces["main"]["steps"]
        assert int(main[provider["creator_step"]]["clock"]) < int(provider["steps"][0]["clock"])
        joined = next(item for item in main if item.get("joined_trace", {}).get("trace") == "provider")
        assert int(provider["steps"][-1]["clock"]) < int(joined["clock"])
        assert joined["joined_trace"]["step"] == len(provider["steps"]) - 1

    if case in (15, 16):
        wanted = {"write_cell", "read_cell"} if case == 15 else {
            "__VERIFIER_atomic_write_cell"}
        expanded = {step.get("expanded_callee")
                    for trace in target_traces for step in trace["steps"]}
        assert wanted <= expanded, (spec, wanted, expanded)
        for trace in target_traces:
            if trace["entry"] != "worker_a":
                continue
            function = "write_cell" if case == 15 else "__VERIFIER_atomic_write_cell"
            helper_calls = [(i, step) for i, step in enumerate(trace["steps"])
                            if step.get("expanded_callee") == function]
            assert len(helper_calls) >= expected[trace["entry"]], (spec, helper_calls)
            # At least an earlier helper invocation really returned with its own
            # changing scalar argument; borrowing its value must be rejected.
            earlier_results = [(i, step) for i, step in helper_calls
                               if "encoded_result" in step]
            assert earlier_results, (spec, "missing actual earlier helper return value")
            i, step = earlier_results[0]
            changed = copy.deepcopy(certificate)
            next(t for t in changed["traces"] if t["id"] == trace["id"])[
                "steps"][i]["encoded_result"] = str(int(step["encoded_result"]) + 1)
            reject(changed, "wrong actual helper return value")
        if case == 16:
            for trace in target_traces:
                assert instructions[trace["steps"][-1]["statement"]]["kind"] == "return"
                changed = copy.deepcopy(certificate)
                next(t for t in changed["traces"] if t["id"] == trace["id"])["steps"].pop()
                reject(changed, "atomic composition omitted an actual target root Return")
    if case == 17:
        creates = [traces[t["creator_trace"]]["steps"][t["creator_step"]]
                   for t in target_traces]
        assert creates[0]["statement"] == creates[1]["statement"], (
            spec, "same-entry instances must come from repeated actual static create")
        assert len({t["creator_step"] for t in target_traces}) == 2
        private_offsets = set()
        for tid, _, index, _ in details:
            statement = traces[tid]["steps"][index]["statement"]
            earlier = next(step for step in traces[tid]["steps"][:index]
                           if step["statement"] == statement and "synthetic_event" not in step)
            private_offsets.add(int(earlier["encoded_address"]["offset"]))
            obj = earlier["encoded_address"]["object"]
            assert (obj["kind"], obj["id"]) == ("global", "cells"), obj
            assert int(traces[tid]["steps"][index]["encoded_address"]["offset"]) == 0
        assert private_offsets == {4, 8}, (spec, "actual distinct per-create pointer arguments", private_offsets)
    if 6 <= case <= 12:
        ordinal = 3 if case == 8 else 2
        target_objects = [step_in(certificate, target)["encoded_address"]["object"]
                          for target in certificate["targets"]]
        assert all(obj["kind"] == "heap" and obj["occurrence"] == ordinal
                   for obj in target_objects), (spec, target_objects)
        assert target_objects[0] == target_objects[1], (spec, target_objects)
        obj = target_objects[0]
        heap = [value for value in replay.objects.values()
                if value.get("allocation") == obj["allocation"]]
        assert len(heap) == ordinal and sorted(v["occurrence"] for v in heap) == list(
            range(1, ordinal + 1)), (spec, heap)
        assert len({(v["allocation_event"]["trace"], v["allocation_event"]["step"])
                    for v in heap}) == ordinal, heap
        assert sum(v["live"] for v in heap) == ordinal - (case == 7), heap
        for value in heap:
            event = value["allocation_event"]
            actual = traces[event["trace"]]["steps"][event["step"]]
            assert actual["statement"] == obj["allocation"]
            inst = instructions[actual["statement"]]
            assert inst["kind"] == "call" and inst["operands"][0]["id"] in (
                "malloc", "ar.libc.malloc"), actual
        width = 8 if wide else 4
        assert all(int(step_in(certificate, t)["encoded_address"]["offset"]) == width
                   and step_in(certificate, t)["encoded_address"]["width"] == width
                   for t in certificate["targets"]), spec
        first = next(v for v in heap if v["occurrence"] == 1)
        target = certificate["targets"][0]
        changed = copy.deepcopy(certificate)
        damaged = step_in(changed, target)["encoded_address"]["object"]
        # Coherently forge a different real generation, not inconsistent fields.
        for key in ("occurrence", "allocation_event", "extent"):
            damaged[key] = copy.deepcopy(first[key] if key != "extent" else str(first["size"]))
        reject(changed, "same-site actual first-generation metadata substituted")
        changed = copy.deepcopy(certificate)
        step_in(changed, target)["encoded_address"]["object"]["allocation_event"]["step"] += 1
        reject(changed, "wrong allocation event step")
        changed = copy.deepcopy(certificate)
        step_in(changed, target)["encoded_address"]["object"]["extent"] = str(int(obj["extent"]) + 1)
        reject(changed, "wrong allocation extent")

        read_target = next(t for t in certificate["targets"] if instructions[
            step_in(certificate, t)["statement"]]["kind"] == "load")
        read = step_in(certificate, read_target)
        assert read.get("read_from"), "heap target omitted byte-rf"
        if "encoded_result" in read:
            assert int(read["encoded_result"]) == 0, read
        old_writes = [(name, index) for name, trace in traces.items()
                      for index, item in enumerate(trace["steps"])
                      if instructions[item["statement"]]["kind"] == "store"
                      and item.get("encoded_address", {}).get("object", {}).get("kind") == "heap"
                      and item["encoded_address"]["object"]["occurrence"] == 1
                      and int(item["encoded_address"]["offset"]) == width]
        assert old_writes, "old-generation equal-offset zero write missing"
        name, index = old_writes[-1]
        changed = copy.deepcopy(certificate)
        step_in(changed, read_target)["read_from"] = [
            {"trace": name, "step": index}] * len(read["read_from"])
        reject(changed, "same-valued same-offset different-generation rf")
        result["cross_generation_rf"] += 1
        result["heap_objects"] += len(heap)
        if case == 7:
            assert first["live"] is False
            free_events = [(name, i) for name, trace in traces.items()
                           for i, item in enumerate(trace["steps"])
                           if instructions[item["statement"]]["kind"] == "call"
                           and instructions[item["statement"]]["operands"][0].get("id")
                               in ("free", "ar.libc.free")]
            assert free_events
            name, i = free_events[0]
            changed = copy.deepcopy(certificate)
            ts = {trace["id"]: trace for trace in changed["traces"]}
            ts[name]["steps"][i]["encoded_address"]["object"] = copy.deepcopy(obj)
            reject(changed, "free borrowed actual later live generation")
    if case == 20:
        addresses = [step_in(certificate, target)["encoded_address"]
                     for target in certificate["targets"]]
        assert all(address["object"]["kind"] == "heap"
                   and address["object"]["occurrence"] == 1
                   and int(address["offset"]) == 0 for address in addresses), addresses
        assert addresses[0]["object"] == addresses[1]["object"], addresses
        assert len({traces[t["creator_trace"]]["steps"][t["creator_step"]]["statement"]
                    for t in target_traces}) == 1
    if case == 14:
        objects = [step_in(certificate, t)["encoded_address"]["object"]
                   for t in certificate["targets"]]
        assert all((obj["kind"], obj["id"]) == ("global", "cells")
                   for obj in objects), objects
        assert len({traces[t["creator_trace"]]["steps"][t["creator_step"]]["statement"]
                    for t in target_traces}) == 1, "expected one repeated static create site"
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--ikos", type=Path)
    parser.add_argument("--fixture", type=Path)
    parser.add_argument("--bits", type=int, choices=(32, 64))
    parser.add_argument("--case", type=int, action="append")
    parser.add_argument("--evidence-dir", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    root = args.project_root.resolve()
    sys.path[:0] = [str(root), str(root / "scripts")]
    from analyzer.python.ikos.race_result import validate_race_result
    from ikos_runtime import runtime_command, runtime_identity
    from sweep_bmc_execution import validate_report
    from validate_bmc_execution import Replay, check, digest_json
    from test_bmc_later_accesses import target_details, omit_unreferenced_step
    from test_bmc_startup_provider import verify_execution
    from test_bmc_combination_oracle import provider_oracle
    ikos = (args.ikos or root / "install/bin/ikos").resolve()
    fixture = args.fixture or root / "analyzer/test/regression/bmc-correctness/architecture-combinations.c"
    fixture_body = fixture.read_text()
    clean = {k: v for k, v in os.environ.items()
             if not k.startswith("IKOS_") and k not in ("PYTHONPATH", "PYTHONHOME")}
    # Runtime identity must be measured under the same clean parent environment.
    for key in tuple(os.environ):
        if key.startswith("IKOS_") or key in ("PYTHONPATH", "PYTHONHOME"):
            os.environ.pop(key)
    before = runtime_identity(ikos)
    selected = [spec for spec in SPECS if not args.case or spec[1] in args.case]
    assert selected, ("no selected combination cases", args.case)
    helpers = {}
    for function in (validate_race_result, runtime_command, runtime_identity,
                     validate_report, Replay, check, digest_json, target_details,
                     omit_unreferenced_step, verify_execution, provider_oracle):
        path = Path(sys.modules[function.__module__].__file__).resolve()
        helpers[str(path)] = sha(path)
    checker_path = Path(sys.modules[check.__module__].__file__).resolve()
    identity = dict(fixture_sha256=sha(fixture), runner_sha256=sha(Path(__file__)),
                    checker=str(checker_path), checker_sha256=sha(checker_path),
                    helper_sha256=helpers)
    rows, counts = [], dict(executions=0, mutations=0, later_targets=0, heap_objects=0,
                            provider_models=0, cross_generation_rf=0)
    manager = (contextlib.nullcontext(args.evidence_dir) if args.evidence_dir
               else tempfile.TemporaryDirectory(prefix="ikos-architecture-combinations-"))
    with manager as directory:
        work = Path(directory)
        work.mkdir(parents=True, exist_ok=True)
        for bits in ([args.bits] if args.bits else (32, 64)):
            for spec in selected:
                name, case, a_count, b_count, wide, kind = spec
                modes = (("enabled", "ai-only", "consumer-off", "producer-off") if case == 14 else
                         (("enabled", "ai-only") if case == 20 else ("enabled",)))
                for mode in modes:
                    run_dir = work / f"{name}-{bits}-{mode}"
                    run_dir.mkdir(exist_ok=True)
                    source, report, rules = (run_dir / "renamed.c", run_dir / "report.json",
                                             run_dir / "rules.jsonl")
                    source.write_text(f"#define CASE {case}\n#define A_COUNT {a_count}\n"
                                      f"#define B_COUNT {b_count}\n" +
                                      ("#define WIDE\n" if wide else "") + fixture_body)
                    report.unlink(missing_ok=True)
                    rules.unlink(missing_ok=True)
                    environment = dict(clean, IKOS_BMC_RECOVER="1", IKOS_BMC_DIAGNOSTICS="1",
                                       IKOS_RACE_RULE_REPORT=str(rules))
                    if mode == "consumer-off":
                        environment["IKOS_RACE_DISABLE_THREAD_ARGUMENT_CONSUMER"] = "0"
                    if mode == "producer-off":
                        environment["IKOS_RACE_DISABLE_THREAD_ARGUMENT_PRODUCER"] = "0"
                    options = ["--analyses=race", "--concurrency=auto", "--partitioning=guards",
                               "--demote-race-to-unknown", f"-m{bits}",
                               "--bmc-deadline-ms=" + ("0" if mode == "ai-only" else "20000"),
                               "--bmc-solver-timeout-ms=1000"]
                    if case in (14, 20):
                        options += ["--domain=var-pack-apron-polka-polyhedra"]
                    command = runtime_command(ikos) + options + [
                        "--format=json", "--report-file=" + str(report),
                        "-o", str(run_dir / "out.db"), str(source)]
                    process = subprocess.run(command, cwd=run_dir, env=environment,
                                             capture_output=True, text=True, timeout=60)
                    (run_dir / "command.json").write_text(json.dumps(command, indent=2) + "\n")
                    (run_dir / "run.log").write_text(process.stdout + process.stderr)
                    assert process.returncode == 0 and report.is_file(), (spec, bits, mode, process.stdout, process.stderr)
                    data = json.loads(report.read_text())
                    metadata = validate_race_result(data)
                    executions = [r["info"]["counterexample"] for r in data["reports"]
                                  if r.get("info", {}).get("counterexample")]
                    checked = validate_report(data)
                    assert len(checked) == len(executions)
                    expected_false = kind == "positive" and mode != "ai-only"
                    if kind == "boundary":
                        assert metadata["verdict"] in ("FALSE", "UNKNOWN"), metadata
                        expected_false = metadata["verdict"] == "FALSE"
                    assert (metadata["verdict"] == "FALSE") == expected_false, (spec, bits, mode, metadata)
                    assert bool(executions) == expected_false, (spec, bits, mode, metadata)
                    if not expected_false:
                        assert metadata["verdict"] in ("SAFE", "UNKNOWN"), metadata
                        assert metadata["analysis_complete"] and metadata["checks_complete"], (
                            spec, bits, mode, "incomplete analysis cannot pass a negative", metadata)
                    if expected_false:
                        assert checked and all(v["status"] == "ACCEPT" and v["strict_false_publishable"]
                                               for v in checked), (spec, checked)
                        assert metadata["coverage"] == "Checked"
                        for execution in executions:
                            result = validate_combined(execution, spec, bits, (
                                check, digest_json, Replay, target_details,
                                omit_unreferenced_step, verify_execution), source)
                            for key, value in result.items():
                                counts[key] += value
                    if case in (14, 20):
                        summaries = [json.loads(line) for line in rules.read_text().splitlines()
                                     if json.loads(line).get("kind") == "pair-rule-summary"]
                        assert summaries and summaries[-1]["remaining_pairs"] > 0
                        proof_count = summaries[-1]["counts"].get("thread-argument-windows-v1", 0)
                        assert bool(proof_count) == (case == 14 and mode in ("enabled", "ai-only")), (case, mode, summaries)
                        if mode == "ai-only":
                            assert metadata["verdict"] == "UNKNOWN" and not executions
                    if case == 13:
                        summaries = [json.loads(line) for line in rules.read_text().splitlines()
                                     if json.loads(line).get("kind") == "pair-rule-summary"]
                        assert any(s["counts"].get("common-mutex-v1", 0) > 0 for s in summaries), summaries
                    oracle = None
                    if case in (0, 1, 2, 3, 5):
                        oracle = provider_oracle(a_count, b_count,
                            ready_writes=(0,) if case == 1 else ((1, 0) if case == 5 else (1,)),
                            common_mutex=case == 3, blocked_provider=case == 2)
                        assert oracle["has_race"] == (case == 0), (spec, oracle)
                    row = dict(name=name, case=case, bits=bits, mode=mode, kind=kind,
                               verdict=metadata["verdict"], executions=len(executions),
                               source_sha256=sha(source), report_sha256=sha(report), oracle=oracle)
                    rows.append(row)
                    print(name, bits, mode, metadata["verdict"], len(executions), flush=True)
    if any(spec[-1] == "positive" for spec in selected):
        assert counts["executions"] and counts["mutations"], counts
    if not args.case:
        assert counts["provider_models"] and counts["heap_objects"] and counts["cross_generation_rf"], counts
    after = runtime_identity(ikos)
    assert before == after and sha(checker_path) == identity["checker_sha256"]
    assert sha(fixture) == identity["fixture_sha256"]
    assert sha(Path(__file__)) == identity["runner_sha256"]
    assert all(sha(Path(path)) == expected for path, expected in helpers.items()), "helper changed during combination run"
    evidence = dict(status="executed", rows=rows, counts=counts, identity=identity,
                    runtime_before=before, runtime_after=after,
                    boundary="provider+heap is audited if confirmed; UNKNOWN is explicitly retained, never counted as capability delivery")
    if args.output:
        args.output.write_text(json.dumps(evidence, indent=2) + "\n")
    print(json.dumps(counts), flush=True)


if __name__ == "__main__":
    main()
