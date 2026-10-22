#!/usr/bin/env python3
"""Actual creation/lock ordering preserves parent and sibling races."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

from ikos_runtime import runtime_command, runtime_identity, verify_runtime

ROOT = Path(__file__).resolve().parents[1]
CASES = tuple(i for i in range(30) if i != 4) + tuple(range(30, 42)) + (44, 45, 46, 47) + tuple(range(48, 65))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--case', type=int, choices=CASES, action='append')
    args = parser.parse_args()
    identity = runtime_identity(args.ikos)
    command = runtime_command(args.ikos)
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/locked-creation.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    with tempfile.TemporaryDirectory(prefix='ikos-locked-creation-') as temp:
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
                    if case in (0, 1, 2, 3, 24, 27, 30, 32, 37, 46, 47, 48, 59):
                        assert safe, (case, bits, wide, output)
                    elif case not in (16, 17, 18, 20, 21, 22, 49, 53, 55, 57, 61, 63, 64):
                        assert not safe, (case, bits, wide, output)
                    rows = [json.loads(line) for line in report.read_text().splitlines()] if report.exists() else []
                    if case in (0, 1, 2, 3, 24, 27, 30, 32, 37, 46, 47, 48, 59):
                        assert any(row.get('counts', {}).get('locked-creation-v1', 0) for row in rows), (case, rows)
                    if case in (16, 17, 18, 20, 21, 22, 29, 39, 49, 52, 53, 54, 55, 57, 60, 61, 63, 64):
                        assert not any(row.get('counts', {}).get('locked-creation-v1', 0) for row in rows), (case, rows)
                    print(f'locked creation={case}/bits={bits}/wide={wide}: safe={safe}, rules={rows}',
                          flush=True)
    verify_runtime(identity)


if __name__ == '__main__':
    main()
