#!/usr/bin/env python3
"""Check pair tracing for disjoint bytes, overlapping bytes and multiplicity."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

from ikos_runtime import runtime_command

SOURCE = """#include <pthread.h>
struct cell { int a; int b; }; struct cell shared;
void *worker(void *arg) { shared.a=1; return 0; }
int main(void) { pthread_t a,b; pthread_create(&a,0,worker,0); MAIN
pthread_join(a,0); return 0; }
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ikos", type=Path, required=True)
    parser.add_argument("--bits", type=int, choices=(32, 64), default=32)
    args = parser.parse_args()
    entry = args.ikos
    cases = [
        ("disjoint", "shared.b=2;", True),
        ("overlap", "((char*)&shared.a)[1]=2;", False),
        ("multiplicity", "pthread_create(&b,0,worker,0); pthread_join(b,0);", False),
    ]
    with tempfile.TemporaryDirectory(prefix="ikos rule contracts ") as name:
        work = Path(name)
        for key, statement, safe in cases:
            source = work / "source.c"
            source.write_text(SOURCE.replace("MAIN", statement))
            report = work / (key + ".jsonl")
            env = dict(os.environ)
            env.pop("IKOS_BMC_RECOVER", None)
            env["IKOS_RACE_RULE_REPORT"] = str(report)
            proc = subprocess.run(
                [*runtime_command(entry), "--analyses=race", "--demote-race-to-unknown",
                 f"-m{args.bits}", "-o", str(work / "output.db"), str(source)],
                env=env, capture_output=True, text=True, timeout=30,
            )
            assert proc.returncode == 0, proc.stderr
            assert ("The program is SAFE" in proc.stdout) == safe, (key, proc.stdout)
            rows = [json.loads(line) for line in report.read_text().splitlines()]
            assert len(rows) == 1 and rows[0]["kind"] == "pair-rule-summary"
            assert (rows[0]["remaining_pairs"] == 0) == safe, (key, rows)
            if safe:
                assert rows[0]["counts"], rows
            print(key, safe, rows, flush=True)
    print("PASS rule trace and conservative overlap/multiplicity controls")


if __name__ == "__main__":
    main()
