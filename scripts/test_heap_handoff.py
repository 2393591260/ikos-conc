#!/usr/bin/env python3
"""Object handoff requires a fresh allocation before each actual create."""
import argparse
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile

from ikos_runtime import runtime_command, runtime_identity, verify_runtime

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--bits', choices=('32', '64'), default='32')
    args = parser.parse_args()
    command = runtime_command(args.ikos.resolve())
    identity = runtime_identity(args.ikos.resolve())
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/heap-handoff.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    with tempfile.TemporaryDirectory(prefix='ikos-heap-handoff-') as temp:
        work = Path(temp)
        for wide in (False, True):
            for case in range(31):
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') + fixture)
                proc = subprocess.run([*command, '--analyses=race',
                    '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                    '--bmc-deadline-ms=0', '-m' + args.bits, '-o', str(work / 'output.db'), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                output = proc.stdout + proc.stderr
                assert proc.returncode == 0, (case, wide, output)
                with sqlite3.connect(work / 'output.db') as db:
                    rows = [json.loads(row[0]) for row in db.execute('SELECT info FROM checks')]
                proofs = [r for r in rows if r.get('private_heap_contract') == 'closed-main-child-malloc-handoff-v1']
                positive = case in {0, 18, 19, 20, 21, 28, 29, 30}
                assert bool(proofs) == positive, (case, wide, proofs, output)
                if positive and case != 28:
                    assert 'The program is SAFE' in output, (case, wide, output)
                if case in {1, 28}:
                    assert 'The program is SAFE' not in output, (case, wide, output)
                print(f'heap handoff={case}/wide={wide}: proofs={len(proofs)}', flush=True)

    verify_runtime(identity)


if __name__ == '__main__':
    main()
