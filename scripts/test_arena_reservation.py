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
    parser.add_argument('--bits', type=int, choices=(32, 64), default=32)
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
                    '--bmc-deadline-ms=0', '-m' + str(args.bits), '-o', str(work / 'output.db'), str(source)],
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


        if args.bits == 64:
            for name, index, adjacent in (
                ('zero-extension', '(unsigned int)token', '(unsigned int)token + 1'),
                ('truncation', '(short)token', '(short)(token + 1)'),
                ('wide-arithmetic', '(long long)token + 2', '(long long)token + 3'),
            ):
                text = fixture.replace('storage[token + 1]', 'storage[' + adjacent + ']')
                text = text.replace('storage[token]', 'storage[' + index + ']')
                source = work / 'unsupported-index.c'
                source.write_text('#define CASE 0\n' + text)
                proc = subprocess.run([str(args.ikos.resolve()), '--analyses=race',
                    '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                    '--bmc-deadline-ms=0', '-m64', '-o', str(work / 'output.db'), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                assert proc.returncode == 0, (name, proc.stdout, proc.stderr)
                with sqlite3.connect(work / 'output.db') as db:
                    proofs = list(db.execute(
                        "SELECT info FROM checks WHERE info LIKE '%arena_reservation_contract%'"))
                assert not proofs and 'The program is SAFE' not in proc.stdout, (
                    name, proofs, proc.stdout, proc.stderr)
                print(f'arena native-index control={name}: rejected', flush=True)


if __name__ == '__main__':
    main()
