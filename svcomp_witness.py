#!/usr/bin/env python3
"""Run IKOS and emit a confirmed no-data-race violation witness (format 2.2).

Thread registrations and targets use dynamic occurrences in the selected BMC
counterexample. FALSE requires independent AR replay and a written witness.
The pinned specification is https://zenodo.org/records/21993990.
"""

import argparse
import bisect
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import uuid
from datetime import datetime, timezone

def race_protocol():
    # Load within the guarded request so missing modules still clean stale output.
    try:
        import race_result
    except ImportError:
        from analyzer.python.ikos import race_result
    return race_result

FORMAT_VERSION = "2.2"
SPECIFICATION = "G ! data-race"
CREATE_NAMES = {"pthread_create", "ar.pthread.create"}


class WitnessProjectionUnsupported(ValueError):
    """A valid selected schedule cannot use the supported source projection."""


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def first_race(data):
    """Select a confirmed race; may reports and explicit UNKNOWN cannot qualify."""
    require(
        isinstance(data, dict) and isinstance(data.get("reports"), list),
        "invalid IKOS report",
    )
    result = race_protocol().validate_race_result(data)
    if result['verdict'] != 'FALSE':
        return None
    for rep in data["reports"]:
        require(isinstance(rep, dict), "invalid IKOS check")
        info = rep.get("info")
        if (
            rep.get("status") == 2
            and isinstance(info, dict)
            and info.get("verdict") != "unknown"
            and info.get("bmc_result", {}).get("status") == "CONFIRMED"
            and "confirmed_pair_id" in info
            and "counterexample" in info
            and "access_a" in info
            and "access_b" in info
        ):
            return info
    return None


def validate_selected_execution(info, data_model):
    """Check the selected execution, without changing its paths or values."""
    if os.path.isfile(os.path.join(os.path.dirname(__file__), "validate_bmc_execution.py")):
        from validate_bmc_execution import check, digest_json
    else:
        from scripts.validate_bmc_execution import check, digest_json

    replay = info.get("counterexample")
    require(isinstance(replay, dict), "missing selected BMC execution")
    certificate = dict(replay)
    program = certificate.pop("program")
    require(program.get("pointer_bits") == (32 if data_model == "ILP32" else 64),
            "execution data model differs from the task")
    certificate["program_sha256"] = digest_json(program)
    result = check(program, certificate)
    require(result.get("status") in {"ACCEPT", "UNSUPPORTED"},
            "selected BMC execution rejected: " + str(result.get("reason", result)))
    return result


