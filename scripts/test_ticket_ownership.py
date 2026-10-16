#!/usr/bin/env python3
"""Check parameterized nonwrapping ticket rights without BMC."""
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
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/ticket-ownership.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    cases = list(range(23)) + [24, 26, 27, 28, 29, 30, 31, 32, 33, 36, 37]
    with tempfile.TemporaryDirectory(prefix='ikos-ticket-') as temp:
        work = Path(temp)
        for wide in (False, True):
            for case in cases:
                source = work / 'renamed.c'
                text = fixture.replace('served', 'service_index').replace('next', 'issued_index')
                text = text.replace('protocol', 'work_item').replace('__VERIFIER_atomic_issue', '__VERIFIER_atomic_reserve_ticket')
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') + text)
                proc = subprocess.run([str(args.ikos.resolve()), '--analyses=race',
                    '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                    '--bmc-deadline-ms=0', '-m32', '-o', str(work / 'output.db'), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                assert proc.returncode == 0, (case, wide, proc.stdout, proc.stderr)
                with sqlite3.connect(work / 'output.db') as db:
                    rows = [json.loads(r[0]) for r in db.execute('SELECT info FROM checks')]
                proofs = [r for r in rows if r.get('ticket_ownership_contract')]
                positive = case in (0, 28, 29, 36)
                if case not in (12, 26, 27):
                    assert bool(proofs) == positive, (case, wide, proofs, proc.stdout[-3000:], proc.stderr)
                if positive:
                    assert 'The program is SAFE' in proc.stdout, (case, wide, proc.stdout)
                if case in (7, 9, 10, 12, 16, 17, 18, 20, 21, 22, 24, 26, 27, 33):
                    assert 'The program is SAFE' not in proc.stdout, (case, wide, proc.stdout)
                print(f'ticket case={case}/wide={wide}: proofs={len(proofs)}', flush=True)
    print(f'PASS {len(cases) * 2} ticket controls', flush=True)


if __name__ == '__main__':
    main()
