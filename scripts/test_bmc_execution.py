#!/usr/bin/env python3
"""Use actual analyzer certificates and reject semantic corruptions in both ABIs."""
import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile
from validate_bmc_execution import Frame, Replay, Reject, Thread, UNDEFINED, Unsupported, check, digest_json

ROOT = Path(__file__).resolve().parents[1]


def heap_handle_contract():
    replay = Replay.__new__(Replay)
    replay.program = dict(pointer_bits=32, target_triple='i386-pc-linux-gnu', little_endian=True)
    replay.functions = {'ar.libc.malloc': {}, 'ar.libc.free': {}, 'pthread_create': {}}
    replay.objects = {'global:other': dict(size=4, live=True, variable='other', thread_local=False,
                                           alignment=4, type=dict(kind='integer', bits=32, size=4, extent=4))}
    replay.memory = {'global:other': [0] * 4}
    replay.created, replay.writers, replay.traces = {}, {}, {}
    replay.next_instance = 1
    frame = Frame('main', 'entry', 1)
    thread = Thread(0, [frame], {0: 0})
    invoke = lambda name, args, inst, step: replay.api(name, args, inst, step, 'startup', 0, thread, frame)

    def rejected(action):
        try: action()
        except Reject: return 1
        raise AssertionError('accepted corrupt heap handle execution')

    heap = invoke('ar.libc.malloc', [8], {}, {'statement': 'alloc'})
    other = invoke('ar.libc.malloc', [4], {}, {'statement': 'other_alloc'})
    handle = dict(kind='integer', bits=32, signed=False, size=4, extent=4)
    create = dict(calling_convention=0, argument_layouts=[dict(size=4, alignment=4)],
                  operands=[{}, dict(type=dict(pointee=handle))])
    step = dict(statement='create', encoded_address=dict(
        object=dict(kind='heap', allocation='alloc', owner='main', occurrence=1, extent='8'),
        offset='0', width=4, alignment=4))
    args = [heap, ('null', 0), ('function', 'worker'), ('null', 0)]
    checks = 0
    for field, value in (('extent', '7'), ('allocation', 'other_alloc')):
        corrupt = copy.deepcopy(step)
        corrupt['encoded_address']['object'][field] = value
        checks += rejected(lambda: invoke('pthread_create', args, create, corrupt))
    corrupt = copy.deepcopy(step); corrupt['encoded_address']['alignment'] = 8
    checks += rejected(lambda: invoke('pthread_create', args, create, corrupt))
    checks += rejected(lambda: invoke('pthread_create', args, create, {'statement': 'create'}))
    checks += rejected(lambda: invoke('pthread_create', [(heap[0], 1), *args[1:]], create, step))
    replay.program['target_triple'] = 'x86_64-pc-linux-gnu'
    try: invoke('pthread_create', args, create, step)
    except Unsupported: checks += 1
    else: raise AssertionError('accepted mismatched pthread ABI')
    replay.program['target_triple'] = 'i386-pc-linux-gnu'
    assert invoke('pthread_create', args, create, step) == 0
    checks += 1
    free_step = dict(encoded_address=dict(
        object=dict(kind='heap', allocation='alloc', owner='main', occurrence=1, extent='8'),
        offset='0', width=0))
    for pointer in ((heap[0], 4), ('global:other', 0), ('absolute', 1234)):
        checks += rejected(lambda pointer=pointer: invoke('ar.libc.free', [pointer], {}, free_step))
    checks += rejected(lambda: invoke('ar.libc.free', [heap], {}, {}))
    for field, value in (('allocation', 'other_alloc'), ('occurrence', 2), ('extent', '7')):
        corrupt = copy.deepcopy(free_step)
        corrupt['encoded_address']['object'][field] = value
        checks += rejected(lambda: invoke('ar.libc.free', [heap], {}, corrupt))
    for field, value in (('offset', '4'), ('width', 4)):
        corrupt = copy.deepcopy(free_step)
        corrupt['encoded_address'][field] = value
        checks += rejected(lambda: invoke('ar.libc.free', [heap], {}, corrupt))
    assert invoke('ar.libc.free', [heap], {}, free_step) is UNDEFINED
    assert replay.objects[other[0]]['live']
    checks += 1
    checks += rejected(lambda: invoke('ar.libc.free', [heap], {}, free_step))
    checks += rejected(lambda: replay.region(heap, 4))
    assert invoke('ar.libc.free', [('null', 0)], {}, {}) is UNDEFINED
    checks += 1
    return checks


