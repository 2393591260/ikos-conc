#!/usr/bin/env python3
"""Check shared heap initialization effects and preserve real conflicting pairs."""
import argparse
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile

from ikos_runtime import runtime_command, runtime_identity, verify_runtime
from test_allocation_continuations import wrapped

ROOT = Path(__file__).resolve().parents[1]
CASES = tuple(range(25)) + (26, 27, 28, 29, 30, 31)
POSITIVE = {0, 20, 21, 27}
# These source frames remain valid; their data accesses genuinely conflict.
RACY = {1, 2}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--case', type=int, choices=CASES, action='append')
    args = parser.parse_args()
    entry = args.ikos.resolve()
    identity = runtime_identity(entry)
    command = runtime_command(entry)
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/heap-pthread-initialization.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    with tempfile.TemporaryDirectory(prefix='ikos-heap-pthread-init-') as temp:
        work = Path(temp)
        for bits in (32, 64):
            for wide in (False, True):
                cases = [(case, variant) for case in (CASES if args.case is None else args.case)
                         for variant in ('plain', 'abort')]
                if args.case is None or 0 in args.case:
                    cases += [(0, variant) for variant in
                              ('failure-write', 'return-null', 'failure-call', 'repeat-allocation',
                               'reverse-null', 'neq', 'two-allocations',
                               'post-create-check', 'post-create-byte-check', 'post-create-failure-write', 'alternate-ingress')]
                for case, variant in cases:
                    body = fixture if variant == 'plain' else wrapped(fixture, variant)
                    if variant in ('post-create-check', 'post-create-byte-check', 'post-create-failure-write', 'alternate-ingress'):
                        body = fixture.replace('cells = malloc(2 * sizeof(struct cell));',
                            'struct cell *pending = malloc(2 * sizeof(struct cell)); cells = pending;')
                        if variant in ('post-create-byte-check', 'post-create-failure-write'):
                            # Preserve the actual malloc result, without a typed-pointer round trip.
                            body = body.replace('struct cell *pending = malloc', 'void *pending = malloc')
                        if variant == 'alternate-ingress':
                            body = body.replace('initialize(cells, 1);',
                                'extern int __VERIFIER_nondet_int(void); '
                                'if (__VERIFIER_nondet_int()) goto resumed; '
                                'if (pending == NULL) abort(); resumed: initialize(cells, 1);')
                        else:
                            failure = 'cells[1].value++; abort();' if variant.endswith('failure-write') else 'abort();'
                            body = body.replace('pthread_create(&thread, 0, worker, 0);',
                                'pthread_create(&thread, 0, worker, 0); '
                                'if (' + ('(void *)pending' if variant != 'post-create-check' else 'pending') + ' == NULL) { ' + failure + ' }')
                    if variant == 'reverse-null':
                        body = body.replace('result == NULL', 'NULL == result')
                    elif variant == 'neq':
                        body = body.replace('if (result == NULL) { abort(); }',
                                            'if (result != NULL) { } else { abort(); }')
                    elif variant == 'two-allocations':
                        body = body.replace('cells = malloc(2 * sizeof(struct cell));',
                                            'int *scratch = malloc(sizeof(int)); *scratch = 1; '
                                            'cells = malloc(2 * sizeof(struct cell));')
                    if variant == 'repeat-allocation':
                        body = body.replace('cells = malloc(2 * sizeof(struct cell));',
                            'extern int __VERIFIER_nondet_int(void); '
                            'do { cells = malloc(2 * sizeof(struct cell)); } '
                            'while (__VERIFIER_nondet_int());')
                    source = work / 'renamed.c'
                    source.write_text(f'#define CASE {case}\n' +
                                      ('#define WIDE\n' if wide else '') + body)
                    database = work / 'output.db'
                    result = subprocess.run(command + ['--analyses=race',
                        '--concurrency=auto', '--partitioning=guards',
                        '--demote-race-to-unknown', '--bmc-deadline-ms=0',
                        '--log=debug', f'-m{bits}', '-o', str(database), str(source)],
                        env=env, capture_output=True, text=True, timeout=30)
                    output = result.stdout + result.stderr
                    assert result.returncode == 0, (case, bits, wide, variant, output)
                    with sqlite3.connect(database) as db:
                        infos = [json.loads(row) for row, in db.execute('SELECT info FROM checks')]
                    proofs = [row for row in infos if row.get('pthread_initialization_contract') ==
                              'constant-heap-runtime-byte-frame-v1']
                    safe = 'The program is SAFE' in output
                    supported = bits == 32 and variant in ('plain', 'abort', 'reverse-null', 'neq', 'two-allocations', 'post-create-byte-check')
                    if supported and case in POSITIVE:
                        assert proofs and safe, (case, bits, wide, variant, infos, output)
                    elif supported and case in RACY:
                        assert proofs and not safe, (case, bits, wide, variant, infos, output)
                    elif bits == 64 and case == 21 and variant in ('plain', 'abort'):
                        # Existing default-normal mutex reasoning already proves this control.
                        assert not proofs and safe, (case, bits, wide, variant, infos, output)
                    else:
                        assert not proofs and not safe, (case, bits, wide, variant, infos, output)
                    print(f'CASE={case}/bits={bits}/WIDE={int(wide)}/{variant}: safe={safe}, '
                          f'proofs={len(proofs)}', flush=True)
    verify_runtime(identity)


if __name__ == '__main__':
    main()