class SourceMap:
    """Map debug positions and Clang expression ranges to the hashed input."""

    def __init__(self, source):
        self.source = os.path.abspath(source)
        self.file_name = os.path.basename(source)
        with open(source, "rb") as f:
            self.bytes = f.read()
        self.text = self.bytes.decode("utf-8")
        self.lines = [
            line.decode("utf-8") for line in self.bytes.splitlines(keepends=True)
        ]
        self.starts = []
        offset = 0
        for line in self.lines:
            self.starts.append(offset)
            offset += len(line.encode("utf-8"))
        # A commented directive cannot change the compiler's presumed position.
        tokens = r'/\*.*?\*/|//[^\r\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
        clean = re.sub(
            tokens,
            lambda m: m[0] if m[0][0] in "\"'" else re.sub(r"[^\r\n]", " ", m[0]),
            self.text,
            flags=re.DOTALL,
        )
        marker = re.compile(
            r'^\s*#\s*(?:line\s+)?(\d+)(?:\s+("(?:\\.|[^"\\])*"))?(?:\s+\d+)*\s*$'
        )
        clean_lines = [
            line.decode("utf-8")
            for line in clean.encode("utf-8").splitlines(keepends=True)
        ]
        require(
            all(
                not re.match(r"^\s*#\s*(?:line\b|\d)", line) or marker.match(line)
                for line in clean_lines
            ),
            "unsupported linemarker syntax",
        )
        has_markers = any(marker.match(line) for line in clean_lines)
        require(
            not has_markers
            or (
                not re.search(
                    r"^\s*#\s*(?:if|ifdef|ifndef|elif|else|endif)\b",
                    clean,
                    re.MULTILINE,
                )
                and not re.search(r"\\\r?\n", self.text)
            ),
            "conditional or spliced linemarkers cannot be mapped",
        )
        self.positions = {}
        file_name, logical = self.source, 1
        for physical, line in enumerate(clean_lines, 1):
            match = marker.match(line)
            if match:
                logical = int(match[1])
                if match[2]:
                    file_name = json.loads(match[2])
                continue
            self.positions.setdefault((self.path(file_name), logical), []).append(
                physical
            )
            logical += 1
        self.expressions, self.creations = [], []

    @staticmethod
    def path(file_name):
        require(isinstance(file_name, str) and file_name, "missing source file")
        return os.path.realpath(file_name)

    def position(self, obj):
        line = obj.get("line")
        require(type(line) is int and line > 0, "missing source line")
        positions = self.positions.get((self.path(obj.get("file")), line), [])
        require(len(positions) == 1, "source position is absent or ambiguous")
        physical = positions[0]
        column = obj.get("column")
        raw_line = self.lines[physical - 1].rstrip("\r\n").encode("utf-8")
        require(
            type(column) is int and 1 <= column <= len(raw_line),
            "missing or invalid debug column",
        )
        return self.starts[physical - 1] + column - 1

    def location(self, offset):
        require(
            type(offset) is int and 0 <= offset < len(self.bytes),
            "invalid physical offset",
        )
        line = bisect.bisect_right(self.starts, offset)
        column = len(self.bytes[self.starts[line - 1] : offset].decode("utf-8")) + 1
        return {"file_name": self.file_name, "line": line, "column": column}

    def load_ast(self, ikos, data_model):
        executable = shutil.which(ikos)
        require(executable is not None, "IKOS executable not found")
        config = os.path.join(
            os.path.dirname(os.path.abspath(executable)), "ikos-config"
        )
        proc = subprocess.run(
            [config, "--clang", "--includedir"], capture_output=True, text=True
        )
        require(
            proc.returncode == 0 and len(proc.stdout.splitlines()) == 2,
            "cannot obtain IKOS frontend configuration",
        )
        clang, include = proc.stdout.splitlines()
        prefix = os.path.dirname(os.path.dirname(os.path.abspath(executable)))
        libraries = [
            os.path.join(prefix, "lib"),
            os.path.join(os.path.dirname(clang), "..", "lib"),
        ]
        env = dict(os.environ)
        env["LD_LIBRARY_PATH"] = os.pathsep.join(
            libraries + [env.get("LD_LIBRARY_PATH", "")]
        )
        # Match analyzer.py:clang_ikos_flags and the wrapper's compile inputs.
        proc = subprocess.run(
            [
                clang,
                "-U_FORTIFY_SOURCE",
                "-D_FORTIFY_SOURCE=0",
                "-D__IKOS__",
                "-g",
                "-O0",
                "-Xclang",
                "-disable-O0-optnone",
                "-fgnu89-inline",
                "-isystem",
                include,
                "-m32" if data_model == "ILP32" else "-m64",
                "-w",
                "-fsyntax-only",
                "-Xclang",
                "-ast-dump=json",
                self.source,
            ],
            capture_output=True,
            text=True,
            env=env,
        )
        require(
            proc.returncode == 0, "Clang AST extraction failed: " + proc.stderr.strip()
        )
        ast = json.loads(proc.stdout)
        with open(self.source, "rb") as f:
            require(f.read() == self.bytes, "source changed during AST extraction")
        # Clang elides repeated physical filenames in JSON. Restore them in
        # serialization order, including both spelling and expansion locations.
        file_name = None
        pending = [ast]
        while pending:
            obj = pending.pop()
            if isinstance(obj, dict):
                if "offset" in obj:
                    file_name = obj.get("file", file_name)
                    if file_name is not None:
                        obj["physical_file"] = self.path(file_name)
                pending.extend(reversed(list(obj.values())))
            elif isinstance(obj, list):
                pending.extend(reversed(obj))

        def interval(node):
            span = node.get("range", {})
            begin, end = span.get("begin", {}), span.get("end", {})
            # Macro-generated endpoints need a distinct source expansion proof.
            if begin.get("physical_file") != self.path(self.source) or end.get(
                "physical_file"
            ) != self.path(self.source):
                return None
            a, b = begin.get("offset"), end.get("offset")
            if type(a) is int and type(b) is int and 0 <= a <= b < len(self.bytes):
                return a, b + end.get("tokLen", 1)
            return None

        def expression(node):
            return "valueCategory" in node

        def add(node):
            if expression(node):
                span = interval(node)
                if span is not None:
                    self.expressions.append(span)

        def initializer(node):
            # Aggregate initializer semantics need scalar-element ranges,
            # including Clang's array_filler; leave these unsupported here.
            if node.get("kind") != "InitListExpr":
                add(node)

        pending = [ast]
        while pending:
            node = pending.pop()
            children = node.get("inner", [])
            kind = node.get("kind")
            if kind == "CompoundStmt":
                for child in children:
                    add(child)
            elif kind == "VarDecl" and "init" in node:
                values = [child for child in children if expression(child)]
                if len(values) == 1:
                    initializer(values[0])
            elif kind in {"ReturnStmt", "IfStmt", "WhileStmt", "SwitchStmt"}:
                if children:
                    add(children[0])
                if kind != "ReturnStmt":
                    for child in children[1:]:
                        add(child)
            elif kind == "ForStmt" and len(children) == 5:
                for index in (0, 2, 3, 4):
                    add(children[index])
            elif kind in {
                "DoStmt",
                "LabelStmt",
                "CaseStmt",
                "DefaultStmt",
                "AttributedStmt",
            }:
                for child in children if kind == "DoStmt" else children[-1:]:
                    add(child)
            if kind == "CallExpr" and children:
                callee = children[0]
                while (
                    callee.get("kind") in {"ImplicitCastExpr", "ParenExpr"}
                    and len(callee.get("inner", [])) == 1
                ):
                    callee = callee["inner"][0]
                if (
                    callee.get("referencedDecl", {}).get("kind") == "FunctionDecl"
                    and callee["referencedDecl"].get("name") == "pthread_create"
                ):
                    span = interval(node)
                    end = node.get("range", {}).get("end", {}).get("offset")
                    if (
                        span is not None
                        and type(end) is int
                        and self.bytes[end : end + 1] == b")"
                    ):
                        self.creations.append((span[0], end))
            pending.extend(reversed(children))

    def creation(self, obj):
        offset = self.position(obj)
        matches = [end for begin, end in self.creations if begin == offset]
        require(len(matches) == 1, "creation has no unique physical Clang call range")
        # The pinned 2.2 guide puts function_enter at the closing parenthesis.
        return self.location(matches[0])

    def target(self, obj):
        offset = self.position(obj)
        matches = [begin for begin, end in self.expressions if begin <= offset < end]
        require(
            len(matches) == 1, "target has no unique physical Clang full expression"
        )
        return self.location(matches[0])


