#!/usr/bin/env python3
"""Stable publication never removes alternate writers, escapes or early spawns."""

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
    renamed = args.output / 'renamed.c'
    renamed.write_text(Path(__file__).with_name('publication.c').read_text()
                       .replace('published', 'root_slot').replace('worker', 'callback')
                       .replace('member', 'payload'))
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
                source = renamed if bits == 64 else Path(__file__).with_name('publication.c').resolve()
                command = [str(args.ikos.resolve()), '--analyses=race', '--concurrency=auto',
                           '--demote-race-to-unknown', '-m', str(bits), f'-DCASE={case}',
                           '--format=json', '--report-file=' + str(folder / 'report.json'),
                           '-o', str(folder / 'output.db')]
                if bits == 64:
                    command.append('-DWIDE')
                command.append(str(source))
                proc = subprocess.run(command, cwd=folder, env=env, capture_output=True,
                                      text=True, timeout=30)
                (folder / 'run.log').write_text(proc.stdout + proc.stderr)
                verdict = next((line for line in proc.stdout.splitlines()
                                if line.startswith('The program is')), 'NONE')
                assert proc.returncode == 0 and verdict != 'NONE', (folder, proc.stderr)
                safe = case in (0, 4, 5, 8, 9)
                executions = validate_report(json.loads((folder / 'report.json').read_text())) if mode == 'bmc' else []
                if not args.observe:
                    assert verdict != ('The program is definitely UNSAFE' if safe else 'The program is SAFE'), (folder, verdict)
                    if case == 0:
                        assert verdict == 'The program is SAFE', (folder, verdict)
                    if verdict == 'The program is definitely UNSAFE':
                        assert executions, folder
                        # The baseline already confirms the pointer-byte control;
                        # the independent checker explicitly lacks this load representation.
                        assert all(e['status'] == 'ACCEPT' or
                                   (case == 6 and e['status'] == 'UNSUPPORTED' and
                                    e.get('reason') == 'load representation/type')
                                   for e in executions), (folder, executions)
                rows.append(dict(bits=bits, case=case, mode=mode,
                                 expected='Safe' if safe else ('Boundary' if case in (7, 10) else 'Race Found'),
                                 verdict=verdict, executions=executions))
                print(folder.name, verdict, flush=True)
    (args.output / 'summary.json').write_text(json.dumps(rows, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--observe', action='store_true')
    main(parser.parse_args())
