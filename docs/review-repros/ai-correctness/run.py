#!/usr/bin/env python3
"""Assert repaired AI audit cases; explicitly report any still-pending misses."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'scripts'))
from sweep_bmc_execution import validate_report

MECHANISMS = {
    'distinct-field-locks': 'F1', 'renamed-field-locks': 'F1',
    'transitive-publication': 'F2', 'dynamic-entry': 'F3',
    'atomic-guard-refresh': 'F4', 'join-upper-bound': 'F5',
    'sentinel-writer-union': 'F6',
}


def main(args):
    args.output.mkdir(parents=True, exist_ok=False)
    audit = ROOT / 'docs/artifacts/ai-audit-2026-10-10'
    labels = {r['case']: r['expected'] for r in json.loads((audit / 'ikos-results.json').read_text())}
    rows = []
    for source in sorted((audit / 'cases').glob('*.c')):
        for bits in (32, 64):
            for mode in ('ai', 'bmc'):
                folder = args.output / f'{source.stem}-{bits}-{mode}'
                folder.mkdir()
                env = dict(os.environ)
                env.pop('IKOS_BMC_RECOVER', None)
                if mode == 'bmc':
                    env['IKOS_BMC_RECOVER'] = '1'
                report = folder / 'report.json'
                proc = subprocess.run([
                    str(args.ikos.resolve()), '--analyses=race', '--concurrency=auto',
                    '--demote-race-to-unknown', '-m', str(bits), '--format=json',
                    '--report-file=' + str(report), '-o', str(folder / 'output.db'),
                    str(source.resolve()),
                ], cwd=folder, env=env, capture_output=True, text=True, timeout=30)
                (folder / 'run.log').write_text(proc.stdout + proc.stderr)
                verdict = next((s for s in proc.stdout.splitlines()
                                if s.startswith('The program is')), 'NONE')
                assert proc.returncode == 0 and verdict != 'NONE', (folder, proc.stderr)
                expected = labels[source.stem]
                pending = MECHANISMS.get(source.stem) not in args.fixed and source.stem in MECHANISMS
                if expected == 'Safe':
                    assert verdict != 'The program is definitely UNSAFE', (folder, verdict)
                elif not pending:
                    assert verdict != 'The program is SAFE', (folder, verdict)
                executions = validate_report(json.loads(report.read_text())) if mode == 'bmc' else []
                if mode == 'bmc' and verdict == 'The program is definitely UNSAFE':
                    assert executions and all(e['status'] == 'ACCEPT' for e in executions), (folder, executions)
                status = 'KNOWN_FN_PENDING' if pending and verdict == 'The program is SAFE' else 'CHECKED'
                rows.append(dict(case=source.stem, bits=bits, mode=mode,
                                 expected=expected, verdict=verdict, status=status, executions=executions))
                print(folder.name, verdict, status, flush=True)
    (args.output / 'summary.json').write_text(json.dumps(rows, indent=2) + '\n')
    print('Known misses still pending:', sum(r['status'] == 'KNOWN_FN_PENDING' for r in rows))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--fixed', nargs='+', choices=[f'F{i}' for i in range(1, 7)],
                        default=[f'F{i}' for i in range(1, 7)])
    main(parser.parse_args())
