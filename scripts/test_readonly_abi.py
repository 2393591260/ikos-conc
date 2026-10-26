#!/usr/bin/env python3
"""Actual ABI controls for the conditional immutable-heap read proof."""
import argparse
import json
import os
from pathlib import Path
import re
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
    entry = args.ikos.resolve()
    identity = runtime_identity(entry)
    command = runtime_command(entry)
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/readonly-heap-reads.c').read_text()
    with tempfile.TemporaryDirectory(prefix='ikos-readonly-abi-') as temporary:
        work = Path(temporary)
        def control(name, body, expected, llvm=False, safe=None):
            source = work / ('control.ll' if llvm else 'control.c')
            source.write_text(body)
            database = work / 'control.db'
            proc = subprocess.run(command + ['--analyses=race', '--concurrency=auto',
                '--partitioning=guards', '--demote-race-to-unknown',
                '--bmc-deadline-ms=0', '-m' + args.bits,
                '-o', str(database), str(source)], env=env, text=True,
                capture_output=True, timeout=45)
            output = proc.stdout + proc.stderr
            assert proc.returncode == 0, (name, output)
            with sqlite3.connect(database) as db:
                proofs = [json.loads(row[0]) for row in db.execute('SELECT info FROM checks')
                          if json.loads(row[0]).get('readonly_heap_reads_contract')]
            assert bool(proofs) == expected, (name, proofs, output)
            if safe is not None:
                assert ('The program is SAFE' in output) == safe, (name, output)
            print(f'readonly-abi={name}/bits={args.bits}: proofs={len(proofs)}', flush=True)
        control('native-long', fixture.replace('typedef int datum;', 'typedef long datum;'), True, safe=True)
        control('non-default-address-space', fixture.replace('static datum result;',
                'static datum result __attribute__((address_space(1)));'), False)
        control('old-style-lowering', fixture.replace('__VERIFIER_nondet_int(void)', '__VERIFIER_nondet_int()'), args.bits == '64', safe=args.bits == '64')
        control('nondet-wrong-return', fixture.replace('extern int __VERIFIER_nondet_int(void);',
                'extern long long __VERIFIER_nondet_int(void);'), False)
        control('nondet-extra-actual', fixture.replace('extern int __VERIFIER_nondet_int(void);',
                'extern int __VERIFIER_nondet_int();').replace('= __VERIFIER_nondet_int();',
                '= __VERIFIER_nondet_int(1);'), False)
        control('handle-upper-byte', fixture.replace('  pthread_create(&two, 0, reader, 0);',
                '  pthread_create(&two, 0, reader, 0);\n  ((unsigned char *)&one)[sizeof one - 1] = 1;'), False)
        control('huge-index', fixture.replace('matrix[left[position]][right[position]]',
                'matrix[left[position] + 0x4000000000000000LL][right[position]]'), args.bits == '32')
        control('negative-index', fixture.replace('matrix[left[position]][right[position]]',
                'matrix[-1][right[position]]'), False)
        control('misaligned-row', fixture.replace('  pthread_create(&two, 0, reader, 0);',
                '  left = (datum *)((char *)left + 1);\n  pthread_create(&two, 0, reader, 0);'), False)
        native_bounds = fixture.replace('4294967295u', '__SIZE_MAX__')
        vector = native_bounds.replace('count = __VERIFIER_nondet_int();', 'count = 1073741825;')
        vector = vector.replace('rows = __VERIFIER_nondet_int();', 'rows = 1;')
        vector = vector.replace('columns = __VERIFIER_nondet_int();', 'columns = 1;')
        control('vector-above-4GB', vector, args.bits == '64')
        table = native_bounds.replace('count = __VERIFIER_nondet_int();', 'count = 1;')
        table = table.replace('rows = __VERIFIER_nondet_int();', 'rows = 1073741825;')
        table = table.replace('columns = __VERIFIER_nondet_int();', 'columns = 1;')
        control('table-above-4GB', table, args.bits == '64')
        control('zero-vector', fixture.replace('count = __VERIFIER_nondet_int();', 'count = 0;'), False, safe=True)
        snapshots = (ROOT / 'analyzer/test/regression/bmc-values/readonly-loop-snapshots.c').read_text()
        control('malloc-size-t-width', snapshots.replace('typedef __SIZE_TYPE__ size_t;',
                'typedef unsigned int size_t;'), args.bits == '32')
        control('truncated-handle-backing', fixture.replace('pthread_t one, two;',
                'unsigned int one, two;'), args.bits == '32')
        control('zero-rows', fixture.replace('rows = __VERIFIER_nondet_int();', 'rows = 0;'),
                True)
        compiler = json.loads(subprocess.check_output(command[:-1] + ['-c',
            'import json; from ikos import settings, analyzer; '
            'print(json.dumps([settings.clang(), analyzer.clang_ikos_flags()]))'], text=True))
        source = work / 'native.c'
        source.write_text(fixture)
        ir = work / 'native.ll'
        subprocess.run([compiler[0], *compiler[1], '-S', '-emit-llvm',
                        '-m' + args.bits, str(source), '-o', str(ir)],
                       check=True, capture_output=True)
        original = ir.read_text()
        control('llvm-positive', original, True, llvm=True, safe=True)
        changed, count = re.subn(r'(alloca i' + args.bits + r', align )\d+',
                                 r'\g<1>1', original)
        assert count
        control('handle-alignment', changed, False, llvm=True)
        changed = original.replace('call i32 @__VERIFIER_nondet_int(',
                                   'call fastcc i32 @__VERIFIER_nondet_int(')
        changed = changed.replace('declare i32 @__VERIFIER_nondet_int(',
                                  'declare fastcc i32 @__VERIFIER_nondet_int(')
        assert changed != original and 'fastcc i32 @__VERIFIER_nondet_int' in changed
        control('non-C-nondet', changed, False, llvm=True)
        lines = original.splitlines()
        calls = declarations = 0
        for i, line in enumerate(lines):
            if '@malloc(' not in line:
                continue
            if ' call ' in line:
                lines[i] = line.replace('call ', 'call fastcc ', 1)
                calls += 1
            elif line.startswith('declare '):
                lines[i] = line.replace('declare ', 'declare fastcc ', 1)
                declarations += 1
        assert calls and declarations == 1
        changed = '\n'.join(lines) + '\n'
        control('non-C-malloc', changed, False, llvm=True)
        lines = original.splitlines()
        count = 0
        for i, line in enumerate(lines):
            if ' call ' in line and '@pthread_create(' in line:
                needle = 'i' + args.bits + '*'
                assert needle in line
                lines[i] = line.replace(needle, needle + ' byval(i' + args.bits + ')', 1)
                count += 1
        assert count
        control('create-byval', '\n'.join(lines) + '\n', False, llvm=True)
        layout = next(line for line in original.splitlines() if line.startswith('target datalayout = '))
        fields = [x for x in layout.split('"')[1].split('-') if not x.startswith('p:')]
        fields.insert(1, 'p:' + args.bits + ':' + args.bits + ':' + args.bits + ':' +
                      ('16' if args.bits == '32' else '32'))
        control('index-width', original.replace(layout, 'target datalayout = "' +
                '-'.join(fields) + '"'), False, llvm=True)
    verify_runtime(identity)

if __name__ == '__main__':
    main()
