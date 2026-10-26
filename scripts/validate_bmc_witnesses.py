#!/usr/bin/env python3
"""Export saved BMC pairs to GraphML and run local race witness validators.

Local research adapter; the competition's YAML 2.2 exporter is unchanged.
Source locations/order are projected from AR. SMT values, read-from choices,
subexpression identity and all dynamic call identities are NOT preserved.
An ACCEPT is therefore not an exact replay of the original SMT execution.
"""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
NS = "http://graphml.graphdrawing.org/xmlns"
ET.register_namespace("", NS)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def thread_ids(pair):
    """Only assign instances when the saved creation list is unambiguous."""
    for field in ("counterexample", "replay"):
        if field not in pair:
            continue
        replay = pair[field]
        if not isinstance(replay, dict):
            raise ValueError("malformed " + field + " field in legacy pair")
        traces = replay.get("traces")
        if (not isinstance(traces, list) or not traces
                or any(not isinstance(trace, dict)
                       or not isinstance(trace.get("id"), str) for trace in traces)):
            raise ValueError("malformed " + field + " traces in legacy pair")
        if any(trace["id"] in {"main", "provider"} for trace in traces):
            raise ValueError("timed main/provider source projection is unsupported by the legacy adapter")
    creations = pair["thread_creations"]
    instances = {"main": [0]}
    main_steps = list(pair.get("confirmed_startup", []))
    for side in ("a", "b"):
        if pair["access_" + side]["thread_id"] == "main":
            main_steps.extend(pair.get("confirmed_path_" + side, []))
    for i, creation in enumerate(creations, 1):
        creator = creation.get("creator", "main")
        if creator in {c["thread_function"] for c in creations}:
            raise ValueError("nested creator instance is not recorded")
        if creator != "main" and not any("call @" + creator + "(" in e["ar"] for e in main_steps):
            raise ValueError("creator helper is not recorded on a main prefix")
        instances.setdefault(creation["thread_function"], []).append(i)
    names = [pair["access_a"]["thread_id"], pair["access_b"]["thread_id"]]
    if names[0] == names[1]:
        ids = instances.get(names[0], [])
        if len(ids) != 2:
            raise ValueError("cannot identify two specific same-entry instances")
        return ids
    ids = []
    for name in names:
        choices = instances.get(name, [])
        if len(choices) != 1:
            raise ValueError("target thread instance is ambiguous or missing")
        ids.append(choices[0])
    return ids


def projected_steps(pair, mode):
    ids = thread_ids(pair)
    creations = pair["thread_creations"]
    steps = []
    if mode == "targets":
        for i, creation in enumerate(creations, 1):
            steps.append(dict(creation, thread=0, create=i))
        # Preserve the chosen target order when numeric model clocks exist.
        targets = []
        for side, tid in zip(("a", "b"), ids):
            path = pair.get("confirmed_path_" + side, [])
            clock = int(path[-1]["clock"]) if path else len(targets)
            targets.append((clock, dict(pair["access_" + side], thread=tid)))
        steps.extend(step for _, step in sorted(targets, key=lambda x: x[0]))
        return steps

    if not pair.get("confirmed_path_a") or not pair.get("confirmed_path_b"):
        raise ValueError("confirmed diagnostic paths are required for prefix mode")
    remaining_creations = list(enumerate(creations, 1))

    def project(path, tid, startup=False):
        groups = []
        for index, event in enumerate(path):
            ar = event["ar"]
            projected = dict(event, thread=tid)
            if re.search(r"\bcall @(?:ar\.pthread\.create|pthread_create)\(", ar):
                matches = [(i, c) for i, c in remaining_creations
                           if Path(c.get("file", "")).name == Path(event.get("file", "")).name
                           and c.get("line") == event.get("line")
                           and ("column" not in c or c["column"] == event.get("column"))]
                if tid != 0 or not matches:
                    raise ValueError("creation event has no unambiguous main-prefix binding")
                instance, creation = matches[0]
                remaining_creations.remove((instance, creation))
                projected["create"] = instance
            elif (("load " not in ar and not ar.startswith("store @")
                   and "call @" not in ar and index != len(path) - 1)
                  or "@ar.ikos.nondet" in ar or "@__VERIFIER_nondet" in ar):
                continue
            # Distinct creations must not be merged even at one source line.
            key = (event.get("file"), event.get("line"), projected.get("create"))
            clock = index if startup else int(event["clock"])
            if groups and groups[-1][0] == key:
                groups[-1] = (key, (clock, projected))
            else:
                groups.append((key, (clock, projected)))
        return [projected for _, projected in groups]

    # Startup precedes the workers. Preserve initialization relative to create;
    # moving all creations to the front can produce an impossible C sequence.
    steps.extend(event for _, event in project(pair.get("confirmed_startup", []), 0, True))
    events = []
    for side, tid in zip(("a", "b"), ids):
        path = pair["confirmed_path_" + side]
        # Coalesce a source expression BEFORE merging the threads: otherwise
        # an interleaved AR load/store can require executing one C line twice.
        # This is a source abstraction, not preservation of the AR schedule.
        events.extend(project(path, tid))
    if remaining_creations:
        raise ValueError("creation metadata is absent from the recorded execution prefix")
    # Startup ordering is separate in the saved diagnostics, not numeric clocks.
    # Its conditions/values are not asserted by this source projection.
    for _, event in sorted(events, key=lambda x: x[0]):
        steps.append(event)
    return steps


