#!/usr/bin/env python3
"""Check constant main completion separately from dynamic ThreadLifecycle."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sqlite3
import tempfile

from ikos_runtime import runtime_command

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ikos", type=Path, default=ROOT / "install/bin/ikos")
    parser.add_argument("--bits", type=int, choices=(32, 64))
    args = parser.parse_args()
    fixture = (ROOT / "analyzer/test/regression/bmc-values/constant-thread-completion.c").read_text()
    env = {k: v for k, v in os.environ.items()
           if not k.startswith("IKOS_") and k not in ("PYTHONPATH", "PYTHONHOME")}
    with tempfile.TemporaryDirectory(prefix="ikos constant completion ") as folder:
        work = Path(folder)
        for bits in ([args.bits] if args.bits else [32, 64]):
            for case in range(6):
                source, trace = work / "renamed.c", work / "rules.jsonl"
                source.write_text(f"#define CASE {case}\n" + fixture)
                if trace.exists():
                    trace.unlink()
                proc = subprocess.run(
                    runtime_command(args.ikos) + [
                        "--analyses=race", "--demote-race-to-unknown",
                        "--partitioning=guards", "--bmc-deadline-ms=0",
                        "-m" + str(bits), "-o", str(work / "output.db"), str(source)],
                    env=dict(env, IKOS_RACE_RULE_REPORT=str(trace)),
                    capture_output=True, text=True, timeout=30)
                assert proc.returncode == 0, (bits, case, proc.stdout, proc.stderr)
                if trace.exists():
                    rows = [json.loads(line) for line in trace.read_text().splitlines()]
                    assert len(rows) == 1, rows
                    hits = rows[0]["counts"].get("inactive-child-v1", 0)
                else:
                    with sqlite3.connect(work / "output.db") as db:
                        result = json.loads(db.execute(
                            "SELECT value FROM settings WHERE name='race_result'").fetchone()[0])
                    assert result["verdict"] == "UNKNOWN" and result["coverage"] == "Blocked", result
                    rows, hits = [], 0
                assert case != 0 or hits > 0, (bits, case, rows, proc.stdout)
                if case == 0:
                    assert "The program is SAFE" in proc.stdout, (bits, case, proc.stdout)
                else:
                    assert "The program is SAFE" not in proc.stdout, (bits, case, proc.stdout)
                print(f"constant-completion bits={bits} case={case} hits={hits}", flush=True)


if __name__ == "__main__":
    main()
