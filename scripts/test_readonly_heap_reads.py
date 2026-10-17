#!/usr/bin/env python3
"""Check whole initialization, immutable lookup bounds and unrelated races."""
import argparse
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile

from ikos_runtime import runtime_command, runtime_identity, verify_runtime

ROOT = Path(__file__).resolve().parents[1]
CASES = (*range(20), 21, 22, 23, 24, 25, 27, 28, 29, 31,
         33, 34, 35, 36, 37, 38, 39, 41, 42, 43, 45, 46, 47)
POSITIVE = {0, 7, 24, 33}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--case', type=int, choices=CASES, action='append')
    args = parser.parse_args()
    entry = args.ikos.resolve()
    identity = runtime_identity(entry)
    command = runtime_command(entry)
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/readonly-heap-reads.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    with tempfile.TemporaryDirectory(prefix='ikos-readonly-heap-') as temp:
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
                    '--log=debug', '-m32', '-o', str(dbpath), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                output = result.stdout + result.stderr
                assert result.returncode == 0, (case, wide, output)
                with sqlite3.connect(dbpath) as db:
                    checks = [json.loads(text) for text, in
                              db.execute('SELECT info FROM checks')]
                proofs = [c for c in checks if c.get('readonly_heap_reads_contract')]
                safe = 'The program is SAFE' in output
                if case in POSITIVE:
                    assert proofs, (case, wide, checks, output)
                    assert safe == (case != 7), (case, wide, checks, output)
                elif case in {27, 28}:
                    # Heap provenance cannot order a remaining main/output race.
                    assert not safe, (case, wide, checks, output)
                else:
                    assert not proofs, (case, wide, checks, output)
                print(f'CASE={case}/WIDE={int(wide)}: safe={safe}, '
                      f'proofs={len(proofs)}', flush=True)
    verify_runtime(identity)


if __name__ == '__main__':
    main()
