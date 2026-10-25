#!/usr/bin/env python3
"""Only a proved terminal zero branch can replace syntactic edge dominance."""
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
    parser.add_argument('--bits', type=int, choices=(32, 64), default=32)
    args = parser.parse_args()
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/zero-guard-arena.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    with tempfile.TemporaryDirectory(prefix='ikos-zero-guard-') as tmp:
        work = Path(tmp)
        for wide in (False, True):
            for case in range(24):
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') + fixture)
                proc = subprocess.run([str(args.ikos.resolve()), '--analyses=race',
                    '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                    '--bmc-deadline-ms=0', '-m' + str(args.bits), '-o', str(work / 'output.db'), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                output = proc.stdout + proc.stderr
                assert proc.returncode == 0, (case, wide, output)
                with sqlite3.connect(work / 'output.db') as db:
                    rows = [json.loads(r[0]) for r in db.execute('SELECT info FROM checks')]
                proofs = [r for r in rows if r.get('arena_reservation_contract')]
                positive = case in {0, 1, 2, 7, 12, 16, 17, 18, 19, 22}
                if case != 8:
                    assert bool(proofs) == positive, (case, wide, proofs, output)
                safe = positive and case != 19
                assert ('The program is SAFE' in output) == safe, (case, wide, proofs, output)
                print(f'zero guard={case}/wide={wide}: proofs={len(proofs)}', flush=True)


if __name__ == '__main__':
    main()
