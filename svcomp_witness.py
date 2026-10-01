#!/usr/bin/env python3
"""SV-COMP entry point: run IKOS race analysis, print the verdict, and emit a
no-data-race *violation witness* (format 2.2) `witness.yml` when a race is found.

Runs IKOS once with `--format json` + `--demote-race-to-unknown` + the BMC
re-confirmation env gate (IKOS_BMC_RECOVER=1), forwards IKOS's summary/verdict
line to stdout (so a BenchExec tool-info module can parse it), and, on a
CONFIRMED race (Result::Error, "definitely UNSAFE"), writes `witness.yml` in
the YAML exchange format described by
https://gitlab.com/sosy-lab/benchmarking/sv-witnesses (user-guide/Witness-Format.md).

The witness is a `violation_sequence` whose final segment is a multi-follow
segment holding the two conflicting accesses as `target` waypoints. Spawned
threads are registered by `function_enter` waypoints on their `pthread_create`
call sites (k-th registration assigns thread_id k; main is 0).

Usage:
    python3 svcomp_witness.py <file.c|.i> [--ikos /path/to/ikos] [--data-model LP64|ILP32] [--out witness.yml]

Exit code is 0 on a clean analysis (race or not); non-zero only when IKOS
itself crashes, which a BenchExec tool-info maps to UNKNOWN.
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import uuid
from datetime import datetime, timezone


FORMAT_VERSION = "2.2"
SPECIFICATION = "G ! data-race"


def first_race(data):
    """Return the info dict of the first CONFIRMED race report, or None.

    Only a check with `status == 2` (Result::Error = "definitely UNSAFE") is a
    confirmed race. With `--demote-race-to-unknown`, every non-confirmed race
    is demoted to Result::Warning (`status == 1`, potentially UNSAFE = UNKNOWN),
    and the model-boundary groups carry `info.verdict == "unknown"` — neither
    may produce a violation witness, which asserts a race actually occurs.
    """
    for rep in data.get("reports", []):
        if rep.get("status") != 2:  # Result::Error (see result.hpp enum order)
            continue
        info = rep.get("info", {})
        if "access_a" in info and "access_b" in info:
            return info
    return None


def closing_paren_column(source, line, start_col):
    """Column of the ')' that closes the call starting at (line, start_col).

    The witness `function_enter` waypoint must point at the closing paren of
    the thread-creating call, not its start; IKOS reports the call's start.
    """
    try:
        with open(source, encoding="utf-8", errors="replace") as f:
            text = f.readlines()[line - 1]
    except (OSError, IndexError):
        return start_col
    depth = 0
    for i in range(start_col - 1, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return i + 1  # 1-indexed column
    return start_col


def loc(obj, source, kind="target"):
    """Build a witness location dict from an access/creation JSON object."""
    out = {"file_name": obj.get("file", "unknown.c")}
    if "line" in obj:
        out["line"] = obj["line"]
    if "column" in obj:
        col = obj["column"]
        if kind == "function_enter":
            col = closing_paren_column(source, obj["line"], col)
        out["column"] = col
    return out


def build_witness(source, info, data_model):
    thread_creations = info.get("thread_creations", [])

    # thread_id map: main -> 0, k-th pthread_create (in source order) -> k.
    tid = {"main": 0}
    for k, tc in enumerate(thread_creations, start=1):
        tid.setdefault(tc.get("thread_function"), k)

    creator_id = {}
    for tc in thread_creations:
        creator = tc.get("creator", "main")
        creator_id[creator] = tid.get(creator, 0)

    content = []
    for tc in thread_creations:
        seg = [{"waypoint": {
            "type": "function_enter",
            "action": "follow",
            "thread_id": creator_id.get(tc.get("creator", "main"), 0),
            "location": loc(tc, source, "function_enter"),
        }}]
        content.append({"segment": seg})

    # final multi-follow segment: the two conflicting accesses.
    final = [{"waypoint": {
        "type": "target",
        "action": "follow",
        "thread_id": tid.get(info["access_a"].get("thread_id"), 0),
        "location": loc(info["access_a"], source),
    }}, {"waypoint": {
        "type": "target",
        "action": "follow",
        "thread_id": tid.get(info["access_b"].get("thread_id"), 0),
        "location": loc(info["access_b"], source),
    }}]
    content.append({"segment": final})

    file_name = os.path.basename(source)
    sha = hashlib.sha256(open(source, "rb").read()).hexdigest()

    return [{
        "entry_type": "violation_sequence",
        "metadata": {
            "format_version": FORMAT_VERSION,
            "uuid": str(uuid.uuid4()),
            "creation_time": datetime.now(timezone.utc).strftime(
                "%Y-%m-%dT%H:%M:%SZ"),
            "producer": {"name": "ikos-conc", "version": "3.5"},
            "task": {
                "input_files": [file_name],
                "input_file_hashes": {file_name: sha},
                "specification": SPECIFICATION,
                "data_model": data_model,
                "language": "C",
            },
        },
        "content": content,
    }]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source")
    ap.add_argument("--ikos", default="ikos")
    ap.add_argument("--data-model", default="ILP32", choices=["LP64", "ILP32"])
    ap.add_argument("--out", default="witness.yml")
    args = ap.parse_args()

    db = tempfile.mktemp(suffix=".db")
    report = tempfile.mktemp(suffix=".json")
    # Pass the data model to clang (-m32 / -m64) so the .c/.i is compiled with
    # the right type widths (long/pointer). The SV-COMP no-data-race category is
    # entirely ILP32, so default to ILP32 (never the host 64-bit target).
    machine = ["-m", "32" if args.data_model == "ILP32" else "64"]
    # Enable the bounded-model-checking race re-confirmation: the abstract
    # checker demotes every race to UNKNOWN, then the BMC re-confirms the ones
    # it can prove (so those become "definitely UNSAFE" = FALSE). This recovers
    # ~64 TRUE positives at FP=0 / FN=0 (sound for the FALSE direction: it only
    # claims a race when the HB/alias encoding proves one). Kept behind the env
    # gate so the standalone demote-only run stays available.
    env = dict(os.environ, IKOS_BMC_RECOVER="1")
    try:
        proc = subprocess.run(
            [args.ikos, "--analyses=race", "--concurrency=auto",
             "--demote-race-to-unknown"] + machine +
            ["--format=json", "--report-file=" + report,
             "-o", db, args.source],
            capture_output=True, text=True, env=env)
    except FileNotFoundError:
        sys.stderr.write("error: ikos executable not found\n")
        return 127

    # Forward IKOS's output (carries the verdict line) for the tool-info
    # determine_result parser.
    sys.stdout.write(proc.stdout)
    sys.stderr.write(proc.stderr)
    if proc.returncode != 0:
        return proc.returncode  # crash -> non-zero -> UNKNOWN

    try:
        with open(report, encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError):
        return 0

    info = first_race(data)
    if info is not None:
        witness = build_witness(args.source, info, args.data_model)
        with open(args.out, "w", encoding="utf-8") as f:
            # JSON is a subset of YAML 1.2, so json.dump emits a valid
            # witness.yml without requiring the PyYAML dependency.
            json.dump(witness, f, indent=1)

    for p in (db, report, db + "-wal", db + "-shm"):
        try:
            os.remove(p)
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
