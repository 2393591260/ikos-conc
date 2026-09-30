#!/usr/bin/env python3
"""Generate a Dartagnan-understandable (GraphML, SV-COMP witness 2.0) data-race
violation witness from an IKOS race report, and optionally validate it with
Dartagnan.

IKOS's `svcomp_witness.py` emits the COMPETITION format (YAML 2.2), which
Dartagnan 4.3.1 cannot parse (it only reads GraphML). This script emits the
GraphML form so Dartagnan's `--validate` can semantically confirm that the
race IKOS flagged is real — an independent cross-check of IKOS's TPs.

Usage:
    python3 ikos2graphml.py <file.c> --ikos /path/to/ikos \
        [--out witness.graphml] [--dartagnan /path/to/dartagnan.jar]
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from datetime import datetime, timezone

GRAPHML_TEMPLATE = """<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<graphml xmlns="http://graphml.graphdrawing.org/xmlns">
<key attr.name="witness-type" attr.type="string" for="graph" id="witness-type"/>
<key attr.name="producer" attr.type="string" for="graph" id="producer"/>
<key attr.name="programfile" attr.type="string" for="graph" id="programfile"/>
<key attr.name="programhash" attr.type="string" for="graph" id="programhash"/>
<key attr.name="specification" attr.type="string" for="graph" id="specification"/>
<key attr.name="unroll-bound" attr.type="string" for="graph" id="unroll-bound"/>
<key attr.name="entry" attr.type="boolean" for="node" id="entry"/>
<key attr.name="violation" attr.type="boolean" for="node" id="violation"/>
<key attr.name="createThread" attr.type="string" for="edge" id="createThread"/>
<key attr.name="threadId" attr.type="string" for="edge" id="threadId"/>
<key attr.name="enterFunction" attr.type="string" for="edge" id="enterFunction"/>
<key attr.name="startline" attr.type="string" for="edge" id="startline"/>
<graph edgedefault="directed">
  <data key="witness-type">violation_witness</data>
  <data key="producer">ikos-conc</data>
  <data key="programfile">{programfile}</data>
  <data key="programhash">{programhash}</data>
  <data key="specification">CHECK( init(main()), LTL(G ! data-race) )</data>
  <data key="unroll-bound">2</data>
{nodes}
{edges}
</graph></graphml>
"""


def first_race(data):
    for rep in data.get("reports", []):
        info = rep.get("info", {})
        if info.get("verdict") == "unknown":
            continue
        if "access_a" in info and "access_b" in info:
            return info
    return None


def build_graphml(source, info, data_model):
    thread_creations = info.get("thread_creations", [])
    # thread_id map: main -> 0, k-th pthread_create (source order) -> k.
    tid = {"main": 0}
    for k, tc in enumerate(thread_creations, start=1):
        tid.setdefault(tc.get("thread_function"), k)
    creator_id = {tc.get("creator", "main"): tid.get(tc.get("creator", "main"), 0)
                  for tc in thread_creations}

    nodes = []
    edges = []
    nid = 0

    def node(extra=""):
        nonlocal nid
        nid += 1
        nodes.append(f'  <node id="N{nid - 1}">{extra}</node>')
        return nid - 1

    def edge(src, dst, attrs):
        body = "".join(f'<data key="{k}">{v}</data>' for k, v in attrs.items())
        edges.append(f'  <edge source="N{src}" target="N{dst}">{body}</edge>')

    entry = node('<data key="entry">true</data>')
    cur = entry

    # Spawn main (thread 0), enter main.
    nxt = node()
    edge(cur, nxt, {"createThread": "0"})
    cur = nxt
    nxt = node()
    edge(cur, nxt, {"enterFunction": "main", "threadId": "0"})
    cur = nxt

    # Spawn each child thread at its pthread_create call site.
    for tc in thread_creations:
        nxt = node()
        attrs = {
            "createThread": str(tid.get(tc.get("thread_function"), 1)),
            "threadId": str(creator_id.get(tc.get("creator", "main"), 0)),
        }
        if "line" in tc:
            attrs["startline"] = str(tc["line"])
        edge(cur, nxt, attrs)
        cur = nxt

    # The two racing accesses: the FIRST goes to an intermediate node, the
    # SECOND lands on the violation node (so the entry→violation path is
    # connected — Dartagnan's getPathToViolation walks backwards from the
    # violation node and requires an edge targeting it).
    a = info["access_a"]
    b = info["access_b"]
    for i, acc in enumerate((a, b)):
        is_last = (i == 1)
        nxt = node('<data key="violation">true</data>' if is_last else "")
        attrs = {"threadId": str(tid.get(acc.get("thread_id"), 0))}
        if "line" in acc:
            attrs["startline"] = str(acc["line"])
        edge(cur, nxt, attrs)
        cur = nxt

    file_name = os.path.basename(source)
    sha = hashlib.sha256(open(source, "rb").read()).hexdigest()
    return GRAPHML_TEMPLATE.format(
        programfile=os.path.abspath(source),
        programhash=sha,
        nodes="\n".join(nodes),
        edges="\n".join(edges),
    )


def run_ikos(ikos, source, data_model):
    machine = ["-m", "32" if data_model == "ILP32" else "64"]
    db = tempfile.mktemp(suffix=".db")
    report = tempfile.mktemp(suffix=".json")
    try:
        proc = subprocess.run(
            [ikos, "--analyses=race", "--concurrency=auto"] + machine +
            ["--format=json", "--report-file=" + report, "-o", db, source],
            capture_output=True, text=True)
    except FileNotFoundError:
        sys.stderr.write("error: ikos executable not found\n")
        return None
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        return None
    try:
        with open(report, encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError):
        return None
    finally:
        for p in (db, report, db + "-wal", db + "-shm"):
            try:
                os.remove(p)
            except OSError:
                pass
    return data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source")
    ap.add_argument("--ikos", default="ikos")
    ap.add_argument("--data-model", default="ILP32", choices=["LP64", "ILP32"])
    ap.add_argument("--out", default="witness.graphml")
    ap.add_argument("--dartagnan", default=None,
                    help="path to dartagnan.jar to validate the witness")
    ap.add_argument("--cat", default=None, help="CAT file (for --dartagnan)")
    args = ap.parse_args()

    data = run_ikos(args.ikos, args.source, args.data_model)
    if data is None:
        return 2
    info = first_race(data)
    if info is None:
        sys.stderr.write("no definite race found by IKOS\n")
        return 0

    graphml = build_graphml(args.source, info, args.data_model)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(graphml)
    print(f"witness written to {args.out}")

    if args.dartagnan:
        # Dartagnan's CAT parser resolves its stdlib includes relative to the
        # working directory, so run from the Dartagnan root (the dir containing
        # cat/ and dartagnan/target/dartagnan.jar).
        # jar = <dart_home>/dartagnan/target/dartagnan.jar → 3 levels up.
        dart_home = os.path.dirname(os.path.dirname(os.path.dirname(args.dartagnan)))
        env = dict(os.environ)
        env["CFLAGS"] = "-fgnu89-inline"
        env["DAT3M_HOME"] = dart_home
        env["DAT3M_OUTPUT"] = os.path.join(dart_home, "output")
        cat_rel = os.path.relpath(args.cat or os.path.join(dart_home, "cat",
                                                           "svcomp.cat"),
                                  dart_home)
        cmd = ["java", "-jar", os.path.join("dartagnan", "target", "dartagnan.jar"),
               cat_rel, "--target=C11", os.path.abspath(args.source),
               "--property=DATARACEFREEDOM", "--validate=" + os.path.abspath(args.out)]
        proc = subprocess.run(cmd, capture_output=True, text=True, env=env,
                              cwd=dart_home)
        for line in proc.stdout.splitlines():
            if "Result:" in line or "Reason:" in line:
                print("dartagnan:", line.strip())
        if proc.returncode != 0:
            print(proc.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
