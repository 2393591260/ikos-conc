#!/usr/bin/env python3
"""Confirm that selected evidence and outcomes survive diagnostic-off runs."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

from ikos_runtime import runtime_command
from sweep_bmc_execution import validate_report

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    args = parser.parse_args()
    source = ROOT / 'analyzer/test/regression/bmc-values/values.c'
    accepted_executions = 0
    with tempfile.TemporaryDirectory(prefix='ikos-results-') as temp:
        work = Path(temp)
        for bits in ('32', '64'):
            for case in (1, 11, 26, 31, 39):
                evidence = []
                for diagnostic in (False, True):
                    env = dict(os.environ, IKOS_BMC_RECOVER='1')
                    env.pop('IKOS_BMC_DIAGNOSTICS', None)
                    if diagnostic:
                        env['IKOS_BMC_DIAGNOSTICS'] = '1'
                    report = work / 'report.json'
                    report.unlink(missing_ok=True)
                    proc = subprocess.run(runtime_command(args.ikos.resolve()) + ['--analyses=race',
                        '--concurrency=auto', '--demote-race-to-unknown', '-m', bits,
                        '-DCASE=' + str(case), '--format=json', '--report-file=' + str(report),
                        '-o', str(work / 'output.db'), str(source)], cwd=work, env=env,
                        capture_output=True, text=True, timeout=30)
                    assert proc.returncode == 0, proc.stdout + proc.stderr
                    data = json.loads(report.read_text())
                    infos = [r['info'] for r in data['reports'] if 'bmc_result' in r['info']]
                    confirmed = [i for i in infos if i['bmc_result']['status'] == 'CONFIRMED']
                    assert bool(confirmed) == (case in (1, 11, 31)), (bits, case, infos)
                    executions = validate_report(data)
                    if confirmed:
                        assert len(executions) == len(confirmed) and all(
                            row['status'] == 'ACCEPT' and row['strict_false_publishable']
                            for row in executions), (bits, case, diagnostic, executions)
                        accepted_executions += len(executions)
                    else:
                        assert not executions, (bits, case, diagnostic, executions)
                    for info in infos:
                        assert ('counterexample' in info) == (info in confirmed)
                        if info in confirmed:
                            assert info['bmc_result']['validation_status'] == 'UNVALIDATED'
                            trace = info['counterexample']
                            assert len(trace['targets']) == 2 and trace['program']['instructions']
                            assert {'a', 'b'} <= {t['id'] for t in trace['traces']}
                        if not diagnostic:
                            assert 'bmc' not in info
                    selected = [i['counterexample'] for i in confirmed]
                    for counterexample in selected:
                        for instruction in counterexample['program']['instructions'].values():
                            instruction.pop('ar', None)  # Presentation contains ASLR pointer names.
                    evidence.append(selected)
                assert evidence[0] == evidence[1], (bits, case, 'diagnostics changed selected model')
                print(f'{bits}-bit case {case}: diagnostic-off/on evidence [PASS]')
    assert accepted_executions > 0
    print(f'PASS {accepted_executions} nonempty strict AR executions ACCEPT', flush=True)


if __name__ == '__main__':
    main()
