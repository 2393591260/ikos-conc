#!/usr/bin/env python3
"""Finite SC protocol fixed point and its conservative boundary."""
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
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/sc-protocol.c').read_text()
    target_lines = {i for i, line in enumerate(fixture.splitlines(), 3) if line.strip() == 'payload = 2;'}
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    with tempfile.TemporaryDirectory(prefix='ikos-sc-protocol-') as temporary:
        work = Path(temporary)
        for wide in (0, 1):
            for case in range(32):
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n#define WIDE {wide}\n' + fixture)
                proc = subprocess.run([str(args.ikos.resolve()), '--analyses=race',
                    '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                    '--bmc-deadline-ms=0', '-m32', '-o', str(work / 'output.db'), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                output = proc.stdout + proc.stderr
                if case == 12:
                    assert proc.returncode != 0 and "unsupported llvm instruction: fence" in output, output
                    print(f"CASE={case}/WIDE={wide}: frontend refuses fence", flush=True)
                    continue
                assert proc.returncode == 0, (case, wide, output)
                with sqlite3.connect(work / 'output.db') as db:
                    infos = [json.loads(r[0]) for r in db.execute('SELECT info FROM checks')]
                proofs = [r for r in infos if r.get('sc_protocol_contract')]
                positive = case in {0, 1, 10, 16, 26}
                target_proofs = [r for r in proofs if target_lines.intersection((r.get('access_a_line'), r.get('access_b_line')))]
                assert bool(target_proofs) == positive, (case, wide, proofs, output)
                if positive or case == 2:
                    assert 'The program is SAFE' in output, (case, wide, output)
                if case in {3, 4, 5, 7, 8, 13, 15, 17, 18, 19, 20, 21, 22, 24}:
                    assert 'The program is SAFE' not in output, (case, wide, output)
                print(f'CASE={case}/WIDE={wide}: proofs={len(proofs)}', flush=True)


if __name__ == '__main__':
    main()
