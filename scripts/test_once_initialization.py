#!/usr/bin/env python3
"""A sealed mutex gate orders initialization, never ordinary later races."""
import argparse
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile

from ikos_runtime import runtime_command

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--bits', type=int, choices=(32, 64), default=32)
    args = parser.parse_args()
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/once-initialization.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    with tempfile.TemporaryDirectory(prefix='ikos-once-initialization-') as tmp:
        work = Path(tmp)
        for wide in (False, True):
            for case in range(25):
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') + fixture)
                proc = subprocess.run(runtime_command(args.ikos.resolve()) + ['--analyses=race',
                    '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                    '--bmc-deadline-ms=0', '-m' + str(args.bits), '-o', str(work / 'output.db'), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                output = proc.stdout + proc.stderr
                assert proc.returncode == 0, (case, wide, output)
                with sqlite3.connect(work / 'output.db') as db:
                    rows = [json.loads(r[0]) for r in db.execute('SELECT info FROM checks')]
                proofs = [r for r in rows if r.get('once_initialization_contract')]
                safe = case in {0, 6, 23}
                if safe:
                    assert 'The program is SAFE' in output, (case, wide, proofs, output)
                if case in {1, 2, 3, 4, 5, 7, 9, 10, 14, 19, 20, 22, 24}:
                    assert 'The program is SAFE' not in output, (case, wide, proofs, output)
                # Bypass accesses keep their conflicts, although another source
                # with a universal PostGate fact can still have a valid proof.
                if safe:
                    assert proofs, (case, wide, output)
                if case in {7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 21}:
                    assert not proofs, (case, wide, proofs, output)
                print(f'once initialization={case}/wide={wide}: proofs={len(proofs)}', flush=True)


if __name__ == '__main__':
    main()
