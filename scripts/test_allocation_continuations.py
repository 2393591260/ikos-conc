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
    'heap-fields.c': (0, 1, 4, 5, 15, 16),
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
    elif variant == 'shared-fallback':
        insertion = next(i for i, line in enumerate(lines) if line.startswith('struct node {')) + 1
        declarations = 'static struct node fallback;'
        failure = 'return &fallback;'
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
    args = parser.parse_args()
    env = {k: v for k, v in os.environ.items()
           if k not in ('IKOS_BMC_RECOVER', 'PYTHONPATH', 'PYTHONHOME')}
    with tempfile.TemporaryDirectory(prefix='ikos-allocation-continuation-') as temp:
        work = Path(temp)
        for bits in args.bits:
            for wide in (False, True):
                cases = [(name, case, 'abort') for name, cases in FIXTURES.items()
                         for case in cases]
                cases += [('heap-fields.c', 0, variant)
                          for variant in ('failure-write', 'shared-fallback')]
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
                    if name == 'helper-prefix-fields.c' and bits == 64 and case == 0:
                        # Dynamic list/handle coverage is still unsupported in LP64;
                        # the unwrapped baseline has the same unresolved effects.
                        assert verdict == VERDICT_UNKNOWN, (name, bits, wide, verdict)
                    elif case == 0 and variant == 'abort':
                        assert verdict == VERDICT_SAFE, (name, bits, wide, verdict,
                                                        run.stdout, run.stderr)
                    else:
                        assert verdict == VERDICT_UNKNOWN, (name, case, bits, wide, verdict,
                                                        run.stdout, run.stderr)
                    print(f'{name}: case={case}/variant={variant}/bits={bits}/wide={wide}: {verdict}', flush=True)


if __name__ == '__main__':
    main()
