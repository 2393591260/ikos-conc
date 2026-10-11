#!/usr/bin/env python3
"""Cycle-safe diagnostics preserve concrete race evidence and acyclic formatting."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'scripts'))
from sweep_bmc_execution import validate_report


def main(args):
    args.output.mkdir(parents=True, exist_ok=False)
    if not args.observe:
        flags = shlex.split(subprocess.check_output(
            ['llvm-config-14', '--cxxflags', '--ldflags'], text=True))
        cmd = ['g++', *flags, '-std=c++17', '-fexceptions', '-UNDEBUG', '-O1',
               '-I' + str(ROOT / 'ar/include'), '-I' + str(ROOT / 'core/include'),
               str(Path(__file__).with_name('types.cpp')),
               str(ROOT / 'build/ar/libikos-ar.a'), '-o', str(args.output / 'unit'),
               '-lgmpxx', '-lgmp', '-lLLVM-14', '-pthread']
        compiled = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        (args.output / 'unit-build.log').write_text(compiled.stdout + compiled.stderr)
        assert compiled.returncode == 0, compiled.stderr
        subprocess.run([str(args.output / 'unit')], check=True, timeout=10)
    rows = []
    for bits in (32, 64):
        for case in range(4):
            for mode in ('ai', 'bmc'):
                folder = args.output / f'{bits}-{case}-{mode}'; folder.mkdir()
                env = dict(os.environ)
                env.pop('IKOS_BMC_RECOVER', None)
                env['IKOS_BMC_DIAGNOSTICS'] = '1'
                if mode == 'bmc': env['IKOS_BMC_RECOVER'] = '1'
                cmd = [str(args.ikos.resolve()), '--analyses=race', '--concurrency=auto',
                       '--demote-race-to-unknown', '-m', str(bits), f'-DCASE={case}',
                       '--format=json', '--report-file=' + str(folder / 'report.json'),
                       '-o', str(folder / 'output.db'),
                       str(Path(__file__).with_name('recursive.c').resolve())]
                proc = subprocess.run(cmd, cwd=folder, env=env, capture_output=True,
                                      text=True, timeout=30)
                (folder / 'run.log').write_text(proc.stdout + proc.stderr)
                verdict = next((s for s in proc.stdout.splitlines()
                                if s.startswith('The program is')), 'NONE')
                executions = []
                if proc.returncode == 0:
                    report = json.loads((folder / 'report.json').read_text())
                    report['reports'] = [r for r in report['reports'] if isinstance(r.get('info'), dict)]
                    if mode == 'bmc': executions = validate_report(report)
                if not args.observe:
                    assert proc.returncode == 0 and verdict != 'NONE', (folder, proc.stderr)
                    if case == 3: assert verdict == 'The program is SAFE', (folder, verdict)
                    elif mode == 'bmc':
                        assert verdict == 'The program is definitely UNSAFE', (folder, verdict)
                        assert executions and all(e['status'] == 'ACCEPT' for e in executions), (folder, executions)
                    else: assert verdict != 'The program is SAFE', (folder, verdict)
                rows.append(dict(bits=bits, case=case, mode=mode, command=cmd,
                                 returncode=proc.returncode, verdict=verdict, executions=executions))
                print(folder.name, proc.returncode, verdict, flush=True)
    (args.output / 'summary.json').write_text(json.dumps(rows, separators=(',', ':')) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--observe', action='store_true')
    main(parser.parse_args())
