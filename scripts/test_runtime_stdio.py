#!/usr/bin/env python3
"""Check complete stdio frames, private handle ranges and refusal variants."""
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
    args = parser.parse_args()
    entry = args.ikos.resolve()
    identity = runtime_identity(entry)
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/runtime-stdio.c').read_text()
    variants = [(case, (), case in (0, 1)) for case in range(32)]
    variants += [(0, ('LIMIT 37',), True), (0, ('INITIAL 0',), True),
                 (0, ('INITIAL -2',), True), (0, ('WIDE',), True),
                 (1, ('WIDE',), True), (0, ('MAPPED',), True),
                 (0, ('population renamed_count', 'payload renamed_payload',
                      'worker renamed_worker', 'handles renamed_slots'), True)]
    with tempfile.TemporaryDirectory(prefix='ikos-runtime-stdio-') as temp:
        work = Path(temp)
        for case, definitions, expected in variants:
            source = work / 'independent.c'
            source.write_text(f'#define CASE {case}\n' + ''.join(
                f'#define {definition}\n' for definition in definitions) + fixture)
            proc = subprocess.run(runtime_command(entry) + [
                '--analyses=race', '--concurrency=auto', '--partitioning=guards',
                '--demote-race-to-unknown', '--bmc-deadline-ms=0', '-m32',
                '-o', str(work / 'out.db'), str(source)],
                env=env, text=True, capture_output=True, timeout=30)
            assert proc.returncode == 0, (case, definitions, proc.stdout, proc.stderr)
            with sqlite3.connect(work / 'out.db') as db:
                rows = [json.loads(row[0]) for row in db.execute('SELECT info FROM checks')]
            contracts = [row for row in rows if row.get('runtime_stdio_contract')]
            assert bool(contracts) == expected, (case, definitions, rows, proc.stdout, proc.stderr)
            safe = 'The program is SAFE' in proc.stdout
            if case == 0:
                assert safe, (case, definitions, rows, proc.stdout, proc.stderr)
            if case == 1:
                assert not safe and any(row.get('conflicting_accesses') for row in rows), (
                    case, definitions, rows, proc.stdout, proc.stderr)
            print(f'runtime-stdio case={case} defs={definitions}: '
                  f'frame={bool(contracts)} SAFE={safe}', flush=True)
    verify_runtime(identity)
    print(f'PASS {len(variants)} runtime-stdio controls', flush=True)


if __name__ == '__main__':
    main()
