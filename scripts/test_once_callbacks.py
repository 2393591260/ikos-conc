#!/usr/bin/env python3
"""Closed once alternatives preserve callback coverage and ordinary races."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

from ikos_runtime import runtime_command, runtime_identity, verify_runtime

ROOT = Path(__file__).resolve().parents[1]
CASES = (0, 1, 2, 3, 4, 10, 11, 12, 13, 14, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--case', type=int, choices=CASES, action='append')
    args = parser.parse_args()
    identity = runtime_identity(args.ikos)
    command = runtime_command(args.ikos)
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/once-callbacks.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    with tempfile.TemporaryDirectory(prefix='ikos-once-callbacks-') as temp:
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
                    assert safe == (case in (0, 1, 2, 3, 4)), (case, bits, wide, output)
                    rows = [json.loads(line) for line in report.read_text().splitlines()] if report.exists() else []
                    if safe:
                        assert any(row.get('counts', {}).get('same-closed-once-v1', 0) for row in rows), (case, rows)
                    if case in (12, 13, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31):
                        assert not any(row.get('counts', {}).get('same-closed-once-v1', 0) for row in rows), (case, rows)
                    print(f'once callback={case}/bits={bits}/wide={wide}: safe={safe}, rules={rows}',
                          flush=True)
    verify_runtime(identity)


if __name__ == '__main__':
    main()