def heap_handle_executions(ikos, bits):
    fixture = (ROOT / 'analyzer/test/regression/bmc-auxiliary/heap-handles.c').read_text()
    variants = {
        'renamed': fixture.replace('data', 'shared_result').replace('writer', 'entry'),
        'old-prototype': fixture.replace('__VERIFIER_nondet_int(void)', '__VERIFIER_nondet_int()'),
        'wide': fixture.replace('int data;', 'long long data;'),
        'helper': fixture.replace('void *writer(void *arg) {',
            'void update(void) { data = 7; }\nvoid *writer(void *arg) {').replace('  data = 7;', '  update();'),
    }
    checks = 0
    with tempfile.TemporaryDirectory(prefix='ikos-heap-handles-') as temp:
        work = Path(temp)
        for variant, text in variants.items():
            for case in range(14):
                source, report = work / 'renamed.c', work / 'report.json'
                source.write_text('#define CASE ' + str(case) + '\n' + text)
                report.unlink(missing_ok=True)
                proc = subprocess.run([str(ikos.resolve()), '--analyses=race', '--concurrency=auto',
                    '--demote-race-to-unknown', '-m', bits, '--format=json',
                    '--report-file=' + str(report), '-o', str(work / 'output.db'), str(source)],
                    cwd=work, env=dict(os.environ, IKOS_BMC_RECOVER='1'),
                    capture_output=True, text=True, timeout=30)
                assert proc.returncode == 0, proc.stdout + proc.stderr
                data = json.loads(report.read_text())
                executions = [r['info']['counterexample'] for r in data['reports']
                              if 'counterexample' in r['info']]
                # 3..12 are search/semantic boundaries; 13 is a common-lock safe control.
                assert bool(executions) == (case in (0, 1, 2)), (bits, variant, case, proc.stdout + proc.stderr)
                checks += 1
                for execution in executions:
                    certificate = copy.deepcopy(execution)
                    program = certificate.pop('program')
                    certificate['program_sha256'] = digest_json(program)
                    assert check(program, certificate)['status'] == 'ACCEPT', (bits, variant, case)
                    checks += 1

                    def corrupt(step_index, field, value):
                        nonlocal checks
                        trace_index, index = step_index
                        changed = copy.deepcopy(certificate)
                        step = changed['traces'][trace_index]['steps'][index]
                        if field == 'extent': step['encoded_address']['object'][field] = value
                        elif field == 'offset': step['encoded_address'][field] = value
                        else: step[field] = value
                        result = check(program, changed)
                        assert result['status'] == 'REJECT', (bits, variant, case, field, result)
                        checks += 1

                    for ti, trace in enumerate(certificate['traces']):
                        for si, step in enumerate(trace['steps']):
                            address = step.get('encoded_address', {})
                            if address.get('object', {}).get('kind') == 'heap':
                                corrupt((ti, si), 'extent', str(int(address['object']['extent']) + 1))
                                corrupt((ti, si), 'offset', str(int(address['offset']) + 1))
                            if case != 0 and 'encoded_result' in step and program['instructions'][step['statement']]['kind'] == 'call':
                                corrupt((ti, si), 'encoded_result', str(int(step['encoded_result']) + 1))
                print(f'{bits}-bit heap handle {variant}/{case}: execution checks [PASS]', flush=True)
    return checks


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
    checks = 1 + heap_handle_contract()
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
    checks += heap_handle_executions(args.ikos, args.bits)
    print(f'{checks} execution assertions passed')


if __name__ == '__main__':
    main()
