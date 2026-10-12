#!/usr/bin/env python3
"""Check endpoint, nested protection, ongoing writers and coverage boundaries."""
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
            [args.llvm_config, '--cxxflags', '--ldflags'], text=True))
        unit = args.output / 'summary-unit'
        compiled = subprocess.run(['g++', *flags, '-fexceptions', '-UNDEBUG', '-O1',
            '-I' + str(ROOT / 'core/include'), '-I' + str(ROOT / 'ar/include'),
            '-I' + str(ROOT / 'analyzer/include'),
            '-I' + str(ROOT / 'frontend/llvm/include'),
            str(Path(__file__).with_name('summary.cpp')),
            str(ROOT / 'analyzer/src/analysis/memory_location.cpp'),
            str(ROOT / 'build/ar/libikos-ar.a'), '-o', str(unit),
            '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
            '-lgmpxx', '-lgmp', '-lLLVM-14', '-pthread'],
            capture_output=True, text=True, timeout=60)
        (args.output / 'unit-build.log').write_text(compiled.stdout + compiled.stderr)
        assert compiled.returncode == 0, compiled.stderr
        subprocess.run([str(unit)], check=True, timeout=30)
    renamed = args.output / 'renamed.c'
    renamed.write_text(Path(__file__).with_name('release.c').read_text()
                       .replace('outer', 'primary').replace('inner', 'secondary')
                       .replace('reader', 'observer'))
    rows = []
    for bits in (32, 64):
        for case in range(17):
            for mode in ('ai', 'bmc'):
                folder = args.output / f'{bits}-{case}-{mode}'
                folder.mkdir()
                env = dict(os.environ)
                env.pop('IKOS_BMC_RECOVER', None)
                if mode == 'bmc':
                    env['IKOS_BMC_RECOVER'] = '1'
                command = [str(args.ikos.resolve()), '--analyses=race', '--concurrency=auto',
                           '--demote-race-to-unknown', '-m', str(bits), f'-DCASE={case}',
                           '--format=json', '--report-file=' + str(folder / 'report.json'),
                           '-o', str(folder / 'output.db')]
                if bits == 64:
                    command.append('-DWIDE')
                command.append(str(renamed if bits == 64 else Path(__file__).with_name('release.c').resolve()))
                proc = subprocess.run(command, cwd=folder, env=env, capture_output=True,
                                      text=True, timeout=30)
                (folder / 'run.log').write_text(proc.stdout + proc.stderr)
                verdict = next((line for line in proc.stdout.splitlines()
                                if line.startswith('The program is')), 'NONE')
                assert proc.returncode == 0 and verdict != 'NONE', (folder, proc.stderr)
                safe = case in (0, 3, 6, 9, 14, 15, 16)
                executions = validate_report(json.loads((folder / 'report.json').read_text())) if mode == 'bmc' else []
                if not args.observe:
                    assert verdict != ('The program is definitely UNSAFE' if safe else 'The program is SAFE'), (folder, verdict)
                    if case in (0, 3, 14, 15, 16):
                        assert verdict == 'The program is SAFE', (folder, verdict)
                    if verdict == 'The program is definitely UNSAFE':
                        assert executions and all(e['status'] == 'ACCEPT' for e in executions), (folder, executions)
                rows.append(dict(bits=bits, case=case, mode=mode,
                                 expected='Safe' if safe else ('Boundary' if case in (7, 11, 12, 13) else 'Race Found'),
                                 verdict=verdict, executions=executions))
                print(folder.name, verdict, flush=True)
    for bits in (32, 64):
        for mode in ('ai', 'bmc'):
            folder = args.output / f'exception-{bits}-{mode}'
            folder.mkdir()
            bitcode = folder / 'exception.bc'
            compiled = subprocess.run(['clang-14', '-emit-llvm', '-c', '-g', '-O0',
                '-Xclang', '-disable-O0-optnone', '-fno-discard-value-names',
                '-m' + str(bits), str(Path(__file__).with_name('exception.cc').resolve()),
                '-o', str(bitcode)], capture_output=True, text=True, timeout=30)
            (folder / 'clang.log').write_text(compiled.stdout + compiled.stderr)
            assert compiled.returncode == 0, compiled.stderr
            env = dict(os.environ)
            env.pop('IKOS_BMC_RECOVER', None)
            if mode == 'bmc':
                env['IKOS_BMC_RECOVER'] = '1'
            command = [str(args.ikos.resolve()), '--analyses=race', '--concurrency=auto',
                       '--demote-race-to-unknown', '-m', str(bits),
                       '--format=json', '--report-file=' + str(folder / 'report.json'),
                       '-o', str(folder / 'output.db'),
                       str(bitcode)]
            proc = subprocess.run(command, cwd=folder, env=env, capture_output=True,
                                  text=True, timeout=30)
            (folder / 'run.log').write_text(proc.stdout + proc.stderr)
            verdict = next((line for line in proc.stdout.splitlines()
                            if line.startswith('The program is')), 'NONE')
            assert proc.returncode == 0 and verdict == 'The program is potentially UNSAFE', (folder, proc.stderr, verdict)
            rows.append(dict(bits=bits, case='exception', mode=mode,
                             expected='Boundary', verdict=verdict, executions=[]))
            print(folder.name, verdict, flush=True)
    (args.output / 'summary.json').write_text(json.dumps(rows, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--observe', action='store_true')
    parser.add_argument('--llvm-config', default='llvm-config-14')
    main(parser.parse_args())
