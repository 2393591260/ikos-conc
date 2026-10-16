#!/usr/bin/env python3
"""Once prefixes bind actual constant helper arguments and certified gate events."""
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
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/direct-once.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    cases = (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18,
             20, 22, 23, 24, 25, 30)
    with tempfile.TemporaryDirectory(prefix='ikos-direct-once-') as tmp:
        work = Path(tmp)
        for native in (False, True):
            for wide in (False, True):
                for case in cases:
                    source = work / 'renamed.c'
                    source.write_text(f'#define CASE {case}\n' +
                        ('#define WIDE\n' if wide else '') +
                        ('#define NATIVE\n' if native else '') + fixture)
                    proc = subprocess.run([str(args.ikos.resolve()), '--analyses=race',
                        '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                        '--bmc-deadline-ms=0', '-m32', '-o', str(work / 'output.db'), str(source)],
                        env=env, capture_output=True, text=True, timeout=30)
                    output = proc.stdout + proc.stderr
                    assert proc.returncode == 0, (case, native, wide, output)
                    with sqlite3.connect(work / 'output.db') as db:
                        rows = [json.loads(r[0]) for r in db.execute('SELECT info FROM checks')]
                    proofs = [r for r in rows if r.get('once_initialization_contract')]
                    safe = case in {0, 1, 2, 3} or (native and case in {8, 9, 15})
                    assert ('The program is SAFE' in output) == safe, (case, native, wide, proofs, output)
                    if safe:
                        assert proofs, (case, native, wide, output)
                        assert all(bool(r.get('gate_token')) != native for r in proofs)
                    if not safe and case != 14:
                        assert not proofs, (case, native, wide, proofs, output)
                    print(f'direct once={case}/native={native}/wide={wide}: proofs={len(proofs)}', flush=True)


if __name__ == '__main__':
    main()
