#!/usr/bin/env python3
"""Check live main-frame mutex fields and independently replay their execution."""
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
from validate_bmc_execution import Replay, Reject, Unsupported, digest_json


def replay_mutations(certificate):
    program = certificate['program']
    certificate = dict(certificate, program_sha256=digest_json(program))
    allocation = next(k for k, i in program['instructions'].items()
                      if i['kind'] == 'allocate' and 'native_layout' in i)

    def rejected(mutate):
        snapshot = copy.deepcopy(program)
        mutate(snapshot)
        evidence = dict(certificate, program=snapshot, program_sha256=digest_json(snapshot))
        try:
            Replay(snapshot, evidence).run()
        except (Reject, Unsupported):
            return
        raise AssertionError('invalid stack mutex premises accepted')

    rejected(lambda p: p['instructions'][allocation].pop('native_layout'))
    rejected(lambda p: p['instructions'][allocation].update(native_alignment=1))
    rejected(lambda p: p['instructions'][allocation].update(native_owner='worker'))
    rejected(lambda p: p['instructions'][allocation].update(native_address_space=1))
    incompatible_triple = ('x86_64-unknown-linux-gnu' if program['pointer_bits'] == 32
                           else 'i386-unknown-linux-gnu')
    rejected(lambda p: p.update(target_triple=incompatible_triple))
    rejected(lambda p: p['instructions'][allocation]['native_layout'].update(extent=1))

    rejected(lambda p: p.update(target_triple='x86_64-unknown-linux-gnux32'))
    if program['pointer_bits'] == 64:
        rejected(lambda p: p['instructions'][allocation].update(native_alignment=4))

    def corrupt_leaf(p, field, value):
        def walk(node):
            if node.get('name') == 'union.pthread_mutex_t':
                node[field] = value
            for entry in node.get('fields', []):
                walk(entry['type'])
            if 'element' in node:
                walk(node['element'])
        walk(p['instructions'][allocation]['native_layout'])
    opposite_extent = 40 if program['pointer_bits'] == 32 else 24
    opposite_alignment = 8 if program['pointer_bits'] == 32 else 4
    rejected(lambda p: corrupt_leaf(p, 'extent', opposite_extent))
    rejected(lambda p: corrupt_leaf(p, 'alignment', opposite_alignment))

    def remove_name(p):
        def walk(node):
            if node.get('name') == 'union.pthread_mutex_t': node['name'] = 'struct.equal_size'
            for field in node.get('fields', []): walk(field['type'])
            if 'element' in node: walk(node['element'])
        walk(p['instructions'][allocation]['native_layout'])
    rejected(remove_name)
    replay = Replay(program, certificate)
    replay.run()
    pointer = next(p for p in replay.locks if p[0].startswith('stack:'))
    obj = replay.objects[pointer[0]]
    for key, value in [('owner_frame', -1), ('owner_thread', 1), ('count', 2), ('live', False)]:
        previous = obj[key]; obj[key] = value
        try:
            replay.stack_mutex(pointer)
        except (Reject, Unsupported):
            pass
        else:
            raise AssertionError(key)
        obj[key] = previous
    state = replay.locks.pop(pointer)
    try:
        replay.stack_mutex(pointer)
    except (Reject, Unsupported):
        pass
    else:
        raise AssertionError('missing actual initialization')
    replay.locks[pointer] = state
    try:
        replay.region(pointer, 1)
    except Reject:
        pass
    else:
        raise AssertionError('ordinary byte access to mutex accepted')
    mutex_extent = next(width for start, width in obj['mutex_regions']
                        if start == pointer[1])
    replay.region((pointer[0], pointer[1] + mutex_extent), 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--bits', type=int, choices=(32, 64), default=32)
    args = parser.parse_args()
    body = (ROOT / 'analyzer/test/regression/bmc-values/main-stack-mutex.c').read_text()
    positive = {0, 10, 11, 12}
    cases = range(16)
    with tempfile.TemporaryDirectory(prefix='ikos-main-stack-mutex-') as tmp:
        work = Path(tmp)
        for wide in (False, True):
            for case in cases:
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') +
                                  body.replace('worker', 'entry').replace('object', 'item'))
                report = work / 'report.json'; report.unlink(missing_ok=True)
                proc = subprocess.run(runtime_command(args.ikos.resolve()) + ['--analyses=race',
                    '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                    '--bmc-deadline-ms=20000', '--bmc-solver-timeout-ms=5000', f'-m{args.bits}',
                    '--format=json', '--report-file=' + str(report), '-o', str(work / 'output.db'), str(source)],
                    env=dict(os.environ, IKOS_BMC_RECOVER='1'), capture_output=True, text=True, timeout=35)
                output = proc.stdout + proc.stderr
                assert proc.returncode == 0, (case, wide, output)
                verdict = classify(output, proc.returncode, False)
                assert (verdict == 'Race Found') == (case in positive), (case, wide, verdict, output)
                if case in positive:
                    data = json.loads(report.read_text()); checks = validate_report(data)
                    assert checks and all(c['status'] == 'ACCEPT' and c['strict_false_publishable'] for c in checks), checks
                    if case == 0:
                        certificate = next(r['info']['counterexample'] for r in data['reports']
                                           if 'counterexample' in r['info'])
                        replay_mutations(certificate)
                print(f'CASE={case}/WIDE={int(wide)} {verdict} PASS', flush=True)


if __name__ == '__main__':
    main()
