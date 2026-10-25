#!/usr/bin/env python3
"""Only a real locked observation of a closed monotone flag orders payload reads."""
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
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/guarded-publication.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    with tempfile.TemporaryDirectory(prefix='ikos-guarded-publication-') as tmp:
        work = Path(tmp)
        for wide in (False, True):
            for case in range(32):
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
                proofs = [r for r in rows if r.get('guarded_publication_contract')]
                positive = case in {0, 17, 19, 21, 22, 23, 24, 25, 26, 27}
                assert bool(proofs) == positive, (case, wide, proofs, output)
                if positive:
                    assert 'The program is SAFE' in output, (case, wide, output)
                if case in {1, 2, 3, 4, 6, 8, 18, 20, 29, 30}:
                    assert 'The program is SAFE' not in output, (case, wide, output)
                print(f'guarded publication={case}/wide={wide}: proofs={len(proofs)}', flush=True)


        declaration = 'static pthread_cond_t wakeup = PTHREAD_COND_INITIALIZER;'
        variants = {
            'unaligned-cond': fixture.replace(declaration,
                'static pthread_cond_t wakeup __attribute__((aligned(1))) = PTHREAD_COND_INITIALIZER;'),
            'short-cond': fixture.replace(declaration,
                'static unsigned char wakeup_storage[sizeof(pthread_cond_t)-1];\n'
                '#define wakeup (*(pthread_cond_t *)wakeup_storage)'),
            'wrong-size-t': fixture.replace('#include <stdlib.h>',
                'extern void abort(void); extern void free(void *);\n'
                'extern void *malloc(' +
                ('unsigned long long' if args.bits == 32 else 'unsigned int') + ');'),
        }
        for name, text in variants.items():
            source = work / 'broken-abi.c'
            source.write_text('#define CASE 0\n' + text)
            proc = subprocess.run([str(args.ikos.resolve()), '--analyses=race',
                '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                '--bmc-deadline-ms=0', '-m' + str(args.bits),
                '-o', str(work / 'output.db'), str(source)],
                env=env, capture_output=True, text=True, timeout=30)
            assert proc.returncode == 0, (name, proc.stdout, proc.stderr)
            with sqlite3.connect(work / 'output.db') as db:
                proofs = list(db.execute(
                    "SELECT info FROM checks WHERE info LIKE '%guarded_publication_contract%'"))
            assert not proofs and 'The program is SAFE' not in proc.stdout, (
                name, args.bits, proofs, proc.stdout, proc.stderr)
            print(f'guarded ABI control={name}/bits={args.bits}: rejected', flush=True)


if __name__ == '__main__':
    main()
