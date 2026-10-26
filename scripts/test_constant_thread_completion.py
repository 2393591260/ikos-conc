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
                observations = work / "pairs.jsonl"
                source.write_text(f"#define CASE {case}\n" + fixture)
                for output in (trace, observations):
                    output.unlink(missing_ok=True)
                proc = subprocess.run(
                    runtime_command(args.ikos) + [
                        "--analyses=race", "--demote-race-to-unknown",
                        "--partitioning=guards", "--bmc-deadline-ms=0",
                        "-m" + str(bits), "-o", str(work / "output.db"), str(source)],
                    env=dict(env, IKOS_RACE_RULE_REPORT=str(trace),
                             IKOS_RACE_PAIR_OBSERVATIONS=str(observations)),
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
                pair_rows = ([json.loads(line) for line in observations.read_text().splitlines()]
                             if observations.exists() else [])
                details = [row for row in pair_rows if row['kind'] == 'pair-observation-v1']

                def main_read_worker_write(row):
                    for main, worker in ((row['a'], row['b']), (row['b'], row['a'])):
                        if (main['thread'] == 'main' and not main['write'] and
                                worker['thread'] == 'worker' and worker['write'] and
                                all('payload' in side['points_to_snapshot']
                                    for side in (main, worker))):
                            return True
                    return False

                payload_pairs = [row for row in details if main_read_worker_write(row)]
                if payload_pairs:
                    summaries = [row for row in pair_rows
                                 if row['kind'] == 'pair-observation-summary-v1']
                    assert not summaries, ('target observations truncated', summaries)
                    expected_rule = 'inactive-child-v1' if case == 0 else 'none-v1'
                    assert all(row['first_rule'] == expected_rule for row in payload_pairs), (
                        bits, case, expected_rule, payload_pairs)
                    print(f'constant-completion target bits={bits} case={case} '
                          f'main-read/worker-write={len(payload_pairs)} rule={expected_rule}', flush=True)
                else:
                    # Unsupported handle identity may stop before pair enumeration.
                    # These boundaries cannot count as observed consumer withdrawal.
                    assert case in (2, 3) and not hits, (bits, case, rows, pair_rows, proc.stdout)
                    with sqlite3.connect(work / "output.db") as db:
                        result = json.loads(db.execute(
                            "SELECT value FROM settings WHERE name='race_result'").fetchone()[0])
                    assert result["verdict"] == "UNKNOWN" and result["coverage"] == "Blocked", result
                    print(f'constant-completion target bits={bits} case={case} '
                          'not-enumerated/coverage-blocked (boundary control)', flush=True)
                if case == 0:
                    assert "The program is SAFE" in proc.stdout, (bits, case, proc.stdout)
                else:
                    assert "The program is SAFE" not in proc.stdout, (bits, case, proc.stdout)
                print(f"constant-completion bits={bits} case={case} hits={hits}", flush=True)


if __name__ == "__main__":
    main()
