#!/usr/bin/env python3
"""A closed GNU semaphore permit protects both endpoints; library coverage alone does not."""
import argparse
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile
from test_allocation_continuations import wrapped

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    args = parser.parse_args()
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/posix-semaphore.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    with tempfile.TemporaryDirectory(prefix='ikos-semaphore-') as tmp:
        work = Path(tmp)
        for bits in (32, 64):
            for wide in (False, True):
                cases = [(case, variant) for case in range(19)
                         for variant in ('plain', 'abort')]
                cases += [(0, variant) for variant in
                          ('failure-write', 'return-null', 'failure-call',
                           'escaped-definition', 'unused-effect', 'guarded-size',
                           'repeat-allocation')]
                for case, variant in cases:
                    body = fixture if variant == 'plain' else wrapped(fixture, variant)
                    if variant == 'guarded-size':
                        body = body.replace('pthread_t *ids = malloc(n * sizeof *ids);',
                            'if ((unsigned)n > (size_t)-1 / sizeof(pthread_t)) abort(); '
                            'pthread_t *ids = malloc(n * sizeof *ids);')
                    elif variant == 'repeat-allocation':
                        body = body.replace('pthread_t *ids = malloc(n * sizeof *ids);',
                            'pthread_t *ids; do { ids = malloc(n * sizeof *ids); } '
                            'while (__VERIFIER_nondet_int());')
                    source = work / 'renamed.c'
                    source.write_text(f'#define CASE {case}\n' +
                                      ('#define WIDE\n' if wide else '') + body)
                    proc = subprocess.run([str(args.ikos.resolve()), '--analyses=race',
                        '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                        '--bmc-deadline-ms=0', f'-m{bits}', '-o', str(work / 'output.db'), str(source)],
                        env=env, capture_output=True, text=True, timeout=30)
                    output = proc.stdout + proc.stderr
                    assert proc.returncode == 0, (case, bits, wide, variant, output)
                    with sqlite3.connect(work / 'output.db') as db:
                        rows = [json.loads(r[0]) for r in db.execute('SELECT info FROM checks')]
                    proofs = [r for r in rows if r.get('semaphore_ownership_contract')]
                    supported = bits == 32 and variant in ('plain', 'abort', 'unused-effect', 'guarded-size')
                    positive = supported and case in {0, 6, 15}
                    assert bool(proofs) == positive, (case, bits, wide, variant, proofs, output)
                    if supported and case == 2:
                        assert any(r.get('semaphore_effect_contract') for r in rows), (case, wide, rows)
                    assert ('The program is SAFE' in output) == (supported and case in {0, 6}), (
                        case, bits, wide, variant, output)
                    print(f'semaphore={case}/bits={bits}/wide={wide}/{variant}: proofs={len(proofs)}', flush=True)


if __name__ == '__main__':
    main()
