#!/usr/bin/env python3
"""Generate an SV-COMP no-data-race *violation witness* (format 2.2) from IKOS.

Runs IKOS's race analysis (--format json), takes the first reported conflicting
pair, and emits `witness.yml` in the YAML exchange format described by
https://gitlab.com/sosy-lab/benchmarking/sv-witnesses (user-guide/Witness-Format.md).

The witness is a `violation_sequence` whose final segment is a multi-follow
segment holding the two conflicting accesses as `target` waypoints. Spawned
threads are registered by `function_enter` waypoints on their `pthread_create`
call sites (k-th registration assigns thread_id k; main is 0).

Usage:
    python3 svcomp_witness.py <file.c|.i> [--ikos /path/to/ikos] [--data-model LP64|ILP32]

Exit code 0 and `witness.yml` is written iff IKOS reports a definite race;
otherwise nothing is written.
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

import yaml

FORMAT_VERSION = "2.2"
SPECIFICATION = "G ! data-race"


def run_ikos(source, ikos):
    db = tempfile.mktemp(suffix=".db")
    report = tempfile.mktemp(suffix=".json")
    try:
        proc = subprocess.run(
            [ikos, "--analyses=race", "--concurrency=auto",
             "--format=json", "--report-file=" + report,
             "-o", db, source],
            capture_output=True, text=True)
        if proc.returncode != 0:
            return None, (proc.stdout + proc.stderr)
        with open(report, encoding="utf-8") as f:
            data = json.load(f)
        return data, None
    finally:
        for p in (db, report, db + "-wal", db + "-shm"):
            try:
                os.remove(p)
            except OSError:
                pass


def first_race(data):
    """Return (info, statements) of the first definite race report, or None."""
    for rep in data.get("reports", []):
        info = rep.get("info", {})
        if info.get("verdict") == "unknown":
            continue  # model boundary: cannot witness a definite race
        if "access_a" in info and "access_b" in info:
            return info
    return None


def loc(obj):
    """Build a witness location dict from an access/creation JSON object."""
    out = {"file_name": obj.get("file", "unknown.c")}
    if "line" in obj:
        out["line"] = obj["line"]
    if "column" in obj:
        out["column"] = obj["column"]
    return out


def build_witness(source, info):
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
            "location": loc(tc),
        }}]
        content.append({"segment": seg})

    # final multi-follow segment: the two conflicting accesses.
    final = [{"waypoint": {
        "type": "target",
        "action": "follow",
        "thread_id": tid.get(info["access_a"].get("thread_id"), 0),
        "location": loc(info["access_a"]),
    }}, {"waypoint": {
        "type": "target",
        "action": "follow",
        "thread_id": tid.get(info["access_b"].get("thread_id"), 0),
        "location": loc(info["access_b"]),
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
                "data_model": args_data_model,
                "language": "C",
            },
        },
        "content": content,
    }]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source")
    ap.add_argument("--ikos", default="ikos")
    ap.add_argument("--data-model", default="LP64", choices=["LP64", "ILP32"])
    ap.add_argument("--out", default="witness.yml")
    args = ap.parse_args()
    global args_data_model
    args_data_model = args.data_model

    data, err = run_ikos(args.source, args.ikos)
    if data is None:
        print("UNKNOWN" + ((": " + err.splitlines()[-1]) if err else ""),
              file=sys.stderr)
        return 1

    info = first_race(data)
    if info is None:
        print("no definite data race; no witness written", file=sys.stderr)
        return 1

    witness = build_witness(args.source, info)
    with open(args.out, "w", encoding="utf-8") as f:
        yaml.safe_dump(witness, f, sort_keys=False, allow_unicode=True)
    print(f"witness written to {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
