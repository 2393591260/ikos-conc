#!/usr/bin/env python3
"""Check independent mutex-array effect ranges and unsafe graph controls."""
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
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/mutex-array-effects.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    with tempfile.TemporaryDirectory(prefix='ikos-mutex-effects-') as temp:
        work = Path(temp)
        for wide in (False, True):
            for case in range(22):
                source = work / 'renamed.c'
                text = fixture.replace('make_element', 'allocate_node').replace('require_condition', 'guard_value')
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') + text)
                database = work / 'output.db'
                proc = subprocess.run([str(args.ikos.resolve()), '--analyses=race',
                    '--concurrency=auto', '--demote-race-to-unknown', '--partitioning=guards',
                    '--bmc-deadline-ms=0', '--log=debug', '-m32', '-o', str(database), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                assert proc.returncode == 0, (case, wide, proc.stdout, proc.stderr)
                safe = 'The program is SAFE' in proc.stdout
                certified = 'mutex-array preflight certified' in proc.stderr + proc.stdout
                # CASE1 intentionally puts the index behind a helper: it is a
                # valid program, but scalar source closure may still reject it.
                if case in {0, 15, 21}:
                    assert safe and certified, (case, wide, proc.stdout, proc.stderr)
                elif case not in {1, 14}:
                    assert not safe, (case, wide, proc.stdout, proc.stderr)
                # Memory-independent ranges do not trust shared guard values.
                if case in {3, 4, 5, 6, 7, 8, 10, 12, 13, 14, 16, 17, 20}:
                    assert not certified, (case, wide, proc.stdout, proc.stderr)
                if case == 17:
                    assert 'incomplete mutex-array preflight' in proc.stdout + proc.stderr, (
                        case, wide, proc.stdout, proc.stderr)
                    assert any(v in proc.stdout for v in ('The program is UNKNOWN',
                               'The program is potentially UNSAFE')), (case, wide, proc.stdout)
                if case in {18, 19}:
                    assert certified, (case, wide, proc.stdout, proc.stderr)
                with sqlite3.connect(database) as db:
                    proofs = db.execute("SELECT info FROM checks WHERE info LIKE '%indexed-live-heap-owner-v1%'").fetchall()
                if case in {0, 15}:
                    assert proofs, (case, wide, proc.stdout)
                print(f'mutex effects case={case}/wide={wide}: safe={safe}, certified={certified}', flush=True)


if __name__ == '__main__':
    main()
