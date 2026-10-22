#!/usr/bin/env python3
"""Check heap proofs across allocation failure branches, with BMC disabled."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
sys.path.insert(0, str(ROOT))
from svcomp_race_test import classify, VERDICT_SAFE, VERDICT_UNKNOWN
from ikos_runtime import runtime_command

FIXTURES = {
    'first-publication.c': tuple(range(22)),
    'heap-fields.c': (0, 1, 4, 5, 15, 16),
    'heap-handoff.c': (0, 1, 2, 4, 6, 7, 8, 9, 13, 14, 16, 22, 28),
    'bucket-list-ownership.c': (0, 1, 5, 9, 15, 20, 21, 25, 28, 49, 53),
    'constructor-fields.c': (0, 1, 3, 6, 9, 10, 11),
    'helper-prefix-fields.c': (0, 2, 3, 6, 7, 14, 16, 19, 20),
}


def wrapped(body, variant='abort'):
    # Place the macro after system headers; the helper calls genuine malloc.
    lines = body.splitlines(keepends=True)
    insertion = max(i for i, line in enumerate(lines) if line.startswith('#include')) + 1
    failure = 'abort();'
    declarations = ''
    if variant == 'failure-write':
        declarations = 'int allocation_fault;'
        failure = 'allocation_fault++; abort();'
        lines = [line.replace('pthread_create(&thread, 0, worker, 0);',
                              'pthread_create(&thread, 0, worker, 0); allocation_fault++;')
                 for line in lines]
    elif variant == 'return-null':
        failure = 'return NULL;'
    elif variant == 'shared-fallback':
        insertion = next(i for i, line in enumerate(lines) if line.startswith('struct node {')) + 1
        declarations = 'static struct node fallback;'
        failure = 'return &fallback;'
    if variant in ('alternate-ingress', 'prefix-loop'):
        replacement = ('struct cell *p; if (__VERIFIER_nondet_int()) p = head; '
                       'else p = malloc(sizeof *p);') if variant == 'alternate-ingress' else (
                       'struct cell *p = malloc(sizeof *p); '
                       'while (__VERIFIER_nondet_int()) p->datum = 7;')
        lines = [line.replace('struct cell *p = malloc(sizeof *p);', replacement)
                 for line in lines]
    lines.insert(insertion, f"""
{declarations}
static void *checked_allocate(size_t size) {{
    void *result = malloc(size);
    if (result == NULL) {{ {failure} }}
    return result;
}}
#define malloc checked_allocate
""")
    return ''.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--bits', type=int, choices=(32, 64), nargs='+', default=[32, 64])
    parser.add_argument('--fixture', choices=tuple(FIXTURES), action='append')
    args = parser.parse_args()
    env = {k: v for k, v in os.environ.items()
           if k not in ('IKOS_BMC_RECOVER', 'PYTHONPATH', 'PYTHONHOME')}
    with tempfile.TemporaryDirectory(prefix='ikos-allocation-continuation-') as temp:
        work = Path(temp)
        for bits in args.bits:
            for wide in (False, True):
                cases = [(name, case, 'abort') for name, cases in FIXTURES.items()
                         if args.fixture is None or name in args.fixture
                         for case in cases]
                if args.fixture is None or 'heap-fields.c' in args.fixture:
                    cases += [('heap-fields.c', 0, variant)
                              for variant in ('failure-write', 'shared-fallback')]
                if args.fixture is None or 'heap-handoff.c' in args.fixture:
                    cases += [('heap-handoff.c', 0, variant)
                              for variant in ('failure-write', 'return-null')]
                if args.fixture is None or 'bucket-list-ownership.c' in args.fixture:
                    cases += [('bucket-list-ownership.c', 0, variant)
                              for variant in ('failure-write', 'return-null')]
                if args.fixture is None or 'first-publication.c' in args.fixture:
                    cases += [('first-publication.c', 0, variant)
                              for variant in ('failure-write', 'return-null',
                                              'alternate-ingress', 'prefix-loop')]
                for name, case, variant in cases:
                    body = (ROOT / 'analyzer/test/regression/bmc-values' / name).read_text()
                    source = work / 'renamed.c'
                    source.write_text(f'#define CASE {case}\n' +
                                      ('#define WIDE\n' if wide else '') + wrapped(body, variant))
                    command = runtime_command(args.ikos.resolve()) + [
                        '--analyses=race', '--concurrency=auto', '--partitioning=guards',
                        '--demote-race-to-unknown', '--bmc-deadline-ms=0',
                        f'-m{bits}', '-o', str(work / 'out.db'), str(source)]
                    run = subprocess.run(command, env=env, capture_output=True,
                                         text=True, timeout=30)
                    assert run.returncode == 0, (name, case, bits, run.stdout, run.stderr)
                    verdict = classify(run.stdout, run.returncode, False)
                    if (name in ('bucket-list-ownership.c', 'heap-handoff.c', 'first-publication.c') or
                            (name == 'helper-prefix-fields.c' and case == 0)) and bits == 64:
                        # Dynamic list/handle coverage is still unsupported in LP64;
                        # the unwrapped baseline has the same unresolved effects.
                        assert verdict == VERDICT_UNKNOWN, (name, bits, wide, verdict)
                    elif variant == 'abort' and (case == 0 or
                            (name == 'first-publication.c' and case in (16, 20))):
                        assert verdict == VERDICT_SAFE, (name, bits, wide, verdict,
                                                        run.stdout, run.stderr)
                    else:
                        assert verdict == VERDICT_UNKNOWN, (name, case, bits, wide, verdict,
                                                        run.stdout, run.stderr)
                    print(f'{name}: case={case}/variant={variant}/bits={bits}/wide={wide}: {verdict}', flush=True)


if __name__ == '__main__':
    main()
