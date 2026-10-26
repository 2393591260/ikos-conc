#!/usr/bin/env python3
"""Uniform private-output effects preserve once-gate ordering, not atomicity."""
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
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/token-once.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    with tempfile.TemporaryDirectory(prefix='ikos-token-once-') as tmp:
        work = Path(tmp)
        for native in (False, True):
            for wide in (False, True):
                for case in range(27):
                    source = work / 'renamed.c'
                    source.write_text(f'#define CASE {case}\n' +
                        ('#define WIDE\n' if wide else '') +
                        ('#define NATIVE\n' if native else '') + fixture)
                    proc = subprocess.run(runtime_command(args.ikos.resolve()) + ['--analyses=race',
                        '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                        '--bmc-deadline-ms=0', '-m' + str(args.bits), '-o', str(work / 'output.db'), str(source)],
                        env=env, capture_output=True, text=True, timeout=30)
                    output = proc.stdout + proc.stderr
                    assert proc.returncode == 0, (case, wide, native, output)
                    with sqlite3.connect(work / 'output.db') as db:
                        rows = [json.loads(r[0]) for r in db.execute('SELECT info FROM checks')]
                    proofs = [r for r in rows if r.get('once_initialization_contract')]
                    safe = case in {0, 1, 2, 3}
                    assert ('The program is SAFE' in output) == safe, (case, wide, native, proofs, output)
                    if safe or case in {15, 23}:
                        assert proofs, (case, wide, native, output)
                        assert all(bool(r.get('gate_token')) != native for r in proofs)
                    if case not in {0, 1, 2, 3, 15, 23}:
                        assert not proofs, (case, wide, native, proofs, output)
                    print(f'token once={case}/wide={wide}/native={native}: proofs={len(proofs)}', flush=True)


if __name__ == '__main__':
    main()
