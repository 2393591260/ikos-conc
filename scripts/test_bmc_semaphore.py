#!/usr/bin/env python3
"""Check concrete semaphore credits and reject missing synchronization premises."""
import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from ikos_runtime import runtime_command

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT), str(ROOT / 'scripts')]
from svcomp_race_test import classify
from sweep_bmc_execution import validate_report
from validate_bmc_execution import check, digest_json, Replay, Thread, Unsupported, Reject


def check_providers(program, certificate):
    replay = Replay(program, certificate)
    def call(name):
        return next(inst for inst in program['instructions'].values()
                    if inst['kind'] == 'call' and inst['operands'][0].get('id') == name)
    name = next(name for name, obj in program['globals'].items()
                if obj.get('frontend_type_name') == 'union.sem_t')
    pointer = ('global:' + name, 0)
    main = Thread(0, [], {0: 1})
    replay.api('sem_init', [pointer, 0, 1], call('sem_init'), {}, 'startup', 0, main, None)
    main.vc = {0: 8}
    replay.api('sem_post', [pointer], call('sem_post'), {}, 'startup', 1, main, None)
    provider = Thread(1, [], {0: 2, 1: 5})
    replay.api('sem_post', [pointer], call('sem_post'), {}, 'a', 0, provider, None)
    consumer = Thread(2, [], {0: 2, 2: 1})
    replay.api('sem_wait', [pointer], call('sem_wait'), {}, 'b', 0, consumer, None)
    # An old initial permit cannot hide either preceding provider's clock.
    assert consumer.vc == {0: 8, 1: 5, 2: 1}, consumer.vc
    assert replay.semaphores[pointer[0]]['count'] == 2
    try:
        replay.region(pointer, 1)
    except Unsupported:
        pass
    else:
        raise AssertionError('raw semaphore storage access accepted')
    empty = Replay(program, certificate)
    empty.api('sem_init', [pointer, 0, 1], call('sem_init'), {}, 'startup', 0, main, None)
    empty.api('sem_wait', [pointer], call('sem_wait'), {}, 'a', 0, provider, None)
    try:
        empty.api('sem_wait', [pointer], call('sem_wait'), {}, 'b', 0, consumer, None)
    except Reject:
        pass
    else:
        raise AssertionError('second wait consumed a nonexistent permit')
    for initial in (0, 2, 3, 2147483647):
        counted = Replay(program, certificate)
        counted.api('sem_init', [pointer, 0, initial], call('sem_init'), {}, 'startup', 0, main, None)
        assert counted.semaphores[pointer[0]]['count'] == initial
        if initial:
            counted.api('sem_wait', [pointer], call('sem_wait'), {}, 'a', 0, provider, None)
            assert counted.semaphores[pointer[0]]['count'] == initial - 1
        if initial == 2147483647:
            counted.api('sem_post', [pointer], call('sem_post'), {}, 'startup', 1, main, None)
            try:
                counted.api('sem_post', [pointer], call('sem_post'), {}, 'startup', 2, main, None)
            except Reject:
                pass
            else:
                raise AssertionError('semaphore overflow accepted')


