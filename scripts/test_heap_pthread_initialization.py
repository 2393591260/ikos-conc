#!/usr/bin/env python3
"""Check shared heap initialization effects and preserve real conflicting pairs."""
import argparse
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile

from ikos_runtime import runtime_command, runtime_identity, verify_runtime

ROOT = Path(__file__).resolve().parents[1]
CASES = tuple(range(25)) + (26, 27, 28, 29, 30, 31)
POSITIVE = {0, 20, 21, 27}
# These source frames remain valid; their data accesses genuinely conflict.
RACY = {1, 2}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--case', type=int, choices=CASES, action='append')
    args = parser.parse_args()
    entry = args.ikos.resolve()
    identity = runtime_identity(entry)
    command = runtime_command(entry)
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/heap-pthread-initialization.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    with tempfile.TemporaryDirectory(prefix='ikos-heap-pthread-init-') as temp:
        work = Path(temp)
        for wide in (False, True):
            for case in (CASES if args.case is None else args.case):
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n' +
                                  ('#define WIDE\n' if wide else '') + fixture)
                database = work / 'output.db'
                result = subprocess.run(command + ['--analyses=race',
                    '--concurrency=auto', '--partitioning=guards',
                    '--demote-race-to-unknown', '--bmc-deadline-ms=0',
                    '--log=debug', '-m32', '-o', str(database), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                output = result.stdout + result.stderr
                assert result.returncode == 0, (case, wide, output)
                with sqlite3.connect(database) as db:
                    infos = [json.loads(row) for row, in db.execute('SELECT info FROM checks')]
                proofs = [row for row in infos if row.get('pthread_initialization_contract') ==
                          'constant-heap-runtime-byte-frame-v1']
                safe = 'The program is SAFE' in output
                if case in POSITIVE:
                    assert proofs and safe, (case, wide, infos, output)
                elif case in RACY:
                    assert proofs and not safe, (case, wide, infos, output)
                else:
                    assert not proofs, (case, wide, infos, output)
                print(f'CASE={case}/WIDE={int(wide)}: safe={safe}, '
                      f'proofs={len(proofs)}', flush=True)
    verify_runtime(identity)


if __name__ == '__main__':
    main()
