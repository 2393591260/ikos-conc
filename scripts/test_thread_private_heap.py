#!/usr/bin/env python3
"""Worker allocation privacy requires closed provenance, lifetime and extent."""
import argparse
import json
import os
import re
from pathlib import Path
import sqlite3
import subprocess
import tempfile

from ikos_runtime import runtime_command, runtime_identity, verify_runtime

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--bits', choices=('32', '64'), default='32')
    parser.add_argument('--native-only', action='store_true')
    args = parser.parse_args()
    command = runtime_command(args.ikos.resolve())
    identity = runtime_identity(args.ikos.resolve())
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/thread-private-heap.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    with tempfile.TemporaryDirectory(prefix='ikos-worker-heap-') as temp:
        work = Path(temp)
        for wide in (() if args.native_only else (False, True)):
            for case in range(28):
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') + fixture)
                proc = subprocess.run([*command, '--analyses=race',
                    '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                    '--bmc-deadline-ms=0', '-m' + args.bits, '-o', str(work / 'output.db'), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                output = proc.stdout + proc.stderr
                assert proc.returncode == 0, (case, wide, output)
                with sqlite3.connect(work / 'output.db') as db:
                    rows = [json.loads(row[0]) for row in db.execute('SELECT info FROM checks')]
                proofs = [r for r in rows if r.get('private_heap_contract')]
                positive = case in {0, 1, 12, 22, 24} or (case == 11 and wide)
                assert bool(proofs) == positive, (case, wide, proofs, output)
                if positive and case != 24:
                    assert 'The program is SAFE' in output, (case, wide, output)
                if case == 24:
                    assert 'The program is SAFE' not in output, (wide, output)
                print(f'private heap={case}/wide={wide}: proofs={len(proofs)}', flush=True)

        def native_check(name, text, expected):
            source = work / 'native.ll'
            source.write_text(text)
            proc = subprocess.run([*command, '--analyses=race', '--concurrency=auto',
                '--partitioning=guards', '--demote-race-to-unknown', '--bmc-deadline-ms=0',
                '-m' + args.bits, '-o', str(work / 'output.db'), str(source)],
                env=env, capture_output=True, text=True, timeout=30)
            output = proc.stdout + proc.stderr
            assert proc.returncode == 0, (name, output)
            with sqlite3.connect(work / 'output.db') as db:
                proofs = [row for row, in db.execute(
                    "SELECT info FROM checks WHERE info LIKE '%private_heap_contract%'")]
            assert bool(proofs) == expected, (name, output, proofs)
            if expected:
                assert 'The program is SAFE' in output, (name, output)
            else:
                assert 'The program is SAFE' not in output, (name, output)
            print(f'private native={name}/bits={args.bits}: proof={expected}', flush=True)

        compiler = json.loads(subprocess.check_output(command[:-1] + ['-c',
            'import json; from ikos import settings, analyzer; '
            'print(json.dumps([settings.clang(), analyzer.clang_ikos_flags()]))'], text=True))
        for count, accepted in ((4, True), (2147483647, True), (2147483648, False)):
            # Keep the actual loop signed i32. The last allocation count does not
            # equal that loop's bound, even though the heap may be larger.
            body = fixture.replace('int n = __VERIFIER_nondet_int();',
                                   'int n = ' + str(min(count, 2147483647)) + ';')
            body = body.replace('typedef int datum;', 'typedef char datum;')
            if not accepted:
                body = body.replace('calloc((size_t)n, sizeof(datum))',
                                    'calloc((size_t)' + str(count) + 'ULL, sizeof(datum))')
            source = work / 'native.c'
            source.write_text(body)
            llvm = work / 'constant.ll'
            subprocess.run([compiler[0], *compiler[1], '-S', '-emit-llvm',
                '-m' + args.bits, str(source), '-o', str(llvm)], check=True,
                capture_output=True)
            native_check('constant-count-' + str(count), llvm.read_text(), accepted)
        for mode in ('thread-private-heap', 'heap-handoff'):
            source = work / 'native.c'
            body = (ROOT / 'analyzer/test/regression/bmc-values' / (mode + '.c')).read_text()
            source.write_text(body)
            llvm = work / 'original.ll'
            subprocess.run([compiler[0], *compiler[1], '-S', '-emit-llvm',
                '-m' + args.bits, str(source), '-o', str(llvm)], check=True,
                capture_output=True)
            original = llvm.read_text()
            native_check(mode + '/positive', original, True)
            for place in ('call', 'declaration'):
                changed = 0
                lines = original.splitlines()
                for i, line in enumerate(lines):
                    selected = line.startswith('declare ') if place == 'declaration' else ' call ' in line
                    if selected and '@free(' in line:
                        prefix, arguments = line.split('@free(', 1)
                        assert 'i8*' in arguments
                        lines[i] = prefix + '@free(' + arguments.replace('i8*', 'i8* byval(i8)', 1)
                        changed += 1
                assert changed > 0
                native_check(mode + '/byval-' + place, '\n'.join(lines) + '\n', False)
            layout_line = next(line for line in original.splitlines()
                               if line.startswith('target datalayout = '))
            fields = [x for x in layout_line.split('"')[1].split('-')
                      if not x.startswith('p:')]
            pointer = args.bits
            index = '16' if pointer == '32' else '32'
            fields.insert(1, 'p:' + pointer + ':' + pointer + ':' + pointer + ':' + index)
            mutated = original.replace(layout_line, 'target datalayout = "' + '-'.join(fields) + '"')
            native_check(mode + '/index-width', mutated, False)
    verify_runtime(identity)


if __name__ == '__main__':
    main()
