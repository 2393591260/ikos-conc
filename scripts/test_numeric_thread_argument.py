#!/usr/bin/env python3
"""Numerical pthread payloads need closed pointer uses in LLVM and AR."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from ikos_runtime import runtime_command
from analyzer.python.ikos.race_result import validate_race_result
from sweep_bmc_execution import validate_report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--bits', type=int, choices=(32, 64), default=32)
    args = parser.parse_args()
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/numeric-thread-argument.c').read_text()
    command = runtime_command(args.ikos.resolve())
    with tempfile.TemporaryDirectory(prefix='numeric-argument-') as folder:
        work = Path(folder)
        for wide in (False, True):
            for case in (0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 14, 15, 16, 17, 18, 20, 21):
                source, report = work / 'renamed.c', work / 'report.json'
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') + fixture)
                proc = subprocess.run(command + ['--analyses=race', '--concurrency=auto',
                    '--demote-race-to-unknown', '--partitioning=guards', f'-m{args.bits}',
                    '--format=json', '--report-file=' + str(report),
                    '-o', str(work / 'output.db'), str(source)],
                    capture_output=True, text=True, timeout=30,
                    env=dict(os.environ, IKOS_BMC_RECOVER='1'))
                assert proc.returncode == 0, (case, wide, proc.stdout, proc.stderr)
                data = json.loads(report.read_text())
                metadata = validate_race_result(data)
                # ILP32 truncates CASE15 before packing; LP64 preserves an invalid index.
                expected = ('SAFE' if case == 1 else
                            'FALSE' if case in (0, 16) or (case == 21 and args.bits == 32) or (case == 15 and args.bits == 32)
                            else 'UNKNOWN')
                assert metadata['verdict'] == expected, (case, wide, metadata, proc.stdout)
                executions = validate_report(data)
                if case == 15 and args.bits == 64:
                    assert not any(row['info'].get('counterexample')
                                   for row in data['reports']), data
                    assert not executions, executions
                elif case in (18, 21) and args.bits == 64:
                    assert metadata['verdict'] == 'UNKNOWN' and metadata['coverage'] == 'Checked', metadata
                    assert not metadata['obligations'] and not executions, metadata
                elif expected == 'UNKNOWN':
                    assert metadata['coverage'] == 'Blocked' and any(
                        o['reason'] == 'integer-derived pointer aliasing is not modeled'
                        and not o['contract'] for o in metadata['obligations']), metadata
                    assert not metadata['search_performed'] and not executions, metadata
                else:
                    assert metadata['coverage'] == 'Checked' and not metadata['obligations'], metadata
                    if expected == 'FALSE':
                        assert executions and all(e['status'] == 'ACCEPT' for e in executions), executions
                print(f'CASE={case}/wide={wide}: {expected}', flush=True)
    print(f'34 {args.bits}-bit numeric argument controls passed')


if __name__ == '__main__':
    main()
