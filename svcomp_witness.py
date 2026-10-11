#!/usr/bin/env python3
"""Run IKOS and emit a confirmed no-data-race violation witness (format 2.2).

Thread registrations and targets use dynamic occurrences in the selected BMC
counterexample. FALSE is printed only after the witness has been written.
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

FORMAT_VERSION = "2.2"
SPECIFICATION = "G ! data-race"
CREATE_NAMES = {"pthread_create", "ar.pthread.create"}


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def first_race(data):
    """Select a confirmed race; may reports and explicit UNKNOWN cannot qualify."""
    require(
        isinstance(data, dict) and isinstance(data.get("reports"), list),
        "invalid IKOS report",
    )
    for rep in data["reports"]:
        require(isinstance(rep, dict), "invalid IKOS check")
        info = rep.get("info")
        if (
            rep.get("status") == 2
            and isinstance(info, dict)
            and info.get("verdict") != "unknown"
            and "access_a" in info
            and "access_b" in info
        ):
            return info
    return None


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
                for child in (children if kind == "DoStmt" else children[-1:]):
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
    traces = {t["id"]: t for t in trace_list}
    require(
        len(traces) == len(trace_list)
        and set(traces) <= {"startup", "a", "b"}
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
                    previous is None or previous < clock, "ambiguous occurrence order"
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
            require(
                (name, index) not in children, "selected creation follows a race target"
            )
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
                            "location": source_map.creation(inst),
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
                        "location": source_map.target(inst),
                    }
                }
                for (name, _), inst in zip(target_occurrences, target_instructions)
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
                "producer": {"name": "ikos-conc", "version": "3.5"},
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
    ap.add_argument("source")
    ap.add_argument("--ikos", default="ikos")
    ap.add_argument("--data-model", default="ILP32", choices=["LP64", "ILP32"])
    ap.add_argument("--out", default="witness.yml")
    args = ap.parse_args()
    temporary_witness = None
    try:
        require(
            os.path.realpath(args.out) != os.path.realpath(args.source),
            "witness output must differ from the source",
        )
        try:
            os.remove(args.out)
        except FileNotFoundError:
            pass
        require(
            os.path.splitext(args.source)[1] in {".c", ".i"},
            "only C .c/.i inputs are supported",
        )
        with open(args.source, "rb") as f:
            source_hash = hashlib.sha256(f.read()).digest()
        with tempfile.TemporaryDirectory(prefix="ikos-race-") as temp:
            report = os.path.join(temp, "report.json")
            db = os.path.join(temp, "output.db")
            proc = subprocess.run(
                [
                    args.ikos,
                    "--analyses=race",
                    "--concurrency=auto",
                    "--demote-race-to-unknown",
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
            require(
                proc.returncode == 0, "IKOS failed (exit {})".format(proc.returncode)
            )
            with open(report, encoding="utf-8") as f:
                data = json.load(f)
            with open(args.source, "rb") as f:
                require(
                    hashlib.sha256(f.read()).digest() == source_hash,
                    "source changed during analysis",
                )
            info = first_race(data)
            confirmed = "The program is definitely UNSAFE" in proc.stdout
            require(
                confirmed == (info is not None),
                "verdict and confirmed race report disagree",
            )
            if info is not None:
                witness = build_witness(args.source, info, args.data_model, args.ikos)
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
        RecursionError,
    ) as exc:
        sys.stderr.write("error: cannot produce a competition result: {}\n".format(exc))
        sys.stdout.write("The program is UNKNOWN\n")
        return 1
    finally:
        if temporary_witness is not None:
            try:
                os.remove(temporary_witness)
            except FileNotFoundError:
                pass


if __name__ == "__main__":
    sys.exit(main())
