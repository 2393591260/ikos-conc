#!/usr/bin/env python3
"""Check concrete entry inputs and path-sized native thread-handle storage."""
import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT), str(ROOT / 'scripts')]
from ikos_runtime import runtime_command, runtime_identity, verify_runtime
from svcomp_race_test import classify
from sweep_bmc_execution import validate_report
from validate_bmc_execution import Replay, Reject, Unsupported, digest_json

POSITIVE = {0, 1, 2, 3, 4, 18}


def replay_mutations(certificate):
    original = certificate['program']

    def rejected(change):
        program = copy.deepcopy(original)
        change(program)
        modified = dict(certificate, program_sha256=digest_json(program))
        try:
            Replay(program, modified).run()
        except (Reject, Unsupported):
            return
        raise AssertionError('invalid dynamic stack premise accepted')

    save = next(k for k, i in original['instructions'].items()
                if i.get('native_intrinsic') == 'llvm.stacksave')
    allocation = next(k for k, i in original['instructions'].items()
                      if i['kind'] == 'allocate' and i['operands'][0]['kind'] != 'integer')
    rejected(lambda p: p['instructions'][save].pop('native_intrinsic'))
    rejected(lambda p: p['instructions'][allocation]['operands'].__setitem__(
        0, dict(kind='integer', value='1', type=original['instructions'][allocation]['operands'][0]['type'])))
    rejected(lambda p: p['instructions'][allocation]['allocated_type'].update(extent=1))
    replay = Replay(original, dict(certificate, program_sha256=digest_json(original)))
    replay.run()
    obj = next(k for k, meta in replay.objects.items() if meta.get('dynamic_stack'))
    pointer = (obj, 0)
    bits = original['pointer_bits']
    stride = int(replay.objects[obj]['type']['extent'])
    inst = dict(inbounds=True, index_bits=bits, address_space=0, factors=[str(stride)],
                native_terms=[dict(kind='index', stride=stride, bits=bits)],
                operands=[{}, dict(type=dict(bits=bits))])
    try:
        replay.pointer_shift(inst, [pointer, replay.objects[obj]['count'] + 1])
    except (Reject, Unsupported):
        pass
    else:
        raise AssertionError('intermediate out-of-range stack pointer accepted')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--case', type=int, choices=range(19), action='append')
    parser.add_argument('--bits', type=int, choices=(32, 64), default=32)
    args = parser.parse_args()
    entry = args.ikos.resolve()
    runtime = runtime_identity(entry)
    command = runtime_command(entry)
    body = (ROOT / 'analyzer/test/regression/bmc-values/main-vla-startup.c').read_text()
    with tempfile.TemporaryDirectory(prefix='ikos-main-vla-') as temp:
        work = Path(temp)
        for wide in (False, True):
            for case in (range(19) if args.case is None else args.case):
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') +
                                  body.replace('worker', 'entry').replace('capacity', 'limit'))
                report = work / 'output.json'
                result = subprocess.run(command + ['--analyses=race', '--concurrency=auto',
                    '--partitioning=guards', '--demote-race-to-unknown', f'-m{args.bits}',
                    '--bmc-deadline-ms=20000', '--bmc-solver-timeout-ms=5000',
                    '--format=json', '--report-file=' + str(report), '-o', str(work / 'output.db'), str(source)],
                    env=dict(os.environ, IKOS_BMC_RECOVER='1', IKOS_BMC_DIAGNOSTICS='1'),
                    capture_output=True, text=True, timeout=35)
                output = result.stdout + result.stderr
                assert result.returncode == 0, (case, wide, output)
                verdict = classify(output, result.returncode, False)
                data = json.loads(report.read_text())
                assert (verdict == 'Race Found') == (case in POSITIVE), (case, wide, verdict, data, output)
                if case in POSITIVE:
                    checks = validate_report(data)
                    assert checks and all(c['status'] == 'ACCEPT' for c in checks), checks
                    if case == 0:
                        certificate = next(r['info']['counterexample'] for r in data['reports']
                                           if 'counterexample' in r['info'])
                        replay_mutations(certificate)
                print(f'CASE={case}/WIDE={int(wide)} {verdict} PASS', flush=True)
                if case == 18:
                    configured = subprocess.run(command + ['--analyses=race', '--concurrency=auto',
                        '--partitioning=guards', '--demote-race-to-unknown', '--argc=3', f'-m{args.bits}',
                        '-o', str(work / 'configured.db'), str(source)],
                        env=dict(os.environ, IKOS_BMC_RECOVER='1'), capture_output=True, text=True, timeout=35)
                    assert configured.returncode == 0, configured.stdout + configured.stderr
                    assert classify(configured.stdout + configured.stderr, configured.returncode, False) != 'Race Found'
                    print(f'CASE=18/WIDE={int(wide)}/argc3 rejected PASS', flush=True)
    verify_runtime(runtime)


if __name__ == '__main__':
    main()
