#!/usr/bin/env python3
"""Unavailable call bodies cannot silently supply a SAFE proof."""
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
    renamed_source = args.output / "other-call.c"
    renamed_source.write_text(Path(__file__).with_name("calls.c").read_text())
    rows = []
    for bits in (32, 64):
        for case in range(18):
            for renamed in ([False, True] if case in (0, 6) else [False]):
                for mode in ('ai', 'bmc'):
                    folder = args.output / f'{bits}-{case}-renamed{int(renamed)}-{mode}'
                    folder.mkdir()
                    env = dict(os.environ)
                    env.pop('IKOS_BMC_RECOVER', None)
                    if mode == 'bmc':
                        env['IKOS_BMC_RECOVER'] = '1'
                    report = folder / 'report.json'
                    proc = subprocess.run([
                        str(args.ikos.resolve()), '--analyses=race', '--concurrency=auto',
                        '--demote-race-to-unknown', '-m', str(bits), f'-DCASE={case}',
                        *(['-DRENAMED=1'] if renamed else []),
                        '--format=json', '--report-file=' + str(report),
                        '-o', str(folder / 'output.db'),
                        str(renamed_source.resolve() if renamed else Path(__file__).with_name('calls.c').resolve()),
                    ], cwd=folder, env=env, capture_output=True, text=True, timeout=30)
                    (folder / 'run.log').write_text(proc.stdout + proc.stderr)
                    verdict = next((s for s in proc.stdout.splitlines()
                                    if s.startswith('The program is')), 'NONE')
                    assert proc.returncode == 0 and verdict != 'NONE', (folder, proc.stderr)
                    parsed = json.loads(report.read_text())
                    executions = validate_report(parsed) if mode == 'bmc' else []
                    if not args.observe:
                        if case in (3, 6, 7, 10, 14, 16, 17):
                            assert verdict == 'The program is SAFE', (folder, verdict)
                        else:
                            assert verdict != 'The program is SAFE', (folder, verdict)
                        if verdict == 'The program is definitely UNSAFE':
                            assert executions and all(e['status'] == 'ACCEPT' for e in executions), (folder, executions)
                    boundaries = [r for r in parsed['reports']
                                  if (r.get('info') or {}).get('unknown_reason') == 'external call effects not covered']
                    if not args.observe and case in (0, 4, 5, 8, 9, 11, 12, 13, 15):
                        assert boundaries, (folder, verdict)
                        assert verdict != 'The program is definitely UNSAFE', (folder, verdict)
                    rows.append(dict(bits=bits, case=case, mode=mode, renamed=renamed, verdict=verdict,
                                     boundaries=len(boundaries), executions=executions))
                    print(folder.name, verdict, flush=True)
    for source, cases, reason in [('recursive.c', range(2), 'internal call effects not covered'),
                                  ('assertion.c', range(5), 'external call effects not covered')]:
        for bits in (32, 64):
            for case in cases:
                for mode in ('ai', 'bmc'):
                    folder = args.output / f'{source}-{bits}-{case}-{mode}'
                    folder.mkdir()
                    env = dict(os.environ)
                    env.pop('IKOS_BMC_RECOVER', None)
                    if mode == 'bmc':
                        env['IKOS_BMC_RECOVER'] = '1'
                    report = folder / 'report.json'
                    define = '-DCONTROL=' if source == 'recursive.c' else '-DCASE='
                    proc = subprocess.run([
                        str(args.ikos.resolve()), '--analyses=race', '--concurrency=auto',
                        '--demote-race-to-unknown', '-m', str(bits), define + str(case),
                        '--format=json', '--report-file=' + str(report),
                        '-o', str(folder / 'output.db'),
                        str(Path(__file__).with_name(source).resolve()),
                    ], cwd=folder, env=env, capture_output=True, text=True, timeout=30)
                    (folder / 'run.log').write_text(proc.stdout + proc.stderr)
                    verdict = next((s for s in proc.stdout.splitlines()
                                    if s.startswith('The program is')), 'NONE')
                    assert proc.returncode == 0 and verdict != 'NONE', (folder, proc.stderr)
                    parsed = json.loads(report.read_text())
                    boundaries = [r for r in parsed['reports']
                                  if (r.get('info') or {}).get('unknown_reason') == reason]
                    if not args.observe:
                        if source == 'assertion.c' and case in (0, 1, 2):
                            assert verdict == 'The program is SAFE', (folder, verdict)
                            assert not boundaries, folder
                        else:
                            assert verdict == 'The program is potentially UNSAFE', (folder, verdict)
                        if source == 'recursive.c' or (source == 'assertion.c' and case == 3):
                            assert boundaries, folder
                    rows.append(dict(source=source, bits=bits, case=case, mode=mode,
                                     verdict=verdict, boundaries=len(boundaries)))
                    print(folder.name, verdict, flush=True)
    (args.output / 'summary.json').write_text(json.dumps(rows, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--observe', action='store_true')
    main(parser.parse_args())
