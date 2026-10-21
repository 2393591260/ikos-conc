#!/usr/bin/env python3
"""Validate executed constant addresses and native GEP premises on both ABIs."""
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


def gep_checks(bits):
    replay = Replay.__new__(Replay)
    replay.semaphores = {}
    replay.program = dict(pointer_bits=bits)
    replay.objects = {'stack': dict(size=16, live=True, thread_local=False, owner_frame=1)}
    replay.memory = {'stack': [None] * 16}
    inst = dict(inbounds=True, address_space=0, index_bits=bits, factors=['4'],
                native_terms=[dict(kind='index', stride=4, bits=64)],
                operands=[{}, dict(type=dict(bits=64))])
    end = replay.pointer_shift(inst, [('stack', 0), 4])
    assert replay.pointer_shift(inst, [end, -4]) == ('stack', 0)
    bad = [dict(inst, inbounds=False), dict(inst, address_space=1),
           dict(inst, native_terms=[]), dict(inst, factors=['0'],
                native_terms=[dict(kind='index', stride=1 << 32, bits=64)]),
           dict(inst, native_terms=[dict(kind='index', stride=4, bits=32)]),
           dict(inst, factors=['1'], native_terms=[dict(kind='field', offset=1 << 32)],
                operands=[{}, dict(kind='integer', value='0', type=dict(bits=32))])]
    for operation, values in [(i, [('stack', 0), 0]) for i in bad] + [
            (inst, [('stack', 0), 5]), (inst, [('stack', 0), 1 << 32])]:
        try:
            replay.pointer_shift(operation, values)
        except (Reject, Unsupported):
            pass
        else:
            raise AssertionError('invalid native or intermediate GEP accepted')


