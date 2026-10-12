#!/usr/bin/env python3
"""Check conservative string effects, synchronization and representation changes."""
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
    original = Path(__file__).with_name('copy.c').resolve()
    renamed = args.output / 'renamed.c'
    renamed.write_text(original.read_text().replace('writer', 'copy_worker')
                       .replace('reader', 'observe_worker').replace('payload', 'result_cell'))
    rows = []
    for bits in (32, 64):
        # The native pointer fixtures exercise actual legal x86 object bytes.
        if not args.observe:
            for case in (10, 11, 16):
                native = args.output / f'native-{bits}-{case}'
                compiled = subprocess.run(['gcc', '-m' + str(bits), '-std=c11',
                    '-O0', '-pthread', '-DNATIVE', f'-DCASE={case}', str(original),
                    '-o', str(native)], capture_output=True, text=True, timeout=30)
                assert compiled.returncode == 0, compiled.stderr
                subprocess.run([str(native)], check=True, timeout=30)
        for case in range(17):
            for mode in ('ai', 'bmc'):
                folder = args.output / f'{bits}-{case}-{mode}'
                folder.mkdir()
                env = dict(os.environ)
                env.pop('IKOS_BMC_RECOVER', None)
                if mode == 'bmc':
                    env['IKOS_BMC_RECOVER'] = '1'
                report = folder / 'report.json'
                command = [str(args.ikos.resolve()), '--analyses=race', '--concurrency=auto',
                    '--demote-race-to-unknown', '-m', str(bits), f'-DCASE={case}',
                    '--format=json', '--report-file=' + str(report),
                    '-o', str(folder / 'output.db'),
                    *(['-DWIDE'] if bits == 64 else []),
                    str(renamed if bits == 64 else original)]
                proc = subprocess.run(command, cwd=folder, env=env, capture_output=True,
                                      text=True, timeout=30)
                (folder / 'run.log').write_text(proc.stdout + proc.stderr)
                verdict = next((line for line in proc.stdout.splitlines()
                                if line.startswith('The program is')), 'NONE')
                assert proc.returncode == 0 and verdict != 'NONE', (folder, proc.stderr)
                parsed = json.loads(report.read_text())
                executions = validate_report(parsed) if mode == 'bmc' else []
                reasons = [(row.get('info') or {}).get('unknown_reason')
                           for row in parsed['reports']]
                safe = case in (0, 4, 12, 13, 15)
                if not args.observe:
                    assert verdict == ('The program is SAFE' if safe else
                                       'The program is potentially UNSAFE'), (folder, verdict)
                    if case == 8:
                        assert 'library memory effects not covered' in reasons, folder
                    elif case == 9:
                        assert 'synthetic pthread handle representation not modeled' in reasons, folder
                    elif case != 16:
                        assert 'library memory effects not covered' not in reasons, folder
                    if verdict == 'The program is definitely UNSAFE':
                        assert executions and all(e['status'] == 'ACCEPT' for e in executions), (folder, executions)
                rows.append(dict(bits=bits, case=case, mode=mode,
                    expected='Safe' if safe else ('Boundary' if case in (8, 9) else 'Race Found'),
                    verdict=verdict, reasons=sorted(set(r for r in reasons if r)),
                    executions=executions))
                print(folder.name, verdict, flush=True)
    (args.output / 'summary.json').write_text(json.dumps(rows, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--observe', action='store_true')
    main(parser.parse_args())
