#!/usr/bin/env python3
"""Check retained ABI refusals and supported native-width certificates."""
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
    identity = runtime_identity(args.ikos)
    command = runtime_command(args.ikos)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith('IKOS_') and k not in {'PYTHONPATH', 'PYTHONHOME'}}
    cases = (
        ('array-slices', 'array_slices_contract'),
        ('bucket-list-ownership', 'bucket_list_contract'),
        ('runtime-stdio', 'runtime_stdio_contract'),
    )
    with tempfile.TemporaryDirectory(prefix='ikos-abi-boundary-') as temporary:
        work = Path(temporary)
        for fixture_name, contract in cases:
            fixture = (ROOT / 'analyzer/test/regression/bmc-values' /
                       (fixture_name + '.c')).read_text()
            for bits in (32, 64):
                source = work / 'renamed.c'
                source.write_text('#define CASE 0\n' + fixture)
                database = work / 'output.db'
                proc = subprocess.run(command + [
                    '--analyses=race', '--concurrency=auto', '--partitioning=guards',
                    '--demote-race-to-unknown', '--bmc-deadline-ms=0',
                    '-m' + str(bits), '-o', str(database), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                assert proc.returncode == 0, (fixture_name, bits, proc.stdout, proc.stderr)
                with sqlite3.connect(database) as db:
                    infos = [json.loads(row[0]) for row in db.execute('SELECT info FROM checks')]
                    result = json.loads(db.execute(
                        "SELECT value FROM settings WHERE name='race_result'").fetchone()[0])
                supported = bits == 32 or fixture_name in {'runtime-stdio', 'array-slices'}
                expected = 'SAFE' if supported else 'UNKNOWN'
                assert result['verdict'] == expected, (fixture_name, bits, result, proc.stdout)
                if contract:
                    proofs = [info for info in infos if info.get(contract)]
                    assert bool(proofs) == supported, (fixture_name, bits, proofs)
                else:
                    assert (result['unresolved_count'] == 0) == supported, result
                print(f'{fixture_name}/bits={bits}: {expected}', flush=True)
    verify_runtime(identity)


if __name__ == '__main__':
    main()
