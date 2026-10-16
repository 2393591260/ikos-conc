#!/usr/bin/env python3
"""Worker allocation privacy requires closed provenance, lifetime and extent."""
import argparse
import json
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
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/thread-private-heap.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    with tempfile.TemporaryDirectory(prefix='ikos-worker-heap-') as temp:
        work = Path(temp)
        for wide in (False, True):
            for case in range(28):
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') + fixture)
                proc = subprocess.run([str(args.ikos.resolve()), '--analyses=race',
                    '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                    '--bmc-deadline-ms=0', '-m32', '-o', str(work / 'output.db'), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                output = proc.stdout + proc.stderr
                assert proc.returncode == 0, (case, wide, output)
                with sqlite3.connect(work / 'output.db') as db:
                    rows = [json.loads(row[0]) for row in db.execute('SELECT info FROM checks')]
                proofs = [r for r in rows if r.get('private_heap_contract')]
                positive = case in {0, 1, 12, 22, 24} or (case == 11 and wide)
                assert bool(proofs) == positive, (case, wide, proofs, output)
                if positive and case != 24:
                    assert 'The program is SAFE' in output, (case, wide, output)
                if case == 24:
                    assert 'The program is SAFE' not in output, (wide, output)
                print(f'private heap={case}/wide={wide}: proofs={len(proofs)}', flush=True)


if __name__ == '__main__':
    main()
