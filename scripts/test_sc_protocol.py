#!/usr/bin/env python3
"""Finite SC protocol fixed point and its conservative boundary."""
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
    parser.add_argument("--bits", choices=("32", "64"), default="32")
    args = parser.parse_args()
    identity = runtime_identity(args.ikos)
    command = runtime_command(args.ikos)
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/sc-protocol.c').read_text()
    target_lines = {i for i, line in enumerate(fixture.splitlines(), 3) if line.strip() == 'payload = 2;'}
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    with tempfile.TemporaryDirectory(prefix='ikos-sc-protocol-') as temporary:
        work = Path(temporary)
        for wide in (0, 1):
            for case in range(32):
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n#define WIDE {wide}\n' + fixture)
                proc = subprocess.run([*command, '--analyses=race',
                    '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                    '--bmc-deadline-ms=0', '-m' + args.bits, '-o', str(work / 'output.db'), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                output = proc.stdout + proc.stderr
                if case == 12:
                    assert proc.returncode != 0 and "unsupported llvm instruction: fence" in output, output
                    print(f"CASE={case}/WIDE={wide}: frontend refuses fence", flush=True)
                    continue
                assert proc.returncode == 0, (case, wide, output)
                with sqlite3.connect(work / 'output.db') as db:
                    infos = [json.loads(r[0]) for r in db.execute('SELECT info FROM checks')]
                proofs = [r for r in infos if r.get('sc_protocol_contract')]
                positive = case in {0, 1, 10, 16, 26}
                target_proofs = [r for r in proofs if target_lines.intersection((r.get('access_a_line'), r.get('access_b_line')))]
                assert bool(target_proofs) == positive, (case, wide, proofs, output)
                if positive or case == 2:
                    assert 'The program is SAFE' in output, (case, wide, output)
                if case in {3, 4, 5, 7, 8, 13, 15, 17, 18, 19, 20, 21, 22, 24}:
                    assert 'The program is SAFE' not in output, (case, wide, output)
                print(f'CASE={case}/WIDE={wide}: proofs={len(proofs)}', flush=True)


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
                          if json.loads(row[0]).get('sc_protocol_contract')]
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
        changed, count = re.subn(r'(alloca i' + args.bits + r', align )\d+',
                                 r'\g<1>1', original)
        assert count
        control('handle-alignment', changed, False, llvm=True)
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
