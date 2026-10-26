#!/usr/bin/env python3
"""Check serial child generations without BMC or a loop-unrolling safety rule."""
import argparse
import json
import os
import re
import sqlite3
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from svcomp_race_test import classify, VERDICT_SAFE, VERDICT_UNKNOWN
from ikos_runtime import runtime_command, runtime_identity, verify_runtime



def check_creator_instances(command, bits, env, work):
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/thread-creators.c').read_text()
    for wide in (False, True):
        for case in range(12):
            source = work / 'creators.c'
            source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') +
                              fixture.replace('reader', 'entry').replace('shared', 'payload'))
            proc = subprocess.run([*command, '--analyses=race', '--concurrency=auto',
                '--partitioning=guards', '--demote-race-to-unknown', '--bmc-deadline-ms=0',
                '-m' + bits, '-o', str(work / 'creators.db'), str(source)], env=env,
                capture_output=True, text=True, timeout=30)
            verdict = classify(proc.stdout, proc.returncode, False)
            # Mixed-creator safe orderings may remain inconclusive. Real races
            # must not be silently filtered, even with BMC disabled.
            expected = VERDICT_SAFE if case in (1, 6, 7) else VERDICT_UNKNOWN
            if case in (2, 5):
                assert verdict in (VERDICT_SAFE, VERDICT_UNKNOWN), (case, wide, verdict)
            else:
                assert verdict == expected, (case, wide, verdict, proc.stdout, proc.stderr)
            assert proc.returncode == 0, (case, wide, proc.stderr)
            print(f'creators case={case}/wide={wide}: {verdict}', flush=True)