def check_damage(program, certificate):
    def named(step, name):
        inst = program['instructions'][step['statement']]
        return inst['kind'] == 'call' and inst['operands'][0].get('id') == name
    for operation in ('post', 'wait', 'init', 'creator', 'layout',
                      'native-missing', 'native-declaration', 'native-cc',
                      'native-signature', 'native-bundle', 'native-attribute',
                      'native-width', 'native-return', 'native-address-space',
                      'global-extent', 'global-store-size', 'global-type-alignment',
                      'global-alignment', 'global-address-space', 'call-cc',
                      'endian', 'target', 'native-pointer-bits', 'native-endian',
                      'native-target', 'ar-store-size'):
        damaged, model = copy.deepcopy(certificate), copy.deepcopy(program)
        startup = next(t for t in damaged['traces'] if t['id'] == 'startup')
        if operation == 'post':
            startup['steps'].pop(next(i for i, step in enumerate(startup['steps']) if named(step, 'sem_post')))
        elif operation == 'init':
            pos = next(i for i, step in enumerate(startup['steps']) if named(step, 'sem_init'))
            startup['steps'].insert(pos, copy.deepcopy(startup['steps'][pos]))
        elif operation == 'wait':
            worker = next(t for t in damaged['traces'] if t['id'] == 'a')
            worker['steps'].pop(next(i for i, step in enumerate(worker['steps']) if named(step, 'sem_wait')))
        elif operation == 'creator':
            next(t for t in damaged['traces'] if t['id'] == 'b')['creator_step'] = 0
        elif operation in ('endian', 'target'):
            model['little_endian' if operation == 'endian' else 'target_triple'] = (
                False if operation == 'endian' else 'x86_64-unknown-linux-musl')
        elif operation == 'ar-store-size':
            next(g for g in model['globals'].values()
                 if g.get('frontend_type_name') == 'union.sem_t')['type']['size'] = '1'
        elif operation.startswith('global-'):
            obj = next(g for g in model['globals'].values()
                       if g.get('frontend_type_name') == 'union.sem_t')
            field = operation[len('global-'):].replace('-', '_')
            obj['native_semaphore'][field] = 1
        else:
            for inst in model['instructions'].values():
                if inst['kind'] != 'call' or inst['operands'][0].get('id') != 'sem_wait':
                    continue
                native = inst['native_semaphore']
                if operation == 'layout':
                    inst['argument_layouts'][0]['size'] = 15
                elif operation == 'native-missing':
                    del inst['native_semaphore']
                elif operation == 'call-cc':
                    inst['calling_convention'] = 8
                else:
                    key, value = {
                        'native-pointer-bits': ('pointer_bits', 16),
                        'native-endian': ('little_endian', False),
                        'native-target': ('target_triple', 'x86_64-unknown-linux-musl'),
                        'native-declaration': ('declaration', False),
                        'native-cc': ('calling_convention', 8),
                        'native-signature': ('matching_signature', False),
                        'native-bundle': ('operand_bundles', True),
                        'native-attribute': ('special_parameter_abi', True),
                        'native-width': ('parameter_bits', [64]),
                        'native-return': ('return_bits', 64),
                        'native-address-space': ('parameter_address_space', 1),
                    }[operation]
                    native[key] = value
        damaged['program_sha256'] = digest_json(model)
        result = check(model, damaged)
        assert result['status'] != 'ACCEPT', (operation, result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--bits', type=int, choices=(32, 64), default=32)
    args = parser.parse_args()
    body = (ROOT / 'analyzer/test/regression/bmc-values/semaphore-prefix.c').read_text()
    with tempfile.TemporaryDirectory(prefix='ikos-semaphore-') as tmp:
        work = Path(tmp)
        for wide in (False, True):
            for case in range(21):
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') +
                                  body.replace('permit', 'credit').replace('worker', 'entry'))
                report = work / 'report.json'
                report.unlink(missing_ok=True)
                proc = subprocess.run(runtime_command(args.ikos.resolve()) + ['--analyses=race',
                    '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                    '--bmc-deadline-ms=20000', '--bmc-solver-timeout-ms=1000', '-m' + str(args.bits),
                    '--format=json', '--report-file=' + str(report), '-o', str(work/'out.db'), str(source)],
                    env=dict(os.environ, IKOS_BMC_RECOVER='1'), capture_output=True, text=True, timeout=35)
                verdict = classify(proc.stdout + proc.stderr, proc.returncode, False)
                assert proc.returncode == 0, (case, wide, proc.stdout + proc.stderr)
                assert (verdict == 'Race Found') == (case in (0, 7, 11, 14, 16)), (case, wide, verdict, proc.stdout + proc.stderr)
                if case in (11, 14, 16, 17, 18, 20):
                    assert verdict != 'Safe', (case, wide, verdict)
                if case == 7:
                    data = json.loads(report.read_text())
                    groups = [(r['statement_id'], r['info']['memory_location']) for r in data['reports']
                              if 'access_a' in r['info']]
                    assert len(groups) == len(set(groups)), 'race group emitted twice'
                    command = [arg.replace('--bmc-deadline-ms=20000', '--bmc-deadline-ms=0')
                               for arg in proc.args]
                    probe = subprocess.run(command, capture_output=True, text=True, timeout=35,
                                           env=dict(os.environ, IKOS_BMC_RECOVER='1', IKOS_BMC_DIAGNOSTICS='1'))
                    assert probe.returncode == 0, probe.stdout + probe.stderr
                    assert classify(probe.stdout + probe.stderr, probe.returncode, False) != 'Race Found'
                if case == 0:
                    data = json.loads(report.read_text())
                    checks = validate_report(data)
                    assert checks and all(c['status'] == 'ACCEPT' for c in checks), checks
                    certificates = 0
                    for row in data['reports']:
                        cert = row['info'].get('counterexample')
                        if not cert:
                            continue
                        cert = copy.deepcopy(cert)
                        program = cert.pop('program')
                        cert['program_sha256'] = digest_json(program)
                        assert check(program, cert)['status'] == 'ACCEPT'
                        check_damage(program, cert)
                        check_providers(program, cert)
                        certificates += 1
                    assert certificates > 0, 'missing exported execution'
                print(f'CASE={case}/WIDE={int(wide)} {verdict} PASS', flush=True)


if __name__ == '__main__':
    main()
