#!/usr/bin/env python3
"""One-shot barriers order opposite phases without hiding ordinary races."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

from ikos_runtime import runtime_command, runtime_identity, verify_runtime

ROOT = Path(__file__).resolve().parents[1]
CASES = tuple(range(0, 20)) + (20, 21, 22, 23, 24, 25, 27, 28, 29, 30, 31, 32, 33, 34) + tuple(range(35, 45))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--case', type=int, choices=CASES, action='append')
    args = parser.parse_args()
    identity = runtime_identity(args.ikos)
    command = runtime_command(args.ikos)
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/barrier-phases.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    with tempfile.TemporaryDirectory(prefix='ikos-barrier-phases-') as temp:
        work = Path(temp)
        for bits in (32, 64):
            for wide in (False, True):
                for case in (CASES if args.case is None else args.case):
                    source = work / 'renamed.c'
                    source.write_text(f'#define CASE {case}\n' +
                                      ('#define WIDE\n' if wide else '') + fixture)
                    report = work / 'rules.jsonl'
                    report.unlink(missing_ok=True)
                    env['IKOS_RACE_RULE_REPORT'] = str(report)
                    proc = subprocess.run(command + ['--analyses=race', '--concurrency=auto',
                        '--partitioning=guards', '--demote-race-to-unknown',
                        '--bmc-deadline-ms=0', f'-m{bits}', '-o',
                        str(work / 'output.db'), str(source)], env=env,
                        capture_output=True, text=True, timeout=30)
                    output = proc.stdout + proc.stderr
                    assert proc.returncode == 0, (case, bits, wide, output)
                    safe = 'The program is SAFE' in output
                    assert safe == (case in (0, 1, 5, 6, 9, 33, 35, 36, 41, 43, 44)), (case, bits, wide, output)
                    rows = [json.loads(line) for line in report.read_text().splitlines()] if report.exists() else []
                    if safe:
                        assert any(row.get('counts', {}).get('one-shot-barrier-phases-v1', 0) for row in rows), (case, rows)
                    if case in (10, 11, 12, 13, 14, 15, 17, 18, 19, 20, 21, 22, 23, 24, 25, 29, 30, 37, 38, 39, 40, 42):
                        assert not any(row.get('counts', {}).get('one-shot-barrier-phases-v1', 0) for row in rows), (case, rows)
                    print(f'barrier phase={case}/bits={bits}/wide={wide}: safe={safe}, rules={rows}',
                          flush=True)
    verify_runtime(identity)


if __name__ == '__main__':
    main()
