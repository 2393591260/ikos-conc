#!/usr/bin/env python3
"""Check conservative pthread-key effects independently of TLS value equality."""
import argparse
import json
import os
from pathlib import Path
import re
import sqlite3
import subprocess
import tempfile

from ikos_runtime import runtime_command

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--bits', choices=('32', '64'), default='32')
    args = parser.parse_args()
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/pthread-key-effects.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    command = runtime_command(args.ikos)
    positives = {0, 3, 18}
    with tempfile.TemporaryDirectory(prefix='ikos-pthread-key-') as temp:
        work = Path(temp)
        for wide in (False, True):
            for case in range(30):
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') + fixture)
                db = work / 'output.db'
                proc = subprocess.run([*command, '--analyses=race',
                    '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                    '--bmc-deadline-ms=0', '-m' + args.bits, '-o', str(db), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                output = proc.stdout + proc.stderr
                assert proc.returncode == 0, (case, wide, output)
                safe = 'The program is SAFE' in output
                with sqlite3.connect(db) as connection:
                    reasons = [json.loads(row[0]).get('unknown_reason') for row in connection.execute(
                        "SELECT info FROM checks WHERE info LIKE '%unknown_reason%'")]
                assert safe == (case in positives), (case, wide, output, reasons)
                if case in {3, 18}:
                    assert not reasons, (case, wide, reasons)
                if case in {4, 5, 10, 19}:
                    assert 'integer-derived pointer aliasing is not modeled' in reasons, (case, reasons)
                if case in {6, 7, 9, 16}:
                    assert 'external call effects not covered' in reasons, (case, reasons)
                print(f'pthread key={case}/wide={wide}: safe={safe}, reasons={reasons}', flush=True)
        for intra in (False, True):
            source = work / 'permuted.c'
            renamed = re.sub(r'\bkey\b', 'tls_selector', fixture).replace('worker', 'entry_body')
            source.write_text('#define CASE 3\n' + renamed)
            proc = subprocess.run([*command, '--analyses=race',
                '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                '--bmc-deadline-ms=0', '-m' + args.bits, '--proc=' + ('intra' if intra else 'inter'),
                '-o', str(work / 'output.db'), str(source)], env=env,
                capture_output=True, text=True, timeout=30)
            output = proc.stdout + proc.stderr
            if intra:
                assert proc.returncode != 0, output
                assert 'thread-modular analysis requires -j=1 and -proc=inter' in output, output
                assert 'The program is SAFE' not in output, output
            else:
                assert proc.returncode == 0 and 'The program is SAFE' in output, output
            print(f'renamed opaque token/intraprocedural={intra}: rejected={intra}', flush=True)


        compiler = json.loads(subprocess.check_output(command[:-1] + ['-c',
            'import json; from ikos import settings, analyzer; '
            'print(json.dumps([settings.clang(), analyzer.clang_ikos_flags()]))'], text=True))
        source = work / 'abi.c'
        source.write_text('#define CASE 3' + chr(10) + fixture)
        native = work / 'abi.ll'
        subprocess.run([compiler[0], *compiler[1], '-S', '-emit-llvm',
                        '-m' + args.bits, str(source), '-o', str(native)],
                       capture_output=True, check=True)
        original = native.read_text()
        for api, typ in [('pthread_key_create', 'i32'), ('pthread_setspecific', 'i8')]:
            for place in ('declaration', 'call'):
                lines = original.splitlines()
                changed = 0
                for index, line in enumerate(lines):
                    selected = line.startswith('declare ') if place == 'declaration' else ' call ' in line
                    if not selected or '@' + api + '(' not in line:
                        continue
                    prefix, actuals = line.split('@' + api + '(', 1)
                    needle = typ + '*'
                    assert needle in actuals, (api, place, line)
                    actuals = actuals.replace(needle, needle + ' byval(' + typ + ')', 1)
                    lines[index] = prefix + '@' + api + '(' + actuals
                    changed += 1
                assert changed == 1, (api, place, changed)
                damaged = work / 'changed.ll'
                damaged.write_text(chr(10).join(lines) + chr(10))
                proc = subprocess.run([*command, '--analyses=race', '--concurrency=auto',
                    '--partitioning=guards', '--demote-race-to-unknown', '--bmc-deadline-ms=0',
                    '-m' + args.bits, '-o', str(work / 'output.db'), str(damaged)],
                    env=env, capture_output=True, text=True, timeout=30)
                output = proc.stdout + proc.stderr
                assert proc.returncode == 0, (api, place, output)
                assert 'The program is SAFE' not in output, (api, place, output)
                with sqlite3.connect(work / 'output.db') as connection:
                    reasons = [json.loads(row[0]).get('unknown_reason') for row in connection.execute(
                        "SELECT info FROM checks WHERE info LIKE '%unknown_reason%'")]
                assert 'external call effects not covered' in reasons, (api, place, reasons)
                print(f'pthread ABI={api}/{place}/byval/bits={args.bits}: rejected', flush=True)


if __name__ == '__main__':
    main()
