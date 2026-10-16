#!/usr/bin/env python3
"""Finite counter closure must justify every fresh arena byte range."""
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
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/arena-reservation.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    with tempfile.TemporaryDirectory(prefix='ikos-arena-') as tmp:
        work = Path(tmp)
        for wide in (False, True):
            for case in range(41):
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') + fixture)
                proc = subprocess.run([str(args.ikos.resolve()), '--analyses=race',
                    '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                    '--bmc-deadline-ms=0', '-m32', '-o', str(work / 'output.db'), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                output = proc.stdout + proc.stderr
                assert proc.returncode == 0, (case, wide, output)
                with sqlite3.connect(work / 'output.db') as db:
                    rows = [json.loads(r[0]) for r in db.execute('SELECT info FROM checks')]
                proofs = [r for r in rows if r.get('arena_reservation_contract') == 'closed-finite-counter-slices-v1']
                positive = case in {0, 2, 18, 19, 34, 35} or (wide and case == 30)
                assert bool(proofs) == positive, (case, wide, proofs, output)
                if positive and case not in {34, 35}:
                    assert 'The program is SAFE' in output, (case, wide, output)
                if case in {34, 35}:
                    assert 'The program is SAFE' not in output, (case, wide, output)
                print(f'arena={case}/wide={wide}: proofs={len(proofs)}', flush=True)


if __name__ == '__main__':
    main()
