#!/usr/bin/env python3
"""Exact ordinary call contexts preserve lock/data certificates."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'scripts'))
from sweep_bmc_execution import validate_report


def main(args):
    args.output.mkdir(parents=True, exist_ok=False)
    renamed = args.output / 'renamed.c'
    renamed.write_text(Path(__file__).with_name('helper.c').read_text()
                       .replace('entries', 'slots').replace('worker', 'thread_body').replace('store_value', 'assign_cell'))
    rows = []
    for bits in (32, 64):
        for case in range(12):
            for mode in ('ai', 'bmc'):
                folder = args.output / f'{bits}-{case}-{mode}'
                folder.mkdir()
                env = dict(os.environ)
                env.pop('IKOS_BMC_RECOVER', None)
                if mode == 'bmc':
                    env['IKOS_BMC_RECOVER'] = '1'
                source = renamed if bits == 64 else Path(__file__).with_name('helper.c').resolve()
                cmd = [str(args.ikos.resolve()), '--analyses=race', '--concurrency=auto',
                       '--demote-race-to-unknown', '-m', str(bits), f'-DCASE={case}',
                       '--format=json', '--report-file=' + str(folder / 'report.json'),
                       '-o', str(folder / 'output.db'), str(source)]
                proc = subprocess.run(cmd, cwd=folder, env=env, capture_output=True,
                                      text=True, timeout=30)
                (folder / 'run.log').write_text(proc.stdout + proc.stderr)
                verdict = next((s for s in proc.stdout.splitlines()
                                if s.startswith('The program is')), 'NONE')
                assert proc.returncode == 0 and verdict != 'NONE', (folder, proc.stderr)
                safe = case in (0, 3, 6, 7)
                executions = validate_report(json.loads((folder / 'report.json').read_text())) if mode == 'bmc' else []
                if not args.observe:
                    assert verdict != ('The program is definitely UNSAFE' if safe else 'The program is SAFE'), (folder, verdict)
                    if case in (0, 3, 6, 7):
                        assert verdict == 'The program is SAFE', (folder, verdict)
                    if verdict == 'The program is definitely UNSAFE':
                        assert executions and all(e['status'] == 'ACCEPT' for e in executions), (folder, executions)
                rows.append(dict(bits=bits, case=case, mode=mode, command=cmd,
                                 expected='Safe' if safe else 'Race Found',
                                 verdict=verdict, executions=executions))
                print(folder.name, verdict, flush=True)
    (args.output / 'summary.json').write_text(json.dumps(rows, indent=2) + '\n')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--observe', action='store_true')
    main(p.parse_args())
