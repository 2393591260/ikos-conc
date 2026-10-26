#!/usr/bin/env python3
"""Check closed bucket graphs, real mutex ownership and invalidating effects."""
import argparse
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile

from ikos_runtime import runtime_command, runtime_identity, verify_runtime

ROOT = Path(__file__).resolve().parents[1]
CASES = (0, 1, 2, 4, 5, 8, 9, 10, 11, 12, 14, 15, 18, 20, 21, 22, 23,
         24, 25, 26, 27, 28, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 61, 62, 63, 64, 65, 66, 67, 69,
         70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81)
# CASE26 omits reset.prev, but insertion initializes every required prev cell.
POSITIVE = {0, 23, 26, 33, 37, 48, 64, 65, 69, 70, 71, 80, 81}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--bits', choices=('32', '64'), default='32')
    parser.add_argument('--case', type=int, choices=CASES, action='append')
    args = parser.parse_args()
    entry = args.ikos.resolve()
    identity = runtime_identity(entry)
    command = runtime_command(entry)
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/bucket-list-ownership.c').read_text()
    env = {k: v for k, v in os.environ.items()
           if not k.startswith('IKOS_') and k not in {'PYTHONPATH', 'PYTHONHOME'}}
    with tempfile.TemporaryDirectory(prefix='ikos-bucket-list-') as temp:
        work = Path(temp)
        for wide in (False, True):
            for case in (CASES if args.case is None else args.case):
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n' +
                                  ('#define WIDE\n' if wide else '') + fixture)
                dbpath = work / 'output.db'
                result = subprocess.run(command + ['--analyses=race',
                    '--concurrency=auto', '--partitioning=guards',
                    '--demote-race-to-unknown', '--bmc-deadline-ms=0',
                    '--log=debug', '-m' + args.bits, '-o', str(dbpath), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                output = result.stdout + result.stderr
                assert result.returncode == 0, (case, wide, output)
                with sqlite3.connect(dbpath) as db:
                    checks = [json.loads(text) for text, in
                              db.execute('SELECT info FROM checks')]
                proofs = [c for c in checks if c.get('bucket_list_contract')]
                safe = 'The program is SAFE' in output
                if case in POSITIVE:
                    assert proofs and safe, (case, wide, checks, output)
                else:
                    assert not proofs, (case, wide, checks, output)
                    # CASE2 is a safe lock permutation outside this subset.
                    if case in {1, 5, 12, 15, 49, 53, 72}:
                        assert not safe, (case, wide, checks, output)
                print(f'CASE={case}/WIDE={int(wide)}/bits={args.bits}: safe={safe}, '
                      f'proofs={len(proofs)}', flush=True)
    verify_runtime(identity)


if __name__ == '__main__':
    main()
