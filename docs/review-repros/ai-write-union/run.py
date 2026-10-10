#!/usr/bin/env python3
"""All possible write effects survive flush, independently of visiting order."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[3]


def main(args):
    args.output.mkdir(parents=True, exist_ok=False)
    here = Path(__file__).resolve().parent
    flags = shlex.split(subprocess.check_output(
        [args.llvm_config, '--cxxflags', '--ldflags'], text=True))
    unit = args.output / 'summary-unit'
    build = subprocess.run(['g++', *flags, '-fexceptions', '-UNDEBUG', '-O1',
        '-I' + str(ROOT / 'core/include'), '-I' + str(ROOT / 'ar/include'),
        '-I' + str(ROOT / 'analyzer/include'), str(here / 'summary.cpp'),
        '-o', str(unit), '-lgmpxx', '-lgmp', '-lLLVM-14', '-pthread'],
        capture_output=True, text=True, timeout=60)
    (args.output / 'unit-build.log').write_text(build.stdout + build.stderr)
    assert build.returncode == 0, build.stderr
    check = subprocess.run([str(unit)], cwd=args.output, capture_output=True, text=True)
    if not args.observe:
        assert check.returncode == 0, check.stderr
    rows = []
    for bits in (32, 64):
        for case in range(4):
            for mode in ('ai', 'bmc'):
                folder = args.output / f'{bits}-{case}-{mode}'
                folder.mkdir()
                env = dict(os.environ)
                env.pop('IKOS_BMC_RECOVER', None)
                if mode == 'bmc':
                    env['IKOS_BMC_RECOVER'] = '1'
                proc = subprocess.run([
                    str(args.ikos.resolve()), '--analyses=race', '--concurrency=auto',
                    '--demote-race-to-unknown', '-m', str(bits), f'-DCASE={case}',
                    '-o', str(folder / 'output.db'), str(here / 'writers.c'),
                ], cwd=folder, env=env, capture_output=True, text=True, timeout=30)
                (folder / 'run.log').write_text(proc.stdout + proc.stderr)
                verdict = next((s for s in proc.stdout.splitlines()
                                if s.startswith('The program is')), 'NONE')
                assert proc.returncode == 0 and verdict != 'NONE', (folder, proc.stderr)
                if not args.observe:
                    if case == 3:
                        assert verdict != 'The program is definitely UNSAFE', (folder, verdict)
                    else:
                        assert verdict != 'The program is SAFE', (folder, verdict)
                rows.append(dict(bits=bits, case=case, mode=mode, verdict=verdict))
                print(folder.name, verdict, flush=True)
    (args.output / 'summary.json').write_text(json.dumps(
        dict(unit_returncode=check.returncode, unit_stderr=check.stderr, cases=rows), indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--llvm-config', default='llvm-config-14')
    parser.add_argument('--observe', action='store_true')
    main(parser.parse_args())
