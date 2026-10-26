#!/usr/bin/env python3
"""Check mutex-protected guard protocols without bounded safety inference."""
import argparse
import json
import os
import re
import sqlite3
from pathlib import Path
import subprocess
import sys
import tempfile

from ikos_runtime import runtime_command, runtime_identity, verify_runtime

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from svcomp_race_test import classify, VERDICT_SAFE, VERDICT_UNKNOWN


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument("--bits", choices=("32", "64"), default="32")
    args = parser.parse_args()
    identity = runtime_identity(args.ikos)
    command = runtime_command(args.ikos)
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/protected-protocol.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    cases = list(range(29)) + [30, 31, 32, 33, 35, 37, 40, 41, 42, 44]
    count = 0
    with tempfile.TemporaryDirectory(prefix='ikos-protected-protocol-') as temp:
        work = Path(temp)
        for wide in (False, True):
            for case in cases + ([38] if wide else []):
                text = fixture.replace('active', 'phase').replace('payload', 'shared_data')
                text = text.replace('producer', 'publish').replace('consumer', 'retire')
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
                expected = VERDICT_SAFE if case in (0, 30, 37, 38, 42) else VERDICT_UNKNOWN
                proofs = [r['info'] for r in json.loads(report.read_text())['reports']
                          if r['info'].get('protected_protocol_contract') == 'closed-two-root-protected-scalar-mutex-hb-v1'] if report.exists() else []
                assert proc.returncode == 0 and verdict == expected, (case, wide, verdict, proc.stdout[-5000:], proc.stderr)
                if expected == VERDICT_SAFE:
                    assert proofs, (case, wide)
                count += 1
                print(f'protected case={case}/wide={wide}: {verdict}; proofs={len(proofs)}', flush=True)

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
                          if json.loads(row[0]).get('protected_protocol_contract')]
            assert bool(proofs) == positive, (name, proofs, output)
            assert ('The program is SAFE' in output) == positive, (name, output)
            print(f'native-protocol={name}/bits={args.bits}: proof={positive}', flush=True)

        control('native-long', fixture.replace('typedef int scalar;', 'typedef long scalar;'), True)
        control('handle-upper-byte', fixture.replace('  pthread_join(a, 0);',
            '  ((unsigned char *)&a)[sizeof a - 1] = 1;\n  pthread_join(a, 0);'), False)
        control('handle-truncation', fixture.replace('pthread_join(a, 0);',
            'pthread_join((unsigned int)a, 0);'), args.bits == '32')
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
        changed, mutation_count = re.subn(r'(alloca i' + args.bits + r', align )\d+',
                                 r'\g<1>1', original)
        assert mutation_count
        control('handle-alignment', changed, False, llvm=True)
        lines = original.splitlines()
        mutation_count = 0
        for i, line in enumerate(lines):
            if ' call ' in line and '@pthread_create(' in line:
                needle = 'i' + args.bits + '*'
                assert needle in line
                lines[i] = line.replace(needle, needle + ' byval(i' + args.bits + ')', 1)
                mutation_count += 1
        assert mutation_count
        control('create-byval', '\n'.join(lines) + '\n', False, llvm=True)
        layout = next(line for line in original.splitlines() if line.startswith('target datalayout = '))
        fields = [x for x in layout.split('"')[1].split('-') if not x.startswith('p:')]
        fields.insert(1, 'p:' + args.bits + ':' + args.bits + ':' + args.bits + ':' +
                      ('16' if args.bits == '32' else '32'))
        control('index-width', original.replace(layout, 'target datalayout = "' +
                '-'.join(fields) + '"'), False, llvm=True)

        control('mutex-upper-byte', fixture.replace('  pthread_join(a, 0);',
            '  ((unsigned char *)&inner)[sizeof inner - 1] = 1;\n  pthread_join(a, 0);'), False)
        changed, mutation_count = re.subn(r'(^@inner = [^\n]*\balign )\d+',
                                 r'\g<1>1', original, flags=re.MULTILINE)
        assert mutation_count == 1
        control('mutex-alignment', changed, False, llvm=True)

    print(f'PASS {count} protected protocol controls', flush=True)

    verify_runtime(identity)


if __name__ == '__main__':
    main()
