#!/usr/bin/env python3
"""Check first-publication ordering without treating late initialization as private."""
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
    source = Path(__file__).with_name('publication.c').resolve()
    renamed = args.output / 'renamed.c'
    names = dict(worker='insert_task', protection='guard', root='container',
                 initialize='set_fields', value='datum', next='link')
    renamed.write_text(re.sub(r'\b(' + '|'.join(names) + r')\b',
                             lambda m: names[m.group(0)], source.read_text()))
    rows = []
    for bits in (32, 64):
        for case in range(11):
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
                    str(source if bits == 32 else renamed)]
                p = subprocess.run(command, cwd=folder, env=env, capture_output=True,
                                   text=True, timeout=30)
                (folder / 'run.log').write_text(p.stdout + p.stderr)
                verdict = next((s for s in p.stdout.splitlines()
                                if s.startswith('The program is')), 'NONE')
                assert p.returncode == 0 and verdict != 'NONE', (folder, p.stderr)
                expected = 'Safe' if case in (0, 5, 8, 9) else 'Boundary' if case == 4 else 'Race Found'
                parsed = json.loads(report.read_text())
                executions = validate_report(parsed) if mode == 'bmc' else []
                if not args.observe:
                    if expected == 'Safe':
                        assert verdict != 'The program is definitely UNSAFE', (folder, verdict)
                    else:
                        assert verdict != 'The program is SAFE', (folder, verdict)
                    if case == 0:
                        assert verdict == 'The program is SAFE', (folder, verdict)
                    if verdict == 'The program is definitely UNSAFE':
                        assert executions and all(e['status'] == 'ACCEPT' for e in executions), (folder, executions)
                rows.append(dict(bits=bits, case=case, mode=mode, expected=expected,
                                 verdict=verdict, executions=executions))
                print(folder.name, verdict, flush=True)
    (args.output / 'summary.json').write_text(json.dumps(rows, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--observe', action='store_true')
    main(parser.parse_args())
