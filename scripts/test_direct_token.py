#!/usr/bin/env python3
"""Direct global tokens require exact atomic transitions and actual ownership."""
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
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/direct-token.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    with tempfile.TemporaryDirectory(prefix='ikos-direct-token-') as tmp:
        work = Path(tmp)
        for bits in (8, 32, 64):
            for case in range(33):
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n#define TOKEN_BITS {bits}\n' + fixture)
                proc = subprocess.run([str(args.ikos.resolve()), '--analyses=race',
                    '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                    '--bmc-deadline-ms=0', '-m32', '-o', str(work / 'output.db'), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                output = proc.stdout + proc.stderr
                assert proc.returncode == 0, (case, bits, output)
                with sqlite3.connect(work / 'output.db') as db:
                    rows = [json.loads(r[0]) for r in db.execute('SELECT info FROM checks')]
                proofs = [r for r in rows if r.get('binary_token_contract')]
                # Assigning 2 to _Bool stores 1; the wider variants must reject it.
                safe = (case in {0, 1, 2, 3, 27} or (bits == 8 and case == 23)
                        or (bits == 32 and case == 29))
                assert ('The program is SAFE' in output) == safe, (case, bits, proofs, output)
                if safe:
                    assert proofs, (case, bits, output)
                if not safe and case not in {11, 12, 13, 20, 23, 28, 30}:
                    assert not proofs, (case, bits, proofs, output)
                print(f'direct token={case}/bits={bits}: proofs={len(proofs)}', flush=True)


if __name__ == '__main__':
    main()
