#!/usr/bin/env python3
"""Check startup heap pointer provenance and independent selected execution results."""
import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile
from validate_bmc_execution import check, digest_json

from ikos_runtime import runtime_command

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--bits', type=int, choices=(32, 64), default=32)
    args = parser.parse_args()
    body = (ROOT / 'analyzer/test/regression/bmc-values/initialized-pointer-fields.c').read_text()
    assertions = 0
    with tempfile.TemporaryDirectory(prefix='ikos-pointer-fields-') as temp:
        work = Path(temp)
        for wide in (False, True):
            for case in range(17):
                source = work / 'case.c'
                report = work / 'report.json'
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') +
                                  body.replace('worker', 'entry').replace('initialize', 'construct'))
                report.unlink(missing_ok=True)
                run = subprocess.run(runtime_command(args.ikos.resolve()) + ['--analyses=race', '--concurrency=auto',
                    '--partitioning=guards', '--demote-race-to-unknown', f'-m{args.bits}', '--format=json',
                    '--report-file=' + str(report), '-o', str(work / 'output.db'), str(source)],
                    env=dict(os.environ, IKOS_BMC_RECOVER='1', IKOS_BMC_DIAGNOSTICS='1'),
                    capture_output=True, text=True, timeout=40)
                assert run.returncode == 0, run.stdout + run.stderr
                rows = json.loads(report.read_text())['reports']
                executions = [r['info']['counterexample'] for r in rows if 'counterexample' in r['info']]
                assert bool(executions) == (case in (0, 11)), (case, wide, rows, run.stdout + run.stderr)
                assertions += 1
                for execution in executions:
                    certificate = copy.deepcopy(execution)
                    program = certificate.pop('program')
                    certificate['program_sha256'] = digest_json(program)
                    checked = check(program, certificate)
                    assert checked['status'] == 'ACCEPT', (case, wide, checked)
                    assertions += 1
                    for trace in certificate['traces']:
                        for step in trace['steps']:
                            if 'encoded_object_pointer' not in step:
                                continue
                            for field, value in [('allocation', 'wrong-allocation'), ('extent', '1'),
                                                 ('owner', 'wrong-thread'), ('occurrence', 2)]:
                                damaged = copy.deepcopy(certificate)
                                target = damaged['traces'][certificate['traces'].index(trace)]['steps'][trace['steps'].index(step)]
                                target['encoded_object_pointer']['object'][field] = value
                                assert check(program, damaged)['status'] == 'REJECT', (field, case, wide)
                                assertions += 1
                            damaged = copy.deepcopy(certificate)
                            target = damaged['traces'][certificate['traces'].index(trace)]['steps'][trace['steps'].index(step)]
                            target['encoded_object_pointer']['offset'] = '1'
                            assert check(program, damaged)['status'] == 'REJECT'
                            assertions += 1
                            damaged = copy.deepcopy(certificate)
                            target = damaged['traces'][certificate['traces'].index(trace)]['steps'][trace['steps'].index(step)]
                            target['read_from'] = ['initial'] * len(target['read_from'])
                            assert check(program, damaged)['status'] == 'REJECT'
                            assertions += 1
                reasons = {}
                for row in rows:
                    for reason, count in row['info'].get('bmc', {}).get('counts', {}).items():
                        reasons[reason] = reasons.get(reason, 0) + count
                print(f'CASE={case}/WIDE={int(wide)} executions={len(executions)} reasons={reasons} PASS', flush=True)
    print(f'{assertions} initialized pointer field assertions passed')


if __name__ == '__main__':
    main()
