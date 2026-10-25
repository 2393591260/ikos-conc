#!/usr/bin/env python3
"""Atomic token transitions need real per-thread ownership at both accesses."""
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
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/binary-token.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    with tempfile.TemporaryDirectory(prefix='ikos-binary-token-') as tmp:
        work = Path(tmp)
        for wide in (False, True):
            for case in range(37):
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
                proofs = [r for r in rows if r.get('binary_token_contract')]
                positive = case in {0, 1, 2, 3, 23, 24, 29, 32, 33, 34}
                assert bool(proofs) == positive, (case, wide, proofs, output)
                safe = positive and case not in {23, 24}
                if safe:
                    assert 'The program is SAFE' in output, (case, wide, output)
                if case in {4, 5, 6, 7, 8, 9, 11, 12, 13, 15, 16, 17, 19, 20, 21, 22, 23, 24, 26, 27, 28, 30, 31, 35}:
                    assert 'The program is SAFE' not in output, (case, wide, proofs, output)
                print(f'binary token={case}/wide={wide}: proofs={len(proofs)}', flush=True)


        # The same token proof must close unrelated runtime calls through the
        # actual mutex layout, without assuming arbitrary mutex bytes are normal.
        for name, declaration, admitted in (
            ('normal', 'pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;', True),
            ('unaligned', 'pthread_mutex_t gate __attribute__((aligned(1))) = PTHREAD_MUTEX_INITIALIZER;', False),
            ('nonzero', 'pthread_mutex_t gate = { .__data = { .__kind = 2 } };', False),
        ):
            text = fixture.replace('#include <stdlib.h>',
                '#include <stdlib.h>\n' + declaration).replace(
                'static void* worker(void* ignored) {',
                'static void* worker(void* ignored) {\n'
                'pthread_mutex_lock(&gate); pthread_mutex_unlock(&gate);')
            source = work / 'mutex-layout.c'
            source.write_text(text)
            proc = subprocess.run([str(args.ikos.resolve()), '--analyses=race',
                '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                '--bmc-deadline-ms=0', '-m' + str(args.bits),
                '-o', str(work / 'output.db'), str(source)],
                env=env, capture_output=True, text=True, timeout=30)
            output = proc.stdout + proc.stderr
            assert proc.returncode == 0, (name, args.bits, output)
            with sqlite3.connect(work / 'output.db') as db:
                proofs = list(db.execute(
                    "SELECT info FROM checks WHERE info LIKE '%binary_token_contract%'"))
            assert bool(proofs) == admitted, (name, args.bits, proofs, output)
            assert ('The program is SAFE' in output) == admitted, (name, args.bits, output)
            print(f'mutex layout={name}/bits={args.bits}: admitted={admitted}', flush=True)


if __name__ == '__main__':
    main()
