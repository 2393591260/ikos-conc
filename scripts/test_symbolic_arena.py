#!/usr/bin/env python3
"""Inductive serialized heap slices, byte extents and complete loop frames."""
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
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/symbolic-arena.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    positives = {0, 19, 24, 25, 26}
    cases = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 14, 15, 16, 19, 20, 21, 22, 24, 25, 26, 29, 31, 32, 33, 34, 35, 36]
    with tempfile.TemporaryDirectory(prefix='ikos-symbolic-arena-') as temporary:
        work = Path(temporary)
        configurations = [(wide, case, 7, case in positives) for wide in (0, 1) for case in cases]
        configurations += [(wide, 0, chunk, chunk != 2147483647)
                           for wide in (0, 1) for chunk in (1, 13, 2147483647)]
        for wide, case, chunk, positive in configurations:
            source = work / 'renamed.c'
            source.write_text(f'#define CASE {case}\n#define WIDE {wide}\n#define CHUNK {chunk}\n#line 1\n' + fixture)
            proc = subprocess.run([str(args.ikos.resolve()), '--analyses=race',
                '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                '--bmc-deadline-ms=0', '-m32', '-o', str(work / 'output.db'), str(source)],
                env=env, capture_output=True, text=True, timeout=30)
            output = proc.stdout + proc.stderr
            assert proc.returncode == 0, (case, wide, output)
            with sqlite3.connect(work / 'output.db') as db:
                infos = [json.loads(r[0]) for r in db.execute('SELECT info FROM checks')]
            proofs = [r for r in infos if r.get('arena_reservation_contract') == 'closed-symbolic-bounded-slices-v1']
            assert bool(proofs) == positive, (case, wide, proofs, output)
            if positive:
                assert 'The program is SAFE' in output, (case, wide, output)
                assert all(r['element_bytes'] == (8 if wide else 4) for r in proofs)
            else:
                assert 'The program is SAFE' not in output, (case, wide, output)
            print(f'CASE={case}/WIDE={wide}/CHUNK={chunk}: proofs={len(proofs)}', flush=True)


if __name__ == '__main__':
    main()
