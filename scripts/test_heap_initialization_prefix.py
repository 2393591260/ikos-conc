#!/usr/bin/env python3
"""Check per-instance helper initialization independently of race confirmation."""
import argparse
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    args = parser.parse_args()
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/helper-prefix-fields.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    with tempfile.TemporaryDirectory(prefix='ikos-helper-fields-') as temp:
        work = Path(temp)
        for wide in (False, True):
            for case in range(22):
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') + fixture)
                dbpath = work / 'output.db'
                result = subprocess.run([str(args.ikos.resolve()), '--analyses=race',
                    '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                    '--bmc-deadline-ms=0', '--log=debug', '-m32', '-o', str(dbpath), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                output = result.stdout + result.stderr
                assert result.returncode == 0, (case, wide, output)
                safe = 'The program is SAFE' in output
                certified = 'Heap field initialization: closed allocation prefix' in output
                with sqlite3.connect(dbpath) as db:
                    reasons = db.execute("SELECT info FROM checks WHERE info LIKE '%unknown_reason%'").fetchall()
                if case in (0, 13):
                    assert safe and certified and not reasons, (case, wide, output, reasons)
                elif case not in (11, 12):
                    assert not safe, (case, wide, output)
                if case == 21:
                    assert not certified, (case, wide, output)
                print(f'helper prefix={case}/wide={wide}: safe={safe}, certified={certified}', flush=True)


if __name__ == '__main__':
    main()
