#!/usr/bin/env python3
"""A race-only memory projection does not prove callback assertions/completion."""
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
    parser.add_argument('--bits', choices=('32', '64'), default='32')
    args = parser.parse_args()
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/tls-destructor-effects.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    with tempfile.TemporaryDirectory(prefix='ikos-tls-destructor-') as tmp:
        work = Path(tmp)

        def run(case, analyses='race', bmc=False):
            source = work / 'renamed.c'
            source.write_text(f'#define CASE {case}\n' + fixture)
            actual_env = dict(env)
            if bmc:
                actual_env['IKOS_BMC_RECOVER'] = '1'
            proc = subprocess.run([str(args.ikos.resolve()), '--analyses=' + analyses,
                '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                '--bmc-deadline-ms=' + ('5000' if bmc else '0'), '-m' + args.bits,
                '-o', str(work / 'output.db'), str(source)],
                env=actual_env, capture_output=True, text=True, timeout=30)
            output = proc.stdout + proc.stderr
            assert proc.returncode == 0, (case, analyses, bmc, output)
            with sqlite3.connect(work / 'output.db') as db:
                rows = [json.loads(r[0]) for r in db.execute('SELECT info FROM checks')]
            proofs = [r for r in rows if r.get('tls_destructor_contract')]
            safe = analyses == 'race' and case in {0, 1, 2, 3, 22, 23}
            assert ('The program is SAFE' in output) == safe, (case, analyses, proofs, output)
            expected_proof = analyses == 'race' and case in {0, 1, 2, 3, 19, 23}
            assert bool(proofs) == expected_proof, (case, analyses, proofs, output)
            if bmc:
                assert 'The program is UNSAFE' not in output, (case, analyses, output)
            print(f'TLS destructor={case}/analyses={analyses}/bmc={bmc}: proofs={len(proofs)}', flush=True)

        for case in range(26):
            if case == 21:
                continue
            run(case)
        run(0, 'race,prover')
        run(0, 'prover,race')
        run(19, bmc=True)


if __name__ == '__main__':
    main()
