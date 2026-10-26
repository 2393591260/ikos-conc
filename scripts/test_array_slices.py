#!/usr/bin/env python3
"""Exercise native ABI array slices with BMC disabled and exact proof checks."""
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
BASE_CASES = list(range(14)) + [15, 18, 19, 20, 23, 30, 31, 32, 33, 34]
NATIVE_CASES = [40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 51, 52, 54, 55, 56, 57]
CASES = BASE_CASES + NATIVE_CASES
CONTRACT = 'closed-stable-flat-array-slices-v1'


def expectation(case, bits):
    safe = case in {0, 18, 23, 43, 44, 54}
    safe |= bits == 64 and case in {5, 40}
    safe |= bits == 32 and case == 48
    # CASE18 may already be discharged by another established fixed-size rule.
    return ('SAFE' if safe else 'UNKNOWN', safe and case != 18)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--bits', type=int, choices=(32, 64), default=32)
    parser.add_argument('--case', type=int, choices=CASES, action='append')
    parser.add_argument('--native-only', action='store_true',
                        help='Run the LLVM ABI controls without the C case matrix.')
    args = parser.parse_args()
    entry = args.ikos.resolve()
    command = runtime_command(entry)
    identity = runtime_identity(entry)
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/array-slices.c').read_text()
    env = {key: value for key, value in os.environ.items()
           if not key.startswith('IKOS_') and key not in {'PYTHONPATH', 'PYTHONHOME'}}
    with tempfile.TemporaryDirectory(prefix='ikos-array-slices-') as temporary:
        work = Path(temporary)

        def check(name, body, expected, required, llvm=False):
            source = work / ('control.ll' if llvm else 'renamed.c')
            source.write_text(body)
            database = work / 'output.db'
            proc = subprocess.run([*command, '--analyses=race', '--concurrency=auto',
                '--partitioning=guards', '--demote-race-to-unknown',
                '--bmc-deadline-ms=0', '--log=debug', '-m' + str(args.bits),
                '-o', str(database), str(source)], env=env,
                capture_output=True, text=True, timeout=30)
            output = proc.stdout + proc.stderr
            assert proc.returncode == 0, (name, args.bits, output)
            with sqlite3.connect(database) as db:
                infos = [json.loads(row[0]) for row in db.execute('SELECT info FROM checks')]
                result = json.loads(db.execute(
                    "SELECT value FROM settings WHERE name='race_result'").fetchone()[0])
            proofs = [info for info in infos if info.get('array_slices_contract') == CONTRACT]
            assert result['verdict'] == expected, (name, args.bits, result, output)
            if required:
                assert proofs, (name, args.bits, output)
            elif expected != 'SAFE':
                if name.startswith('case=34/'):
                    # The loop pair is disjoint; the post-loop store must remain.
                    tail = next(index for index, line in enumerate(body.splitlines(), 1)
                                if 'if (i < length) buffer[i] = 9;' in line)
                    assert result['remaining_pairs'] > 0
                    assert any(access.get('line') == tail
                               for info in infos
                               for pair in info.get('conflicting_accesses', [])
                               for access in (pair['access_a'], pair['access_b'])), (
                                   name, args.bits, tail, infos, output)
                else:
                    assert not proofs, (name, args.bits, proofs, output)
            print(f'slices {name}/bits={args.bits}: {expected}; proofs={len(proofs)}',
                  flush=True)

        if not args.native_only:
            for wide in (False, True):
                for case in CASES if args.case is None else args.case:
                    text = fixture.replace('make', 'allocate_values').replace('cut', 'division')
                    text = text.replace('output', 'buffer').replace('first', 'left_part')
                    body = f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') + text
                    expected, required = expectation(case, args.bits)
                    check(f'case={case}/wide={int(wide)}', body, expected, required)

        # A selected C case is a bounded diagnostic; the complete default suite
        # also checks real LLVM conventions and native pointer-index agreement.
        if args.case is None:
            compiler = json.loads(subprocess.check_output(command[:-1] + ['-c',
                'import json; from ikos import settings, analyzer; '
                'print(json.dumps([settings.clang(), analyzer.clang_ikos_flags()]))'],
                env=env, text=True))

            def compile_fixture(case):
                source = work / 'base.c'
                source.write_text(f'#define CASE {case}\n' + fixture)
                native = work / 'base.ll'
                subprocess.run([compiler[0], *compiler[1], '-S', '-emit-llvm',
                    '-m' + str(args.bits), str(source), '-o', str(native)],
                    env=env, check=True, capture_output=True, text=True)
                return native.read_text()

            original = compile_fixture(0)
            check('llvm-positive', original, 'SAFE', True, llvm=True)
            for place in ('call', 'declaration', 'both'):
                lines = original.splitlines()
                changed = 0
                for index, line in enumerate(lines):
                    if '@malloc(' not in line:
                        continue
                    declaration = line.startswith('declare ')
                    invocation = ' call ' in line
                    if declaration and place in {'declaration', 'both'}:
                        lines[index] = line.replace('declare ', 'declare fastcc ', 1)
                        changed += 1
                    elif invocation and place in {'call', 'both'}:
                        lines[index] = line.replace('call ', 'call fastcc ', 1)
                        changed += 1
                assert changed, (place, original)
                check('malloc-convention-' + place, '\n'.join(lines) + '\n',
                      'UNKNOWN', False, llvm=True)

            # Both flags are independent obligations, including actual i64 values.
            wide_helper = compile_fixture(44)
            check('llvm-i64-positive', wide_helper, 'SAFE', True, llvm=True)
            both, count = re.subn(r'\badd nsw i64\b', 'add nuw nsw i64', wide_helper)
            assert count, wide_helper
            check('llvm-i64-both-flags', both, 'SAFE', True, llvm=True)

            layout_template = (ROOT /
                'analyzer/test/regression/bmc-values/array-slices-layout.ll.in').read_text()
            replacements = {
                '{{WORD}}': str(args.bits), '{{ALIGN}}': str(args.bits // 8),
                '{{I64ALIGN}}': '32' if args.bits == 32 else '64',
                '{{TRIPLE}}': 'i386-pc-linux-gnu' if args.bits == 32 else 'x86_64-pc-linux-gnu',
                '{{EXTEND_N}}': '' if args.bits == 32 else '%native_n = zext i32 %n to i64',
                '{{NATIVE_N}}': '%n' if args.bits == 32 else '%native_n',
                '{{EXTEND_I}}': '' if args.bits == 32 else '%native_i = sext i32 %i to i64',
                '{{NATIVE_I}}': '%i' if args.bits == 32 else '%native_i',
            }
            for name, value in replacements.items():
                layout_template = layout_template.replace(name, value)
            for stride, expected in ((4, 'SAFE'), (3, 'UNKNOWN')):
                body = layout_template.replace('{{STRIDE}}', str(stride))
                assert '{{' not in body
                check('i24-alloc-stride-' + str(stride), body, expected,
                      expected == 'SAFE', llvm=True)

            if args.bits == 64:
                lines = original.splitlines()
                changed = 0
                for index, line in enumerate(lines):
                    match = re.fullmatch(r'target datalayout = "([^"]*)"', line)
                    if not match:
                        continue
                    fields = [field for field in match.group(1).split('-')
                              if not re.match(r'^p(?:0)?:', field)]
                    fields.append('p:64:64:64:32')
                    lines[index] = 'target datalayout = "' + '-'.join(fields) + '"'
                    changed += 1
                assert changed == 1
                check('pointer64-index32', '\n'.join(lines) + '\n',
                      'UNKNOWN', False, llvm=True)
    verify_runtime(identity)


if __name__ == '__main__':
    main()
