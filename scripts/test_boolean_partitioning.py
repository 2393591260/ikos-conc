#!/usr/bin/env python3
"""Check explicit Boolean partitions without changing legacy strategies."""
import argparse
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    args = parser.parse_args()
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/boolean-guards.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    positives = {0, 4, 5, 8}
    with tempfile.TemporaryDirectory(prefix='ikos-boolean-guards-') as temp:
        work = Path(temp)
        for wide in (False, True):
            for case in range(10):
                for strategy in (('no', 'manual', 'return', 'guards') if case == 0 else ('guards',)):
                    source = work / 'renamed.c'
                    text = fixture.replace('check_condition', 'require_value')
                    source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') + text)
                    report = work / 'report.json'
                    database = work / 'output.db'
                    proc = subprocess.run([str(args.ikos.resolve()), '--analyses=prover',
                        '--partitioning=' + strategy, '-m32', '--format=json',
                        '--status-filter=*', '--report-file=' + str(report),
                        '-o', str(database), str(source)], env=env,
                        capture_output=True, text=True, timeout=30)
                    assert proc.returncode == 0, (case, wide, strategy, proc.stdout, proc.stderr)
                    reports = json.loads(report.read_text())['reports']
                    assert reports, (case, wide, strategy, proc.stdout)
                    proven = all(row['status'] == 0 for row in reports)
                    expected = case in positives and strategy == 'guards'
                    assert proven == expected, (case, wide, strategy, reports, proc.stdout)
                    with sqlite3.connect(database) as db:
                        settings = dict(db.execute('SELECT name,value FROM settings'))
                    assert settings['auto-boolean-partitioning'] == str(strategy == 'guards').lower()
                    print(f'guard case={case}/wide={wide}/{strategy}: proven={proven}', flush=True)


        owner_fixture = (ROOT / 'analyzer/test/regression/bmc-values/guarded-ownership.c').read_text()
        for wide in (False, True):
            for case in range(16):
                source = work / 'renamed-owner.c'
                text = owner_fixture.replace('require_condition', 'guard_value')
                text = text.replace('make_element', 'build_node')
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') + text)
                report = work / 'owner.json'
                proc = subprocess.run([str(args.ikos.resolve()), '--analyses=race',
                    '--concurrency=auto', '--demote-race-to-unknown', '--bmc-deadline-ms=0',
                    '--partitioning=guards', '-m32', '--format=json', '--status-filter=*',
                    '--report-file=' + str(report), '-o', str(work / 'owner.db'), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                assert proc.returncode == 0, (case, wide, proc.stdout, proc.stderr)
                proven = 'The program is SAFE' in proc.stdout
                expected = case in {0, 10, 11}
                assert proven == expected, (case, wide, proc.stdout, proc.stderr)
                proofs = [r for r in json.loads(report.read_text())['reports']
                          if r['info'].get('ownership_contract') == 'indexed-live-heap-owner-v1']
                assert bool(proofs) == expected, (case, wide, proofs, proc.stdout)
                print(f'guarded owner case={case}/wide={wide}: proven={proven}; proofs={len(proofs)}', flush=True)


if __name__ == '__main__':
    main()
