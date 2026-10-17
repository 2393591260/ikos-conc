#!/usr/bin/env python3
"""Only a real locked observation of a closed monotone flag orders payload reads."""
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
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/guarded-publication.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    with tempfile.TemporaryDirectory(prefix='ikos-guarded-publication-') as tmp:
        work = Path(tmp)
        for wide in (False, True):
            for case in range(32):
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') + fixture)
                proc = subprocess.run([str(args.ikos.resolve()), '--analyses=race',
                    '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                    '--bmc-deadline-ms=0', '-m32', '-o', str(work / 'output.db'), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                output = proc.stdout + proc.stderr
                assert proc.returncode == 0, (case, wide, output)
                with sqlite3.connect(work / 'output.db') as db:
                    rows = [json.loads(r[0]) for r in db.execute('SELECT info FROM checks')]
                proofs = [r for r in rows if r.get('guarded_publication_contract')]
                positive = case in {0, 17, 19, 21, 22, 23, 24, 25, 26, 27}
                assert bool(proofs) == positive, (case, wide, proofs, output)
                if positive:
                    assert 'The program is SAFE' in output, (case, wide, output)
                if case in {1, 2, 3, 4, 6, 8, 18, 20, 29, 30}:
                    assert 'The program is SAFE' not in output, (case, wide, output)
                print(f'guarded publication={case}/wide={wide}: proofs={len(proofs)}', flush=True)


if __name__ == '__main__':
    main()