def build_witness(source, info, data_model, ikos="ikos"):
    require(
        os.path.splitext(source)[1] in {".c", ".i"}, "only C .c/.i inputs are supported"
    )
    require(data_model in {"ILP32", "LP64"}, "unsupported data model")
    replay = info.get("counterexample")
    require(
        isinstance(replay, dict) and replay.get("schema") == 1,
        "confirmed race has no supported counterexample",
    )
    program = replay["program"]
    instructions = program["instructions"]
    trace_list = replay["traces"]
    native_profile = any(instructions[step['statement']].get('native_sc')
                         for trace in trace_list for step in trace['steps'])
    if native_profile:
        require(validate_selected_execution(info, data_model).get("status") == "ACCEPT",
                "native SC projection requires complete independent AR replay")
    trace_entries = {trace["id"]: trace["entry"] for trace in trace_list}
    creator_graph = replay.get("creator_graph") is True or any(
        trace.get("creator_trace") in trace_entries and
        trace_entries[trace["creator_trace"]] != "main" for trace in trace_list)
    if creator_graph or any("once_role" in step for trace in trace_list for step in trace["steps"]):
        require(validate_selected_execution(info, data_model).get("status") == "ACCEPT",
                "once/creator graph projection requires complete independent AR replay")
        # Callback frames use their caller's existing physical trace/thread id.
        # Only actual pthread_create occurrences below register new threads.
    # Setters and indirect calls require replay even without optional profile
    # metadata: a pointer adapter may resolve to a recursive setter at runtime.
    recursive_execution = any(
        step.get("mutex_semantics", {}).get("recursive") is True or
        (instructions[step["statement"]].get("kind") == "call" and
         instructions[step["statement"]].get("operands") and
         (instructions[step["statement"]]["operands"][0].get("kind") != "function" or
          instructions[step["statement"]]["operands"][0].get("id") == "pthread_mutexattr_settype"))
        for trace in trace_list for step in trace["steps"])
    if replay.get("mutex_profile") == "gnu-x86-recursive-v1" or recursive_execution:
        require(validate_selected_execution(info, data_model).get("status") == "ACCEPT",
                "recursive mutex projection requires complete independent AR replay")
        # AR replay retains actual initialization, relock and partial/final unlock
        # occurrences; only real creates below assign physical thread identities.
    traces = {t["id"]: t for t in trace_list}
    require(
        len(traces) == len(trace_list)
        and all(name in {"startup", "main", "provider", "a", "b"} or
                (name.startswith("aux_") and name[4:].isdigit()) for name in traces)
        and {"a", "b"} <= set(traces),
        "invalid dynamic trace identities",
    )
    roots = [name for name, trace in traces.items() if trace["entry"] == "main"]
    require(len(roots) == 1, "counterexample needs exactly one main instance")
    root = roots[0]
    require(
        "creator_trace" not in traces[root] and "creator_step" not in traces[root],
        "main cannot have a creator",
    )
    if "startup" in traces:
        require(root == "startup", "startup is not the main instance")
    if "main" in traces:
        require(root == "main", "main is not the timed main instance")
    if "provider" in traces:
        require(root == "main", "provider requires a timed main instance")

    def instruction(name, index):
        require(
            name in traces
            and type(index) is int
            and 0 <= index < len(traces[name]["steps"]),
            "invalid dynamic occurrence",
        )
        step = traces[name]["steps"][index]
        return instructions[step["statement"]]

    events = []
    for name, trace in traces.items():
        previous = None
        for index, step in enumerate(trace["steps"]):
            inst = instruction(name, index)
            if name == "startup":
                order = (0, index)
            else:
                require(
                    isinstance(step.get("clock"), (str, int))
                    and type(step.get("clock")) is not bool,
                    "missing occurrence clock",
                )
                clock = int(step["clock"])
                require(
                    previous is None or previous < clock or (native_profile and previous == clock),
                    "ambiguous occurrence order"
                )
                previous = clock
                order = (1, clock)
            events.append((order, name, index, inst))
    events.sort(key=lambda event: event[0])
    orders = {(name, index): order for order, name, index, _ in events}

    children = {}
    for name, trace in traces.items():
        if name == root:
            continue
        creator = trace.get("creator_trace")
        index = trace.get("creator_step")
        inst = instruction(creator, index)
        operands = inst.get("operands", [])
        require(
            inst.get("kind") == "call"
            and len(operands) == 5
            and operands[0].get("kind") == "function"
            and operands[0].get("id") in CREATE_NAMES,
            "creator occurrence is not pthread_create",
        )
        entry = operands[3]
        require(
            entry.get("kind") == "function" and entry.get("id") == trace["entry"],
            "creation entry cannot be matched to dynamic trace",
        )
        occurrence = (creator, index)
        require(occurrence not in children, "two traces share one creation occurrence")
        require(
            trace["steps"] and orders[occurrence] < orders[(name, 0)],
            "thread executes before its creation",
        )
        children[occurrence] = name

    targets = replay["targets"]
    require(
        isinstance(targets, list) and len(targets) == 2,
        "race needs exactly two targets",
    )
    target_occurrences = [(target["trace"], target["step"]) for target in targets]
    if root == "main":
        require({name for name, _ in target_occurrences} == {"a", "b"},
                "timed main requires selected worker targets")
    target_instructions = [
        instruction(*occurrence) for occurrence in target_occurrences
    ]
    require(
        all(
            inst.get("kind") in {"load", "store", "call"}
            for inst in target_instructions
        ),
        "race target is not a memory access",
    )
    relevant_orders = [
        order
        for order, name, index, inst in events
        if (name, index) in target_occurrences
        or (
            inst.get("kind") == "call"
            and inst.get("operands")
            and inst["operands"][0].get("kind") == "function"
            and inst["operands"][0].get("id") in CREATE_NAMES
        )
    ]
    require(
        len(set(relevant_orders)) == len(relevant_orders),
        "ambiguous registration/target order",
    )
    earliest_target = min(orders[occurrence] for occurrence in target_occurrences)
    source_map = SourceMap(source)
    source_map.load_ast(ikos, data_model)
    # Diagnose corrupt source bindings before a representational limit can
    # downgrade the selected execution to UNKNOWN. Include selected late creates.
    creation_locations = {}
    for order, name, index, inst in events:
        operands = inst.get("operands", [])
        if (inst.get("kind") == "call" and operands
                and operands[0].get("kind") == "function"
                and operands[0].get("id") in CREATE_NAMES
                and (order < earliest_target or (name, index) in children)):
            creation_locations[(name, index)] = source_map.creation(inst)
    target_locations = [source_map.target(inst) for inst in target_instructions]
    tids = {root: 0}
    content = []
    next_tid = 1
    for order, name, index, inst in events:
        operands = inst.get("operands", [])
        if not (
            inst.get("kind") == "call"
            and operands
            and operands[0].get("kind") == "function"
            and operands[0].get("id") in CREATE_NAMES
        ):
            continue
        # Final target segments cannot precede registrations. Preserve the
        # selected schedule instead of silently moving a later spawn earlier.
        if order >= earliest_target:
            if (name, index) in children:
                raise WitnessProjectionUnsupported("selected creation follows a race target")
            continue
        require(name in tids, "creation belongs to an unregistered dynamic creator")
        content.append(
            {
                "segment": [
                    {
                        "waypoint": {
                            "type": "function_enter",
                            "action": "follow",
                            "thread_id": tids[name],
                            "location": creation_locations[(name, index)],
                        }
                    }
                ]
            }
        )
        child = children.get((name, index))
        if child is not None:
            tids[child] = next_tid
        next_tid += 1
    require(
        all(name in tids for name, _ in target_occurrences),
        "target has no registered thread instance",
    )
    require(
        tids[target_occurrences[0][0]] != tids[target_occurrences[1][0]],
        "race targets belong to the same thread instance",
    )
    content.append(
        {
            "segment": [
                {
                    "waypoint": {
                        "type": "target",
                        "action": "follow",
                        "thread_id": tids[name],
                        "location": location,
                    }
                }
                for (name, _), location in zip(target_occurrences, target_locations)
            ]
        }
    )
    file_name = source_map.file_name
    return [
        {
            "entry_type": "violation_sequence",
            "metadata": {
                "format_version": FORMAT_VERSION,
                "uuid": str(uuid.uuid4()),
                "creation_time": datetime.now(timezone.utc).strftime(
                    "%Y-%m-%dT%H:%M:%SZ"
                ),
                "producer": {"name": "ikos-conc", "version": "1.0"},
                "task": {
                    "input_files": [file_name],
                    "input_file_hashes": {
                        file_name: hashlib.sha256(source_map.bytes).hexdigest()
                    },
                    "specification": SPECIFICATION,
                    "data_model": data_model,
                    "language": "C",
                },
            },
            "content": content,
        }
    ]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source", nargs="?")
    ap.add_argument(
        "--ikos",
        default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "ikos"),
    )
    ap.add_argument(
        "--version", action="store_true", help="print the bundled IKOS version"
    )
    ap.add_argument("--property", help="SV-COMP property file (no-data-race)")
    ap.add_argument("--data-model", default="ILP32", choices=["LP64", "ILP32"])
    ap.add_argument("--out", default="witness.yml")
    inlining = ap.add_mutually_exclusive_group()
    inlining.add_argument("--inline-allocators", action="store_true",
                         help="expand bounded allocation constructors before analysis")
    inlining.add_argument("--try-inline-allocators", action="store_true",
                         help="retry an inconclusive analysis with constructor inlining")
    ap.add_argument(
        "--bmc-deadline-ms",
        type=int,
        help="BMC cooperative deadline in milliseconds (0 disables BMC)",
    )
    ap.add_argument(
        "--bmc-solver-timeout-ms",
        type=int,
        help="time limit per SMT query in milliseconds",
    )
    args = ap.parse_args()
    if args.bmc_deadline_ms is not None and args.bmc_deadline_ms < 0:
        ap.error("--bmc-deadline-ms must be non-negative")
    if args.bmc_solver_timeout_ms is not None and args.bmc_solver_timeout_ms <= 0:
        ap.error("--bmc-solver-timeout-ms must be positive")
    if not args.version and args.source is None:
        ap.error("a source file is required unless --version is given")
    temporary_witness = None
    try:
        if args.version:
            return subprocess.call([args.ikos, "--version"])
        require(
            os.path.realpath(args.out) != os.path.realpath(args.source),
            "witness output must differ from the source",
        )
        try:
            os.remove(args.out)
        except FileNotFoundError:
            pass
        if args.property:
            with open(args.property, encoding="utf-8") as f:
                specification = re.sub(r"\s+", "", f.read())
            require(
                specification == "CHECK(init(main()),LTL(G!data-race))",
                "only the no-data-race property is supported",
            )
        require(
            os.path.splitext(args.source)[1] in {".c", ".i"},
            "only C .c/.i inputs are supported",
        )
        with open(args.source, "rb") as f:
            source_hash = hashlib.sha256(f.read()).digest()
        with tempfile.TemporaryDirectory(prefix="ikos-race-") as temp:
            report = os.path.join(temp, "report.json")
            db = os.path.join(temp, "output.db")
            inventory = os.path.join(temp, "allocators.json")
            budget_options = [
                "{}={}".format(name, value)
                for name, value in (
                    ("--bmc-deadline-ms", args.bmc_deadline_ms),
                    ("--bmc-solver-timeout-ms", args.bmc_solver_timeout_ms),
                )
                if value is not None
            ]
            modes = [False, True] if args.try_inline_allocators else [args.inline_allocators]
            for inline in modes:
                try:
                    os.remove(report)
                except FileNotFoundError:
                    pass
                proc = subprocess.run(
                    [
                        args.ikos,
                        "--analyses=race",
                        "--concurrency=auto",
                        "--partitioning=guards",
                        "--demote-race-to-unknown",
                        *(["--inline-allocators"] if inline else []),
                        *(["--allocator-inventory=" + inventory]
                          if args.try_inline_allocators and not inline else []),
                        *budget_options,
                        "-m",
                        "32" if args.data_model == "ILP32" else "64",
                        "--format=json",
                        "--report-file=" + report,
                        "-o",
                        db,
                        args.source,
                    ],
                    capture_output=True,
                    text=True,
                    env=dict(os.environ, IKOS_BMC_RECOVER="1"),
                )
                require(proc.returncode == 0, "IKOS failed (exit {})".format(proc.returncode))
                with open(report, encoding="utf-8") as f:
                    data = json.load(f)
                with open(args.source, "rb") as f:
                    require(hashlib.sha256(f.read()).digest() == source_hash,
                            "source changed during analysis")
                info = first_race(data)
                verdicts = {line.strip() for line in proc.stdout.splitlines()
                            if line.strip() in {"The program is SAFE",
                                                "The program is definitely UNSAFE",
                                                "The program is potentially UNSAFE",
                                                "The program is UNKNOWN"}}
                require(len(verdicts) == 1, "missing or conflicting analyzer verdict")
                verdict = next(iter(verdicts))
                require(verdict == race_protocol().race_verdict_line(data),
                        "text and race_result disagree")
                require((verdict == "The program is definitely UNSAFE") == (info is not None),
                        "verdict and confirmed race report disagree")
                if not (args.try_inline_allocators and not inline and
                        verdict in {"The program is potentially UNSAFE", "The program is UNKNOWN"}):
                    break
                with open(inventory, encoding="utf-8") as f:
                    candidates = json.load(f)
                count = candidates['eligible_calls']
                require(type(candidates.get('schema')) is int and candidates['schema'] == 1 and
                        type(count) is int and count >= 0,
                        'invalid allocator inventory')
                if count == 0:
                    break
                sys.stderr.write(proc.stderr)
                sys.stderr.write("[ikos-conc] Inconclusive analysis; trying allocation constructors.\n")
            # Preserve the final attempt's gate in competition logs after temp cleanup.
            sys.stderr.write('[ikos-conc] race_result: ' +
                             json.dumps(data['race_result'], sort_keys=True) + '\n')
            if info is not None:
                validation = validate_selected_execution(info, args.data_model)
                if validation["status"] == "UNSUPPORTED":
                    sys.stderr.write(proc.stderr)
                    sys.stderr.write("[ikos-conc] AR validation unsupported: {}\n".format(
                        validation.get("reason", "unsupported execution")))
                    print("The program is UNKNOWN")
                    return 0
                try:
                    witness = build_witness(args.source, info, args.data_model, args.ikos)
                except WitnessProjectionUnsupported as exc:
                    with open(args.source, "rb") as f:
                        require(hashlib.sha256(f.read()).digest() == source_hash,
                                "source changed during execution validation or witness mapping")
                    sys.stderr.write(proc.stderr)
                    sys.stderr.write("[ikos-conc] Witness projection unsupported: {}\n".format(exc))
                    print("The program is UNKNOWN")
                    return 0
                with open(args.source, "rb") as f:
                    require(hashlib.sha256(f.read()).digest() == source_hash,
                            "source changed during execution validation or witness mapping")
                require(witness[0]["metadata"]["task"]["input_file_hashes"][
                    os.path.basename(args.source)] == source_hash.hex(),
                    "witness source differs from the analyzed input")
                with tempfile.NamedTemporaryFile(
                    mode="w",
                    encoding="utf-8",
                    prefix=".ikos-witness-",
                    dir=os.path.dirname(os.path.abspath(args.out)),
                    delete=False,
                ) as f:
                    temporary_witness = f.name
                    # JSON is a subset of YAML 1.2; no PyYAML dependency needed.
                    json.dump(witness, f, indent=1)
                os.replace(temporary_witness, args.out)
                temporary_witness = None
            sys.stderr.write(proc.stderr)
            sys.stdout.write(proc.stdout)
        return 0
    except (
        OSError,
        ValueError,
        KeyError,
        TypeError,
        AttributeError,
        ImportError,
        RuntimeError,
        RecursionError,
    ) as exc:
        sys.stderr.write("error: cannot produce a competition result: {}\n".format(exc))
        return 1
    finally:
        if temporary_witness is not None:
            try:
                os.remove(temporary_witness)
            except FileNotFoundError:
                pass


if __name__ == "__main__":
    sys.exit(main())
