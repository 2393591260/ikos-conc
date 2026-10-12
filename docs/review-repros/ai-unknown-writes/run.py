#!/usr/bin/env python3
"""Unknown destinations cannot retain a SAFE claim from stale memory."""
import argparse
import json
import os
import re
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'scripts'))
from sweep_bmc_execution import validate_report


def main(args):
    args.output.mkdir(parents=True, exist_ok=False)
    source = Path(__file__).with_name('writes.c').resolve()
    renamed = args.output / 'renamed.c'
    names = dict(selected='choice', left='one', right='two', first='writer_one',
                 second='writer_two', destination='target', enabled='guard_value')
    renamed.write_text(re.sub(r'\b(' + '|'.join(names) + r')\b',
                             lambda m: names[m.group(0)], source.read_text()))
    rows, prefixes = [], []
    for bits in (32, 64):
        for case in range(10):
            input_file = source if bits == 32 else renamed
            native = args.output / f'prefix-{bits}-{case}'
            build = subprocess.run(['gcc', '-m' + str(bits), '-pthread', '-DNATIVE_PREFIX',
                f'-DCASE={case}', str(input_file), '-o', str(native)],
                capture_output=True, text=True, timeout=30)
            assert build.returncode == 0, build.stderr
            check = subprocess.run([str(native)], capture_output=True, text=True, timeout=5)
            assert check.returncode == 0, (native, check.stderr)
            prefixes.append(dict(bits=bits, case=case, exit=check.returncode))
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
                    '--format=json', '--report-file=' + str(report), '-o', str(folder / 'output.db'),
                    str(input_file)]
                p = subprocess.run(command, cwd=folder, env=env, capture_output=True,
                                   text=True, timeout=30)
                (folder / 'run.log').write_text(p.stdout + p.stderr)
                verdict = next((s for s in p.stdout.splitlines()
                                if s.startswith('The program is')), 'NONE')
                assert p.returncode == 0 and verdict != 'NONE', (folder, p.stderr)
                parsed = json.loads(report.read_text())
                reasons = sorted({r.get('info', {}).get('unknown_reason')
                    for r in parsed['reports'] if r.get('info', {}).get('unknown_reason')})
                executions = validate_report(parsed) if mode == 'bmc' else []
                if not args.observe:
                    if case == 2:
                        assert verdict == 'The program is SAFE', (folder, verdict)
                    else:
                        assert verdict != 'The program is SAFE', (folder, verdict)
                    if case not in (1, 2):
                        assert 'unknown destination write effects not covered' in reasons, (folder, reasons)
                    if verdict == 'The program is definitely UNSAFE':
                        assert executions and all(e['status'] == 'ACCEPT' for e in executions), (folder, executions)
                rows.append(dict(bits=bits, case=case, mode=mode, verdict=verdict,
                    expected='Safe' if case == 2 else 'Race Found',
                    reasons=reasons, executions=executions))
                print(folder.name, verdict, flush=True)
    (args.output / 'summary.json').write_text(json.dumps(
        dict(native_prefixes=prefixes, cases=rows), indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--observe', action='store_true')
    main(parser.parse_args())
