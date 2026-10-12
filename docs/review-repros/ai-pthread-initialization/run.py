#!/usr/bin/env python3
"""Check opaque initialization, first ownership, phase and representation limits."""
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
    source = Path(__file__).with_name('init.c').resolve()
    renamed = args.output / 'renamed.c'
    names = dict(worker='update_task', common='protection', data='payload', attr='settings')
    renamed.write_text(re.sub(r'\b(worker|common|data|attr)\b',
                             lambda match: names[match.group(0)], source.read_text()))
    rows = []
    for bits in (32, 64):
        if not args.observe:
            native = args.output / f'native-{bits}'
            p = subprocess.run(['gcc', '-m' + str(bits), '-std=c11', '-D_GNU_SOURCE',
                '-DNATIVE', '-pthread', str(source), '-o', str(native)],
                capture_output=True, text=True, timeout=30)
            assert p.returncode == 0, p.stderr
            subprocess.run([str(native)], check=True, timeout=30)
        for case in range(39):
            for mode in ('ai', 'bmc'):
                folder = args.output / f'{bits}-{case}-{mode}'
                folder.mkdir()
                env = dict(os.environ)
                env.pop('IKOS_BMC_RECOVER', None)
                if mode == 'bmc':
                    env['IKOS_BMC_RECOVER'] = '1'
                report = folder / 'report.json'
                command = [str(args.ikos.resolve()), '--analyses=race', '--concurrency=auto',
                    '--demote-race-to-unknown', '-m', str(bits), '-D_GNU_SOURCE', f'-DCASE={case}',
                    '--format=json', '--report-file=' + str(report), '-o', str(folder / 'output.db'),
                    str(source if bits == 32 else renamed)]
                p = subprocess.run(command, cwd=folder, env=env, capture_output=True,
                                   text=True, timeout=30)
                (folder / 'run.log').write_text(p.stdout + p.stderr)
                verdict = next((s for s in p.stdout.splitlines() if s.startswith('The program is')), 'NONE')
                assert p.returncode == 0 and verdict != 'NONE', (folder, p.stderr)
                parsed = json.loads(report.read_text())
                reasons = sorted({r.get('info', {}).get('unknown_reason')
                    for r in parsed['reports'] if r.get('info', {}).get('unknown_reason')})
                executions = validate_report(parsed) if mode == 'bmc' else []
                if not args.observe:
                    if case in (0, 4, 19, 20, 21, 37, 38):
                        assert verdict == 'The program is SAFE', (folder, verdict, reasons)
                    elif case in (1, 2, 3, 5):
                        assert verdict != 'The program is SAFE', (folder, verdict)
                    else:
                        assert verdict == 'The program is potentially UNSAFE', (folder, verdict)
                        assert 'external call effects not covered' in reasons, (folder, reasons)
                    if verdict == 'The program is definitely UNSAFE':
                        assert executions and all(e['status'] == 'ACCEPT' for e in executions), (folder, executions)
                rows.append(dict(bits=bits, case=case, mode=mode, verdict=verdict,
                    expected='Safe' if case in (0, 4, 19, 20, 21, 37, 38) else
                             'Race Found' if case in (1, 2, 3, 5) else 'Boundary',
                    reasons=reasons, executions=executions))
                print(folder.name, verdict, flush=True)
    (args.output / 'summary.json').write_text(json.dumps(rows, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--observe', action='store_true')
    main(parser.parse_args())