def corruptions(certificate):
    original = certificate['program']
    shift = next(step['statement'] for trace in certificate['traces'] for step in trace['steps']
                 if original['instructions'][step['statement']].get('kind') == 'pointer_shift' and
                 any(t.get('kind') == 'index' for t in
                     original['instructions'][step['statement']].get('native_terms', [])))
    for mode in ('missing', 'stride', 'width', 'field'):
        program = copy.deepcopy(original)
        inst = program['instructions'][shift]
        term = next(t for t in inst['native_terms'] if t['kind'] == 'index')
        if mode == 'missing': inst.pop('native_terms')
        elif mode == 'stride': term['stride'] = int(term['stride']) + 1
        elif mode == 'width': term['bits'] += 1
        else: term.update(kind='field', offset=1 << 32)
        changed = dict(certificate, program_sha256=digest_json(program))
        try:
            Replay(program, changed).run()
        except (Reject, Unsupported):
            pass
        else:
            raise AssertionError('corrupt native GEP evidence accepted: ' + mode)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bits', type=int, choices=(32, 64), default=32)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    args = parser.parse_args()
    runtime = runtime_identity(args.ikos.resolve())
    command = runtime_command(args.ikos.resolve())
    body = (ROOT / 'analyzer/test/regression/bmc-values/executed-offset.c').read_text()
    gep_checks(args.bits)
    with tempfile.TemporaryDirectory(prefix='ikos-executed-offset-') as tmp:
        work = Path(tmp)
        native = (ROOT / 'analyzer/test/regression/bmc-values/executed-offset.ll').read_text()
        native = native.replace('declare i32 @pthread_create', '@index = global i32 0\ndeclare i32 @pthread_create')
        if args.bits == 64:
            native = native.replace('target datalayout = "e-m:e-p:32:32-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:32-n8:16:32-S128"',
                                    'target datalayout = "e-m:e-p:64:64-i64:64-f80:128-n8:16:32:64-S128"')
            native = native.replace('i386-pc-linux-gnu', 'x86_64-pc-linux-gnu')
            native = native.replace('@pthread_create(i32*', '@pthread_create(i64*')
            native = native.replace('@pthread_join(i32', '@pthread_join(i64')
            native = native.replace('%handle = alloca i32, align 4', '%handle = alloca i64, align 8')
            native = native.replace('i32* %handle', 'i64* %handle')
            native = native.replace('%thread = load i32', '%thread = load i64')
        address_cases = [('', True),
            ('%end = getelementptr inbounds i32, i32* %base, i32 4\n'
             '  %back = getelementptr inbounds i32, i32* %end, i32 -4', True),
            ('%end = getelementptr inbounds i32, i32* %base, i32 5\n'
             '  %back = getelementptr inbounds i32, i32* %end, i32 -5', False),
            ('%large = bitcast i32* %base to [4294967296 x i8]*\n'
             '  %shifted = getelementptr inbounds [4294967296 x i8], [4294967296 x i8]* %large, i32 1\n'
             '  %back = bitcast [4294967296 x i8]* %shifted to i32*', False),
            ('%large = bitcast i32* %base to { [4294967296 x i8], i32 }*\n'
             '  %back = getelementptr inbounds { [4294967296 x i8], i32 }, '
             '{ [4294967296 x i8], i32 }* %large, i32 0, i32 1', False),
            ('%origin = getelementptr inbounds i32, i32* @index, i32 0\n'
             '  %index = load volatile i32, i32* %origin, align 4\n'
             '  %back = getelementptr inbounds i32, i32* %base, i32 %index', True),
            ('%end = getelementptr inbounds i32, i32* @index, i32 2\n'
             '  %origin = getelementptr inbounds i32, i32* %end, i32 -2\n'
             '  %index = load volatile i32, i32* %origin, align 4\n'
             '  %back = getelementptr inbounds i32, i32* %base, i32 %index', False),
            ('%large = bitcast i32* @index to [4294967296 x i8]*\n'
             '  %shifted = getelementptr inbounds [4294967296 x i8], [4294967296 x i8]* %large, i32 1\n'
             '  %origin = bitcast [4294967296 x i8]* %shifted to i32*\n'
             '  %index = load volatile i32, i32* %origin, align 4\n'
             '  %back = getelementptr inbounds i32, i32* %base, i32 %index', False)]
        for index, (address, confirmed) in enumerate(address_cases):
            source = work / 'native.ll'
            body_ll = native.replace('; ADDRESS', address)
            if address: body_ll = body_ll.replace('load volatile i32, i32* %base', 'load volatile i32, i32* %back')
            source.write_text(body_ll)
            report = work / 'native.json'
            result = subprocess.run(command + ['--analyses=race', '--concurrency=auto',
                '--partitioning=guards', '--demote-race-to-unknown', f'-m{args.bits}',
                '--bmc-deadline-ms=20000', '--bmc-solver-timeout-ms=5000',
                '--format=json', '--report-file=' + str(report), '-o', str(work / 'native.db'), str(source)],
                env=dict(os.environ, IKOS_BMC_RECOVER='1'), capture_output=True, text=True, timeout=35)
            assert result.returncode == 0, (index, result.stdout, result.stderr)
            verdict = classify(result.stdout + result.stderr, result.returncode, False)
            if confirmed:
                assert verdict == 'Race Found', (index, args.bits, verdict)
            else:
                assert verdict in ('Unknown', 'Safe'), (index, args.bits, verdict)
            if confirmed:
                checks = validate_report(json.loads(report.read_text()))
                assert checks and all(c['status'] == 'ACCEPT' for c in checks), checks
            print(f'ABI={args.bits}/NATIVE={index} {verdict} PASS', flush=True)
        for wide in (False, True):
            for case in range(13):
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') +
                                  body.replace('reader', 'entry').replace('State', 'Record'))
                report = work / 'output.json'
                result = subprocess.run(command + ['--analyses=race', '--concurrency=auto',
                    '--partitioning=guards', '--demote-race-to-unknown', f'-m{args.bits}',
                    '--bmc-deadline-ms=20000', '--bmc-solver-timeout-ms=5000',
                    '--format=json', '--report-file=' + str(report), '-o', str(work / 'output.db'), str(source)],
                    env=dict(os.environ, IKOS_BMC_RECOVER='1', IKOS_BMC_DIAGNOSTICS='1'),
                    capture_output=True, text=True, timeout=35)
                assert result.returncode == 0, (case, wide, result.stdout, result.stderr)
                verdict = classify(result.stdout + result.stderr, result.returncode, False)
                data = json.loads(report.read_text())
                expected = case in (0, 1, 4, 5, 7, 9, 10)
                if case in (0, 1, 4, 5, 7, 9, 10):
                    assert verdict == ('Race Found' if expected else 'Unknown'), (case, wide, args.bits, verdict)
                elif case == 8:
                    assert verdict == 'Safe', (case, wide, args.bits, verdict)
                else:
                    assert verdict in ('Unknown', 'Safe'), (case, wide, args.bits, verdict)
                if expected:
                    checks = validate_report(data)
                    assert checks and all(c['status'] == 'ACCEPT' for c in checks), checks
                    if case == 0:
                        certificate = next(r['info']['counterexample'] for r in data['reports']
                                           if 'counterexample' in r['info'])
                        corruptions(certificate)
                print(f'ABI={args.bits}/CASE={case}/WIDE={int(wide)} {verdict} PASS', flush=True)
    verify_runtime(runtime)


if __name__ == '__main__':
    main()
