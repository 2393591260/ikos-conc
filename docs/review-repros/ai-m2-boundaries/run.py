#!/usr/bin/env python3
"""Record array-value and universal-join boundaries in both machine models."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'scripts'))
from sweep_bmc_execution import validate_report


def main(args):
    args.output.mkdir(parents=True, exist_ok=False)
    rows = []
    for name, count in (('array', 10), ('join', 6)):
        source = Path(__file__).with_name(name + '.c').resolve()
        renamed = args.output / (name + '-renamed.c')
        names = dict(worker='job', slots='table', handles='tokens', data='payload',
                     lock='guard', require_index='check_bound')
        renamed.write_text(re.sub(r'\b(' + '|'.join(names) + r')\b',
            lambda m: names[m.group(0)], source.read_text()))
        for bits in (32, 64):
            for case in range(count):
                for mode in ('ai', 'bmc'):
                    folder = args.output / f'{name}-{bits}-{case}-{mode}'
                    folder.mkdir()
                    env = dict(os.environ)
                    env.pop('IKOS_BMC_RECOVER', None)
                    env.pop('IKOS_ARRAY_PROBE', None)
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
                    parsed = json.loads(report.read_text())
                    reasons = sorted({r.get('info', {}).get('unknown_reason') for r in parsed['reports']
                                      if r.get('info', {}).get('unknown_reason')})
                    executions = validate_report(parsed) if mode == 'bmc' else []
                    expected = 'Safe' if (name == 'array' and case in (0, 2, 4, 6, 7, 8)) or (
                        name == 'join' and case in (0, 1, 2)) else 'Race Found'
                    if expected == 'Safe':
                        assert verdict != 'The program is definitely UNSAFE', (folder, verdict)
                    else:
                        assert verdict != 'The program is SAFE', (folder, verdict)
                    if name == 'join' and case == 1:
                        assert verdict == 'The program is SAFE', (folder, verdict)
                    if not args.baseline and name == 'array' and case in (0, 7):
                        assert verdict == 'The program is SAFE', (folder, verdict)
                    if verdict == 'The program is definitely UNSAFE':
                        assert executions and all(e['status'] == 'ACCEPT' for e in executions), (
                            folder, executions)
                    rows.append(dict(suite=name, bits=bits, case=case, mode=mode,
                        expected=expected, verdict=verdict, reasons=reasons, executions=executions,
                        report_sha256=hashlib.sha256(report.read_bytes()).hexdigest(), command=command))
                    print(folder.name, verdict, flush=True)
    (args.output / 'summary.json').write_text(json.dumps(rows, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--baseline', action='store_true',
                        help='Observe the older analyzer without requiring array recovery')
    main(parser.parse_args())
