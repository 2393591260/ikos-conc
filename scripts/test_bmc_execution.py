#!/usr/bin/env python3
"""Use actual analyzer certificates and reject semantic corruptions in both ABIs."""
import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile
from validate_bmc_execution import Replay, Reject, check, digest_json

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bits', choices=("32", "64"), default="32")
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    args = parser.parse_args()
    # Test invalid library histories even though BMC now refuses to export them.
    replay = Replay.__new__(Replay)
    replay.functions = {'pthread_join': {}, 'pthread_detach': {}}
    replay.created = {1: dict(joined=False, detached=False)}
    invoke = lambda name, values: replay.api(name, values, {}, {}, 'startup', 0, None, None)
    assert invoke('pthread_detach', [1]) == 0
    checks = 1
    for name, values in (('pthread_detach', [1]), ('pthread_join', [1, ('null', 0)])):
        try:
            invoke(name, values)
        except Reject:
            checks += 1
        else:
            raise AssertionError('accepted operation on a detached instance')
    replay.created[1] = dict(joined=True, detached=False)
    for name, values in (('pthread_detach', [1]), ('pthread_join', [1, ('null', 0)])):
        try:
            invoke(name, values)
        except Reject:
            checks += 1
        else:
            raise AssertionError('accepted operation on a joined instance')
    with tempfile.TemporaryDirectory(prefix='ikos-replay-') as temp:
        work = Path(temp)
        for bits in (args.bits,):
            cases = (1, 11, 21, 25, 31, 34, 36, 38, 48, 49, 52, 53, 54,
                     56, 57, 58, 61, 65, 63 if bits == '32' else 62)
            for case in cases:
                report = work / 'report.json'
                report.unlink(missing_ok=True)
                proc = subprocess.run([str(args.ikos.resolve()), '--analyses=race', '--concurrency=auto',
                    '--demote-race-to-unknown', '-m', bits, '-DCASE=' + str(case), '--format=json',
                    '--report-file=' + str(report), '-o', str(work / 'output.db'),
                    str(ROOT / 'analyzer/test/regression/bmc-values/values.c')], cwd=work,
                    env=dict(os.environ, IKOS_BMC_RECOVER='1'), capture_output=True, text=True, timeout=30)
                assert proc.returncode == 0, proc.stdout + proc.stderr
                executions = [r['info']['counterexample'] for r in json.loads(report.read_text())['reports']
                              if 'counterexample' in r['info']]
                assert executions, (bits, case, proc.stdout + proc.stderr)
                for execution in executions:
                    certificate = copy.deepcopy(execution)
                    program = certificate.pop('program')
                    certificate['program_sha256'] = digest_json(program)
                    result = check(program, certificate)
                    assert result['status'] == 'ACCEPT', (bits, case, result)
                    checks += 1
                    traces = {t['id']: t for t in certificate['traces']}

                    def rejected(mutated):
                        nonlocal checks
                        result = check(program, mutated)
                        assert result['status'] == 'REJECT', (bits, case, result)
                        checks += 1

                    c = copy.deepcopy(certificate); c['program_sha256'] = 'broken'; rejected(c)
                    c = copy.deepcopy(certificate); c['targets'][1] = c['targets'][0]; rejected(c)
                    child = next(t for t in certificate['traces'] if 'creator_step' in t)
                    c = copy.deepcopy(certificate)
                    next(t for t in c['traces'] if t['id'] == child['id'])['creator_step'] = -1
                    rejected(c)
                    target = certificate['targets'][0]
                    for field in ('offset', 'width'):
                        c = copy.deepcopy(certificate)
                        step = next(t for t in c['traces'] if t['id'] == target['trace'])['steps'][target['step']]
                        step['encoded_address'][field] = str(int(step['encoded_address'][field]) + 1)
                        rejected(c)
                    for name, trace in traces.items():
                        for index, step in enumerate(trace['steps']):
                            if 'encoded_result' in step and program['instructions'][step['statement']]['kind'] != 'call':
                                c = copy.deepcopy(certificate)
                                changed = next(t for t in c['traces'] if t['id'] == name)['steps'][index]
                                changed['encoded_result'] = str(int(changed['encoded_result']) + 1)
                                rejected(c)
                                break
                        else:
                            continue
                        break
                    for name, trace in traces.items():
                        for index, step in enumerate(trace['steps']):
                            if 'read_from' in step:
                                c = copy.deepcopy(certificate)
                                changed = next(t for t in c['traces'] if t['id'] == name)['steps'][index]
                                changed['read_from'][0] = dict(trace='missing-instance', step=0)
                                rejected(c)
                                break
                        else:
                            continue
                        break
                    c = copy.deepcopy(certificate)
                    c['traces'].append(copy.deepcopy(c['traces'][0])); rejected(c)
                    c = copy.deepcopy(certificate)
                    c['traces'][0]['steps'][0]['synthetic_event'] = 'atomic_begin'; rejected(c)
                print(f'{bits}-bit value/instance/sync case {case}: replay and corruption checks [PASS]', flush=True)
    print(f'{checks} execution assertions passed')


if __name__ == '__main__':
    main()
