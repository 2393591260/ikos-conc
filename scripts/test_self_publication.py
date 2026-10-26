#!/usr/bin/env python3
"""Single state publication, exhaustive guarded arguments and dynamic gate phases."""
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
    parser.add_argument('--case', type=int, choices=range(31), action='append',
                        help='Run only the selected case (repeatable).')
    parser.add_argument("--bits", choices=("32", "64"), default="32")
    args = parser.parse_args()
    identity = runtime_identity(args.ikos.resolve())
    command = runtime_command(args.ikos.resolve())
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/self-publication.c').read_text()
    target_lines = {i for i, line in enumerate(fixture.splitlines(), 1) if line.strip() == 'datum after = state;'}
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    with tempfile.TemporaryDirectory(prefix='ikos-self-publication-') as temporary:
        work = Path(temporary)
        for wide, opt in ((0, 0), (0, 1), (1, 0), (1, 1)):
            for case in (args.case if args.case is not None else range(31)):
                source = work / 'renamed.c'
                definitions = f'#define CASE {case}\n#define WIDE {wide}\n#define OPT {opt}\n'
                if case in {27, 28}:
                    definitions += '#define LENGTH 64\n#define STEP 1\n'
                source.write_text(definitions + '#line 1\n' + fixture)
                proc = subprocess.run(command + ['--analyses=race',
                    '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                    '--bmc-deadline-ms=0', '--log=debug', '-m' + args.bits, '-o', str(work / 'output.db'), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                output = proc.stdout + proc.stderr
                assert proc.returncode == 0, (case, wide, opt, output)
                with sqlite3.connect(work / 'output.db') as db:
                    infos = [json.loads(r[0]) for r in db.execute('SELECT info FROM checks')]
                proofs = [r for r in infos if r.get('self_publication_contract')]
                positive = case in {0, 7, 21, 24, 26, 27, 30} or (opt and case in {6, 18})
                target_proofs = [r for r in proofs if target_lines.intersection((r.get('access_a_line'), r.get('access_b_line')))]
                assert bool(target_proofs) == positive, (case, wide, opt, proofs, output)
                if positive:
                    tuples = {r["argument_tuples"] for r in target_proofs}
                    expected_tuples = 6 if case == 24 else 64 if case in {27, 30} else 3
                    assert tuples == {expected_tuples}, (case, wide, opt, tuples)
                if positive and case != 7:
                    assert 'The program is SAFE' in output, (case, wide, opt, output)
                if case in {1, 2, 3, 7, 8, 9, 11, 12, 23}:
                    assert 'The program is SAFE' not in output, (case, wide, opt, output)
                print(f'CASE={case}/WIDE={wide}/OPT={opt}: proofs={len(proofs)}', flush=True)
        if args.case is None:
            def control(name, body, positive, llvm=False):
                source = work / ('control.ll' if llvm else 'control.c')
                source.write_text(body)
                database = work / 'control.db'
                proc = subprocess.run(command + ['--analyses=race', '--concurrency=auto',
                    '--partitioning=guards', '--demote-race-to-unknown', '--bmc-deadline-ms=0',
                    '-m' + args.bits, '-o', str(database), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                output = proc.stdout + proc.stderr
                assert proc.returncode == 0, (name, output)
                with sqlite3.connect(database) as db:
                    proofs = [json.loads(row[0]) for row in db.execute('SELECT info FROM checks')
                              if json.loads(row[0]).get('self_publication_contract')]
                assert bool(proofs) == positive, (name, proofs, output)
                assert ('The program is SAFE' in output) == positive, (name, output)
                print(f'native-self-publication={name}/bits={args.bits}: proof={positive}', flush=True)

            control('native-long', fixture.replace('typedef int datum;', 'typedef long datum;'), True)
            control('prototyped-nondet', fixture.replace('__VERIFIER_nondet_int();',
                '__VERIFIER_nondet_int(void);', 1), True)
            control('wrong-nondet-return', fixture.replace('extern int __VERIFIER_nondet_int();',
                'extern long long __VERIFIER_nondet_int();'), False)
            control('nondet-extra-actual', fixture.replace('= __VERIFIER_nondet_int();',
                '= __VERIFIER_nondet_int(1);'), False)
            control('handle-upper-byte', fixture.replace('  while (1) {',
                '  ((unsigned char *)&output)[sizeof output - 1] = 1;\n  while (1) {'), False)
            control('mutex-upper-byte', fixture.replace('  while (1) {',
                '  ((unsigned char *)&gate)[sizeof gate - 1] = 1;\n  while (1) {'), False)
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
            control('llvm-positive', original, True, llvm=True)
            changed, count = re.subn(r'(alloca i' + args.bits + r', align )\d+',
                                     r'\g<1>1', original)
            assert count
            control('handle-alignment', changed, False, llvm=True)
            changed, count = re.subn(r'(^@gate = [^\n]*\balign )\d+',
                                     r'\g<1>1', original, flags=re.MULTILINE)
            assert count == 1
            control('mutex-alignment', changed, False, llvm=True)
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
