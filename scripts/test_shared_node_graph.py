#!/usr/bin/env python3
"""Check immutable shared graph footprints without inventing heap ownership."""
import argparse
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile

from ikos_runtime import runtime_command, runtime_identity, verify_runtime

ROOT = Path(__file__).resolve().parents[1]
POSITIVE = {0, 11, 13, 16}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--case', type=int, choices=range(18), action='append')
    args = parser.parse_args()
    entry = args.ikos.resolve()
    identity = runtime_identity(entry)
    command = runtime_command(entry)
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/shared-node-graph.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    with tempfile.TemporaryDirectory(prefix='ikos-shared-graph-') as temp:
        work = Path(temp)
        for wide in (False, True):
            for case in (range(18) if args.case is None else args.case):
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') +
                                  fixture.replace('worker', 'entry').replace('directory', 'registry'))
                dbpath = work / 'output.db'
                result = subprocess.run(command + ['--analyses=race',
                    '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                    '--bmc-deadline-ms=0', '--log=debug', '-m32', '-o', str(dbpath), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                output = result.stdout + result.stderr
                assert result.returncode == 0, (case, wide, output)
                with sqlite3.connect(dbpath) as db:
                    checks = [json.loads(text) for text, in db.execute('SELECT info FROM checks')]
                proofs = [c for c in checks if c.get('bucket_list_contract') ==
                          'complete-typed-immutable-node-graph-v1']
                safe = 'The program is SAFE' in output
                if case in POSITIVE:
                    assert proofs and safe, (case, wide, checks, output)
                else:
                    assert not safe, (case, wide, checks, output)
                    if case not in {1, 2, 9, 12, 14, 17}:
                        assert not proofs, (case, wide, checks, output)
                if case in {1, 2, 17}:
                    assert proofs, (case, wide, checks, output)
                print(f'CASE={case}/WIDE={int(wide)} safe={safe} proofs={len(proofs)} PASS', flush=True)
    verify_runtime(identity)


if __name__ == '__main__':
    main()
