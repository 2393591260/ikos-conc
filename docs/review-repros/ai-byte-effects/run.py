#!/usr/bin/env python3
"""Byte effects publish supported scalars and gate unsupported concurrent layouts."""
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
    renamed_source = args.output / 'renamed-case.c'
    renamed_source.write_text(Path(__file__).with_name('copy.c').read_text())
    rows = []
    for bits in (32, 64):
        for case in range(10):
            for wide, renamed in [(0, 0), (1, 0)] + ([(0, 1), (1, 1)] if case in (0, 1) else []):
                for mode in ('ai', 'bmc'):
                    folder = args.output / f'{bits}-{case}-wide{wide}-renamed{renamed}-{mode}'
                    folder.mkdir()
                    env = dict(os.environ)
                    env.pop('IKOS_BMC_RECOVER', None)
                    if mode == 'bmc':
                        env['IKOS_BMC_RECOVER'] = '1'
                    report = folder / 'report.json'
                    proc = subprocess.run([
                        str(args.ikos.resolve()), '--analyses=race', '--concurrency=auto',
                        '--demote-race-to-unknown', '-m', str(bits), f'-DCASE={case}', f'-DWIDE={wide}',
                        *(['-DRENAMED=1'] if renamed else []),
                        '--format=json', '--report-file=' + str(report),
                        '-o', str(folder / 'output.db'),
                        str(renamed_source.resolve() if renamed else Path(__file__).with_name('copy.c').resolve()),
                    ], cwd=folder, env=env, capture_output=True, text=True, timeout=30)
                    (folder / 'run.log').write_text(proc.stdout + proc.stderr)
                    verdict = next((s for s in proc.stdout.splitlines()
                                    if s.startswith('The program is')), 'NONE')
                    assert proc.returncode == 0 and verdict != 'NONE', (folder, proc.stderr)
                    parsed = json.loads(report.read_text())
                    executions = validate_report(parsed) if mode == 'bmc' else []
                    if not args.observe:
                        if case in (1, 8, 9):
                            assert verdict == 'The program is SAFE', (folder, verdict)
                        elif case == 7:
                            assert verdict != 'The program is definitely UNSAFE', (folder, verdict)
                        else:
                            assert verdict != 'The program is SAFE', (folder, verdict)
                        if verdict == 'The program is definitely UNSAFE':
                            assert executions and all(e['status'] == 'ACCEPT' for e in executions), (folder, executions)
                    boundaries = [r for r in parsed['reports'] if (r.get('info') or {}).get('unknown_reason') == 'byte memory effects not fully modeled']
                    if not args.observe and case in (4, 5):
                        assert boundaries and verdict != 'The program is definitely UNSAFE', (folder, verdict, boundaries)
                    rows.append(dict(bits=bits, case=case, wide=wide, renamed=renamed, mode=mode, verdict=verdict,
                                     boundaries=len(boundaries), executions=executions))
                    print(folder.name, verdict, flush=True)
    (args.output / 'summary.json').write_text(json.dumps(rows, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--observe', action='store_true')
    main(parser.parse_args())