def check_self_join(command, bits, env, work):
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/pthread-self-join.c').read_text()
    for wide in (False, True):
        for case in range(14):
            source = work / 'self-join.c'
            source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') +
                              fixture.replace('worker', 'entry').replace('payload', 'shared_data'))
            proc = subprocess.run([*command, '--analyses=race', '--concurrency=auto',
                '--partitioning=guards', '--demote-race-to-unknown', '--bmc-deadline-ms=0',
                '-m' + bits, '-o', str(work / 'self-join.db'), str(source)], env=env,
                capture_output=True, text=True, timeout=30)
            verdict = classify(proc.stdout, proc.returncode, False)
            expected = VERDICT_SAFE if case in (8, 9, 12, 13) else VERDICT_UNKNOWN
            assert proc.returncode == 0 and verdict == expected, (
                case, wide, verdict, proc.stdout, proc.stderr)
            print(f'self join case={case}/wide={wide}: {verdict}', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument("--bits", choices=("32", "64"), default="32")
    args = parser.parse_args()
    identity = runtime_identity(args.ikos)
    command = runtime_command(args.ikos)
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/thread-lifecycle.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    cases = [0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 15, 16, 17, 18, 20, 21, 22, 25, 27, 29, 31, 33, 34, 36, 38, 39]
    with tempfile.TemporaryDirectory(prefix='ikos-lifecycle-') as temp:
        work = Path(temp)
        check_creator_instances(command, args.bits, env, work)
        check_self_join(command, args.bits, env, work)
        for wide in (False, True):
            for case in cases:
                text = fixture.replace('phase', 'stage_value').replace('payload', 'shared_data')
                text = text.replace('register_child', 'activate').replace('parent', 'middle')
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') + text)
                report = work / 'report.json'
                proc = subprocess.run([*command, '--analyses=race',
                    '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                    '--bmc-deadline-ms=0', '--format=json', '--status-filter=*',
                    '--report-file=' + str(report), '--log=debug', '-m' + args.bits,
                    '-o', str(work / 'output.db'), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                verdict = classify(proc.stdout, proc.returncode, False)
                expected = VERDICT_SAFE if case in (0, 18, 36, 38) else VERDICT_UNKNOWN
                diagnostics = '\n'.join(line for line in proc.stdout.splitlines()
                                        if 'Thread lifecycle' in line or 'Summary' in line)
                assert proc.returncode == 0 and verdict == expected, (
                    case, wide, verdict, diagnostics, proc.stderr)
                proofs = [r['info'] for r in json.loads(report.read_text())['reports']
                          if r['info'].get('thread_lifecycle_contract') == 'serial-structured-thread-lifecycle-v1']
                if case in (0, 18, 36, 38):
                    assert proofs, (case, wide, diagnostics)
                print(f'lifecycle case={case}/wide={wide}: {verdict}; proofs={len(proofs)}', flush=True)


        def control(name, text, positive, llvm=False):
            source = work / ('control.ll' if llvm else 'control.c')
            source.write_text(text)
            database = work / 'control.db'
            proc = subprocess.run(command + ['--analyses=race', '--concurrency=auto',
                '--partitioning=guards', '--demote-race-to-unknown', '--bmc-deadline-ms=0',
                '-m' + args.bits, '-o', str(database), str(source)],
                env=env, capture_output=True, text=True, timeout=30)
            output = proc.stdout + proc.stderr
            assert proc.returncode == 0, (name, output)
            with sqlite3.connect(database) as db:
                proofs = [json.loads(row[0]) for row in db.execute('SELECT info FROM checks')
                          if json.loads(row[0]).get('thread_lifecycle_contract')]
            assert bool(proofs) == positive, (name, proofs, output)
            assert ('The program is SAFE' in output) == positive, (name, output)
            print(f'lifecycle-native={name}/bits={args.bits}: proof={positive}', flush=True)

        old_style = fixture.replace('extern int __VERIFIER_nondet_int(void);',
                                    'extern int __VERIFIER_nondet_int();')
        control('old-style-nondet', old_style, True)
        control('nondet-wrong-return', old_style.replace(
            'extern int __VERIFIER_nondet_int();',
            'extern long long __VERIFIER_nondet_int();'), False)
        control('nondet-extra-actual', old_style.replace(
            '__VERIFIER_nondet_int())', '__VERIFIER_nondet_int(1))'), False)
        control('handle-upper-byte', fixture.replace(
            '  pthread_join(inner, (void **)&status);',
            '  ((unsigned char *)&inner)[sizeof inner - 1] = 1;\n'
            '  pthread_join(inner, (void **)&status);'), False)
        control('join-result-upper-byte', fixture.replace(
            '  pthread_join(inner, (void **)&status);',
            '  ((unsigned char *)&status)[sizeof status - 1] = 1;\n'
            '  pthread_join(inner, (void **)&status);'), False)
        control('truncated-handle', fixture.replace(
            'pthread_join(inner,', 'pthread_join((unsigned int)inner,'), args.bits == '32')

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
        for name, pattern in [('handle-alignment', r'(^@inner = [^\n]*\balign )\d+'),
                              ('join-result-alignment', r'(alloca i8\*, align )\d+')]:
            changed, count = re.subn(pattern, r'\g<1>1', original, flags=re.MULTILINE)
            assert count > 0, name
            control(name, changed, False, llvm=True)
        lines = original.splitlines()
        count = 0
        for i, line in enumerate(lines):
            if ' call ' in line and '@pthread_join(' in line:
                assert 'i8**' in line
                lines[i] = line.replace('i8**', 'i8** byval(i8*)', 1)
                count += 1
        assert count
        control('join-byval', '\n'.join(lines) + '\n', False, llvm=True)
        layout = next(line for line in original.splitlines() if line.startswith('target datalayout = '))
        fields = [x for x in layout.split('"')[1].split('-') if not x.startswith('p:')]
        fields.insert(1, 'p:' + args.bits + ':' + args.bits + ':' + args.bits + ':' +
                      ('16' if args.bits == '32' else '32'))
        control('index-width', original.replace(layout, 'target datalayout = "' +
                '-'.join(fields) + '"'), False, llvm=True)

    verify_runtime(identity)


if __name__ == '__main__':
    main()
