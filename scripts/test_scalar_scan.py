#!/usr/bin/env python3
"""Check exact scalar scanf effects, failure paths and implicit-state fallback."""
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
    parser.add_argument('--bits', type=int, choices=(32, 64), default=32)
    args = parser.parse_args()
    entry = args.ikos.resolve()
    env = {k: v for k, v in os.environ.items()
           if not k.startswith('IKOS_') and k not in {'PYTHONPATH', 'PYTHONHOME'}}
    identity = runtime_identity(entry)
    command = runtime_command(entry)
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/scalar-scan.c').read_text()
    count = 0
    with tempfile.TemporaryDirectory(prefix='ikos-scalar-scan-') as temp:
        work = Path(temp)
        for mapped in (False, True):
            for case in range(26):
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n' + ('#define MAPPED\n' if mapped else '') + fixture)
                proc = subprocess.run(command + ['--analyses=race', '--concurrency=auto',
                    '--partitioning=guards', '--demote-race-to-unknown', '--bmc-deadline-ms=0',
                    f'-m{args.bits}', '-o', str(work / 'out.db'), str(source)], env=env, text=True,
                    capture_output=True, timeout=30)
                assert proc.returncode == 0, (case, mapped, proc.stdout, proc.stderr)
                with sqlite3.connect(work / 'out.db') as db:
                    rows = [json.loads(r[0]) for r in db.execute('SELECT info FROM checks')]
                contracts = [r for r in rows if r.get('stdio_scan_contract')]
                accepted = case in (0, 1, 2, 3, 4, 5, 16, 19, 20, 22, 23, 24, 25)
                assert bool(contracts) == accepted, (case, mapped, contracts, proc.stdout, proc.stderr, rows)
                safe = 'The program is SAFE' in proc.stdout
                if case in (0, 16, 20, 25):
                    assert safe, (case, mapped, proc.stdout, proc.stderr, rows)
                if case in (1, 2, 3, 4, 5, 15, 17, 18, 19, 21, 22, 23, 24):
                    assert not safe, (case, mapped, proc.stdout, proc.stderr, rows)
                if case in (17, 18):
                    assert any(r.get('unknown_reason') == 'scanf implicit state not covered' for r in rows), (case, mapped, rows)
                if case == 21 and mapped:
                    assert any(r.get('unknown_reason') == 'formatted input effects not covered' for r in rows), rows
                count += 1
                print(f'scalar-scan case={case}/mapped={mapped}: contracts={len(contracts)}; SAFE={safe}', flush=True)
        for case in (3, 4, 5, 15):
            source = work / 'wide.c'
            source.write_text(f'#define CASE {case}\n#define WIDE\n' + fixture)
            proc = subprocess.run(command + ['--analyses=race', '--concurrency=auto',
                '--partitioning=guards', '--demote-race-to-unknown', '--bmc-deadline-ms=0',
                f'-m{args.bits}', '-o', str(work / 'out.db'), str(source)], env=env, text=True,
                capture_output=True, timeout=30)
            assert proc.returncode == 0 and 'The program is SAFE' not in proc.stdout, (case, proc.stdout, proc.stderr)
            count += 1
            print(f'scalar-scan case={case}/wide=True: SAFE=False', flush=True)
    verify_runtime(identity)
    print(f'PASS bits={args.bits} {count} scalar-scan controls', flush=True)


if __name__ == '__main__':
    main()
