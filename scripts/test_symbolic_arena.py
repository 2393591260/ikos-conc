#!/usr/bin/env python3
"""Inductive serialized heap slices, byte extents and complete loop frames."""
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
    args = parser.parse_args()
    command = runtime_command(args.ikos.resolve())
    identity = runtime_identity(args.ikos.resolve())
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
            proc = subprocess.run([*command, '--analyses=race',
                '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                '--bmc-deadline-ms=0', '-m' + args.bits, '-o', str(work / 'output.db'), str(source)],
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

        def check_control(name, body, positive, llvm=False):
            source = work / ('control.ll' if llvm else 'control.c')
            source.write_text(body)
            proc = subprocess.run([*command, '--analyses=race', '--concurrency=auto',
                '--partitioning=guards', '--demote-race-to-unknown', '--bmc-deadline-ms=0',
                '--log=debug', '-m' + args.bits, '-o', str(work / 'output.db'), str(source)],
                env=env, capture_output=True, text=True, timeout=30)
            output = proc.stdout + proc.stderr
            assert proc.returncode == 0, (name, output)
            with sqlite3.connect(work / 'output.db') as db:
                infos = [json.loads(row[0]) for row in db.execute('SELECT info FROM checks')]
            proofs = [row for row in infos if row.get('arena_reservation_contract') ==
                      'closed-symbolic-bounded-slices-v1']
            assert bool(proofs) == positive, (name, proofs, output)
            assert ('The program is SAFE' in output) == positive, (name, output)
            print(f'native-arena={name}/bits={args.bits}: proof={positive}', flush=True)

        check_control('nondet-wrong-return', fixture.replace(
            'extern int __VERIFIER_nondet_int();',
            'extern long long __VERIFIER_nondet_int();'), False)
        check_control('nondet-extra-actual', fixture.replace(
            'limit = __VERIFIER_nondet_int();',
            'limit = __VERIFIER_nondet_int(1);'), False)

        large = '#define CASE 7\n#define WIDE 1\n' + fixture.replace(
            'require(limit > 0);', 'require(limit >= 600000000);').replace(
            'if (cursor + CHUNK <= limit)', 'if (cursor <= limit - CHUNK)')
        check_control('extent-above-4GB', large, args.bits == '64')
        check_control('extent-truncated-before-native-size', large.replace(
            'malloc(sizeof(datum) * limit)', 'malloc((unsigned)(sizeof(datum) * limit))'), False)
        check_control('index-truncated', fixture.replace(
            'storage[i]', 'storage[(unsigned short)i]'), False)
        check_control('mutex-upper-byte', fixture.replace('  cursor = 0;\n  limit =',
            '  ((unsigned char *)&serializer)[sizeof serializer - 1] = 1;\n  cursor = 0;\n  limit ='), False)
        check_control('handle-upper-byte', fixture.replace('  pthread_t private_output;',
            '  pthread_t private_output;\n  ((unsigned char *)&private_output)[sizeof private_output - 1] = 1;'), False)

        compiler = json.loads(subprocess.check_output(command[:-1] + ['-c',
            'import json; from ikos import settings, analyzer; '
            'print(json.dumps([settings.clang(), analyzer.clang_ikos_flags()]))'], text=True))
        source = work / 'base.c'
        source.write_text(fixture)
        native = work / 'base.ll'
        subprocess.run([compiler[0], *compiler[1], '-S', '-emit-llvm',
            '-m' + args.bits, str(source), '-o', str(native)], check=True, capture_output=True)
        original = native.read_text()
        check_control('llvm-positive', original, True, llvm=True)
        for place in ('call', 'declaration'):
            lines = original.splitlines()
            changed = 0
            for i, line in enumerate(lines):
                selected = line.startswith('declare ') if place == 'declaration' else ' call ' in line
                if selected and '@pthread_mutex_lock(' in line:
                    prefix, actuals = line.split('@pthread_mutex_lock(', 1)
                    needle = '%union.pthread_mutex_t*'
                    assert needle in actuals
                    lines[i] = prefix + '@pthread_mutex_lock(' + actuals.replace(
                        needle, needle + ' byval(%union.pthread_mutex_t)', 1)
                    changed += 1
            assert changed > 0
            check_control('byval-' + place, '\n'.join(lines) + '\n', False, llvm=True)
        misaligned, changes = re.subn(r'(^@serializer = [^\n]*\balign )\d+',
                                     r'\g<1>1', original, flags=re.MULTILINE)
        assert changes == 1
        check_control('mutex-alignment', misaligned, False, llvm=True)
        layout_line = next(line for line in original.splitlines()
                           if line.startswith('target datalayout = '))
        fields = [field for field in layout_line.split('"')[1].split('-')
                  if not field.startswith('p:')]
        pointer, index = args.bits, ('16' if args.bits == '32' else '32')
        fields.insert(1, 'p:' + pointer + ':' + pointer + ':' + pointer + ':' + index)
        narrowed = original.replace(layout_line, 'target datalayout = "' + '-'.join(fields) + '"')
        check_control('native-index-width', narrowed, False, llvm=True)
    verify_runtime(identity)


if __name__ == '__main__':
    main()