def export_graphml(row, pair, mode, destination):
    source = Path(row["input"]).resolve()
    if digest(source) != row["input_sha256"]:
        raise ValueError("source hash changed since the recorded BMC run")
    if row["data_model"] not in ("ILP32", "LP64"):
        raise ValueError("unknown machine model")
    steps = projected_steps(pair, mode)
    root = ET.Element("{" + NS + "}graphml")

    def element(parent, tag, attrs=None, text=None):
        child = ET.SubElement(parent, "{" + NS + "}" + tag, attrs or {})
        child.text = text
        return child

    keys = {
        "architecture": ("graph", "architecture", "string"),
        "programhash": ("graph", "programHash", "string"),
        "programfile": ("graph", "programFile", "string"),
        "specification": ("graph", "specification", "string"),
        "witness-type": ("graph", "witness-type", "string"),
        "producer": ("graph", "producer", "string"),
        "sourcecodelang": ("graph", "sourcecodeLanguage", "string"),
        "creationtime": ("graph", "creationTime", "string"),
        "entry": ("node", "isEntryNode", "boolean"),
        "violation": ("node", "isViolationNode", "boolean"),
        "threadId": ("edge", "threadId", "string"),
        "createThread": ("edge", "createThread", "string"),
        "startline": ("edge", "startline", "int"),
        "endline": ("edge", "endline", "int"),
        "originfile": ("edge", "originFileName", "string"),
    }
    for key, (scope, name, kind) in keys.items():
        element(root, "key", {"id": key, "for": scope,
                               "attr.name": name, "attr.type": kind})
    graph = element(root, "graph", {"edgedefault": "directed"})
    metadata = dict(architecture="32bit" if row["data_model"] == "ILP32" else "64bit",
                    programhash=row["input_sha256"], programfile=str(source),
                    specification="CHECK( init(main()), LTL(G ! data-race) )",
                    producer="IKOS-ConC local source projection",
                    sourcecodelang="C", creationtime=datetime.now(timezone.utc).isoformat())
    metadata["witness-type"] = "violation_witness"
    for key, value in metadata.items():
        element(graph, "data", {"key": key}, value)
    for i in range(len(steps) + 1):
        node = element(graph, "node", {"id": f"N{i}"})
        if i == 0:
            element(node, "data", {"key": "entry"}, "true")
        if i == len(steps):
            element(node, "data", {"key": "violation"}, "true")
    for i, step in enumerate(steps):
        if not step.get("line") or not step.get("file"):
            raise ValueError("missing source location")
        edge = element(graph, "edge", {"source": f"N{i}", "target": f"N{i+1}"})
        values = dict(threadId=str(step["thread"]), startline=str(step["line"]),
                      endline=str(step["line"]), originfile=step["file"])
        if "create" in step:
            values["createThread"] = str(step["create"])
        for key, value in values.items():
            element(edge, "data", {"key": key}, value)
    ET.indent(root)
    ET.ElementTree(root).write(destination, encoding="utf-8", xml_declaration=True)
    return {"steps": len(steps), "scope": "source_" + mode,
            "exact_smt_replay": False, "thread_ids": thread_ids(pair),
            "method": "per-thread source-line groups ordered by last AR clock" if mode == "prefix" else "ordered target locations",
            "limitations": ["SMT values and rf choices omitted", "startup prefix not replayed (creation edges only)",
                            "AR subexpressions projected to source lines",
                            "unrepresented threads not constrained"]}


def classify(tool, text, code, timeout):
    if timeout or re.search(r"Shutdown requested \((?:The )?CPU-time limit", text):
        return "TIMEOUT"
    if code != 0:
        return "ERROR"
    pattern = r"Verification result: (TRUE|FALSE|UNKNOWN)" if tool == "cpachecker" else r"^Result:\s*(PASS|FAIL|UNKNOWN)"
    found = re.findall(pattern, text, re.M)
    if not found:
        return "ERROR"
    return {"FALSE": "ACCEPT", "FAIL": "ACCEPT", "TRUE": "REJECT", "PASS": "REJECT"}.get(found[-1], "UNKNOWN")


