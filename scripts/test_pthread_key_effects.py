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

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    args = parser.parse_args()
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/pthread-key-effects.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    positives = {0, 3, 18}
    with tempfile.TemporaryDirectory(prefix='ikos-pthread-key-') as temp:
        work = Path(temp)
        for wide in (False, True):
            for case in range(30):
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') + fixture)
                db = work / 'output.db'
                proc = subprocess.run([str(args.ikos.resolve()), '--analyses=race',
                    '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                    '--bmc-deadline-ms=0', '-m32', '-o', str(db), str(source)],
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
            proc = subprocess.run([str(args.ikos.resolve()), '--analyses=race',
                '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                '--bmc-deadline-ms=0', '-m32', '--proc=' + ('intra' if intra else 'inter'),
                '-o', str(work / 'output.db'), str(source)], env=env,
                capture_output=True, text=True, timeout=30)
            output = proc.stdout + proc.stderr
            assert proc.returncode == 0 and ('The program is SAFE' in output) == (not intra), output
            print(f'renamed opaque token/intraprocedural={intra}: safe={not intra}', flush=True)


if __name__ == '__main__':
    main()
