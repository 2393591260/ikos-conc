#!/usr/bin/env python3
"""Continuous mutex intervals, including loop history reset and helper reuse."""
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
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/mutex-handover.c').read_text()
    target_line = next(i for i, line in enumerate(fixture.splitlines(), 3) if line.strip() == 'value = 2;')
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    with tempfile.TemporaryDirectory(prefix='ikos-mutex-handover-') as temporary:
        work = Path(temporary)
        for wide in (0, 1):
            for case in (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13, 14, 15, 17, 18, 19, 20, 21, 26, 27, 28, 29, 30):
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n#define WIDE {wide}\n' + fixture)
                proc = subprocess.run([str(args.ikos.resolve()), '--analyses=race',
                    '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                    '--bmc-deadline-ms=0', '-m32', '-o', str(work / 'output.db'), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                output = proc.stdout + proc.stderr
                assert proc.returncode == 0, (case, wide, output)
                with sqlite3.connect(work / 'output.db') as db:
                    infos = [json.loads(r[0]) for r in db.execute('SELECT info FROM checks')]
                proofs = [r for r in infos if r.get('mutex_handover_contract')]
                positive = case in {0, 10, 12, 29, 30}
                target_proofs = [r for r in proofs if target_line in (r.get('access_a_line'), r.get('access_b_line'))]
                assert bool(target_proofs) == positive, (case, wide, proofs, output)
                if positive:
                    assert 'The program is SAFE' in output, (case, wide, output)
                if case in {1, 2, 3, 4, 5, 6, 7, 13, 14, 15}:
                    assert 'The program is SAFE' not in output, (case, wide, output)
                print(f'CASE={case}/WIDE={wide}: proofs={len(proofs)}', flush=True)


if __name__ == '__main__':
    main()
