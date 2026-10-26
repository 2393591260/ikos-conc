#!/usr/bin/env python3
"""Reject array-slice certificates missing any reachable access occurrence."""
import argparse
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile

from ikos_runtime import runtime_command, runtime_identity, verify_runtime

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from svcomp_race_test import classify, VERDICT_SAFE, VERDICT_UNKNOWN


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    args = parser.parse_args()
    identity = runtime_identity(args.ikos.resolve())
    command = runtime_command(args.ikos.resolve())
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/array-slice-occurrences.c').read_text()
    env = {k: v for k, v in os.environ.items()
           if not k.startswith('IKOS_') and k not in ('PYTHONPATH', 'PYTHONHOME')}
    with tempfile.TemporaryDirectory(prefix='ikos-slice-occurrences-') as temporary:
        work = Path(temporary)
        for wide in (False, True):
            for case in range(6):
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') + fixture)
                database = work / 'output.db'
                proc = subprocess.run(command + ['--analyses=race', '--concurrency=auto',
                    '--partitioning=guards', '--demote-race-to-unknown',
                    '--bmc-deadline-ms=0', '-m32', '-o', str(database), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                output = proc.stdout + proc.stderr
                assert proc.returncode == 0, (case, wide, output)
                with sqlite3.connect(database) as db:
                    proofs = [json.loads(row[0]) for row in db.execute('SELECT info FROM checks')
                              if json.loads(row[0]).get('array_slices_contract')]
                positive = case in (0, 3)
                assert bool(proofs) == positive, (case, wide, proofs, output)
                verdict = classify(proc.stdout, proc.returncode, False)
                assert verdict == (VERDICT_SAFE if positive else VERDICT_UNKNOWN), (case, wide, output)
                print(f'case={case}/wide={wide}: proofs={len(proofs)}', flush=True)
    verify_runtime(identity)


if __name__ == '__main__':
    main()