def run_validator(tool, row, witness, folder, args):
    folder.mkdir()
    source = str(Path(row["input"]).resolve())
    bits = "32" if row["data_model"] == "ILP32" else "64"
    env = dict(os.environ)
    if tool == "cpachecker":
        home = args.cpachecker.resolve()
        # Keep the data-race specification alongside the witness automaton.
        # --spec witness alone replaces datarace.spc and cannot detect races.
        config = "svcomp26--datarace.properties" if args.independent else "violation-witness-validation-datarace.properties"
        command = [str(home / "bin/cpachecker"), "--" + bits,
                   "--config", str(home / "config" / config),
                   "--option", "parser.preprocessor=cpp -m" + bits,
                   "--output-path", str(folder / "output"),
                   "--timelimit", str(args.timeout) + "s", source]
        if not args.independent:
            command[4:4] = ["--spec", str(home / "config/specification/datarace.spc") + "," + str(witness)]
        cwd = folder
    else:
        home = args.dartagnan.resolve()
        env.update(DAT3M_HOME=str(home), DAT3M_OUTPUT=str(folder / "output"),
                   CFLAGS="-fgnu89-inline -m" + bits)
        command = ["java", "-Xmx1800m", "-jar", str(home / "svcomp/target/svcomp.jar"),
                   "--program.processing.skipAssertionsOfType=USER", "cat/svcomp.cat",
                   "--svcomp.property=" + str(args.property.resolve()), source]
        if not args.independent:
            command.insert(-1, "--validate=" + str(witness))
        cwd = home
    started = time.monotonic()
    proc = subprocess.Popen(command, cwd=cwd, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, start_new_session=True)
    timed_out = False
    try:
        out, _ = proc.communicate(timeout=args.timeout + 10)
    except subprocess.TimeoutExpired:
        timed_out = True
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        out, _ = proc.communicate()
    text = out.decode(errors="replace")
    (folder / "validator.log").write_text(text)
    result = dict(status=classify(tool, text, proc.returncode, timed_out),
                  evidence_kind="independent_analysis" if args.independent else "graphml_validation",
                  returncode=proc.returncode, seconds=time.monotonic() - started,
                  command=command, cwd=str(cwd),
                  warnings=[line for line in text.splitlines() if "WARN" in line or "Error:" in line])
    (folder / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pairs", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--mode", choices=("prefix", "targets"), default="prefix")
    parser.add_argument("--independent", action="store_true", help="analyze source without constraining it by the witness; corroboration only")
    parser.add_argument("--jobs", type=int, default=3)
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--cpachecker", type=Path, default=Path("/home/ruan/CPAchecker-4.2.2-unix/CPAchecker-4.2.2-unix"))
    parser.add_argument("--dartagnan", type=Path, default=Path("/home/ruan/dartagnan-svcomp2026/dartagnan"))
    parser.add_argument("--property", type=Path, default=Path("/home/ruan/sv-benchmarks-2026/c/properties/no-data-race.prp"))
    args = parser.parse_args()
    adapter_hash = digest(__file__)
    if args.jobs < 1 or args.timeout < 1:
        parser.error("jobs and timeout must be positive")
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    rows = json.loads(args.pairs.read_text())
    prepared, results = [], []
    for row in rows:
        for index, pair in enumerate(row["confirmed_pairs"]):
            key = hashlib.sha256(row["task"].encode()).hexdigest()[:10]
            folder = args.output / (Path(row["input"]).stem + "-" + key + f"-pair{index}")
            folder.mkdir()
            result = dict(task=row["task"], input=row["input"], data_model=row["data_model"],
                          pair_index=index, artifacts=folder.name)
            (folder / "pair.json").write_text(json.dumps(pair, indent=2) + "\n")
            try:
                witness = folder / "witness.graphml"
                result["projection"] = export_graphml(row, pair, args.mode, witness)
                for tool in ("cpachecker", "dartagnan"):
                    prepared.append((tool, row, witness, folder / tool, result))
            except (ValueError, KeyError) as exc:
                result["export_error"] = str(exc)
            results.append(result)
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = {executor.submit(run_validator, tool, row, witness, folder, args): (tool, result)
                   for tool, row, witness, folder, result in prepared}
        for i, future in enumerate(as_completed(futures), 1):
            tool, result = futures[future]
            try:
                result[tool] = future.result()
            except OSError as exc:
                result[tool] = dict(status="ERROR", error=str(exc))
            print(f"{i}/{len(futures)} {tool} {result['task']} pair {result['pair_index']}: {result[tool]['status']}", flush=True)
    summary = dict(tasks=len(rows), pairs=len(results), mode=args.mode,
                   independent=args.independent,
                   export_errors=sum("export_error" in r for r in results),
                   tools={t: dict(Counter(r[t]["status"] for r in results if t in r))
                          for t in ("cpachecker", "dartagnan")},
                   exact_smt_replay=False,
                   note="Both local legacy validators accepted corrupted thread IDs in calibration. Dartagnan also accepted unmatched target lines and nonconflicting targets. ACCEPT is not strict instance/pair validation or exact SMT replay.",
                   analyzer_sha256=digest(ROOT / "install/bin/ikos-analyzer"),
                   adapter_sha256=adapter_hash,
                   pairs_sha256=digest(args.pairs))
    (args.output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
