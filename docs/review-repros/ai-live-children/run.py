#!/usr/bin/env python3
"""Conditional lifetime joins must preserve live instances across CFG merges."""
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
    unit = args.output / 'domain-unit'
    built = subprocess.run(['g++', '-std=c++17', '-I' + str(ROOT / 'core/include'),
                            str(Path(__file__).with_name('domain.cpp')), '-o', str(unit)],
                           capture_output=True, text=True, timeout=30)
    (args.output / 'unit-build.log').write_text(built.stdout + built.stderr)
    assert built.returncode == 0, built.stderr
    subprocess.run([str(unit)], check=True, timeout=10)
    renamed = args.output / 'renamed.c'
    renamed.write_text(Path(__file__).with_name('lifetime.c').read_text()
                       .replace('payload', 'cell').replace('handle', 'token')
                       .replace('factory', 'producer').replace('worker', 'child'))
    rows = []
    for bits in (32, 64):
        for case in range(23):
            for mode in ('ai', 'bmc'):
                folder = args.output / f'{bits}-{case}-{mode}'
                folder.mkdir()
                env = dict(os.environ)
                env.pop('IKOS_BMC_RECOVER', None)
                if mode == 'bmc':
                    env['IKOS_BMC_RECOVER'] = '1'
                source = renamed if bits == 64 else Path(__file__).with_name('lifetime.c').resolve()
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
                safe = case in (0, 1, 8, 12, 17, 18, 21)
                report = json.loads((folder / 'report.json').read_text())
                report['reports'] = [r for r in report['reports'] if isinstance(r.get('info'), dict)]
                executions = validate_report(report) if mode == 'bmc' else []
                if not args.observe:
                    assert verdict != ('The program is definitely UNSAFE' if safe else 'The program is SAFE'), (folder, verdict)
                    if case in (0, 1, 8, 17, 18):
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
