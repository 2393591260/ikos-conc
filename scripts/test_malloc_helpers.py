#!/usr/bin/env python3
"""Preserve malloc helper failure paths and validate successful NULL guards."""
import argparse
import copy
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT), str(ROOT / 'scripts')]
from ikos_runtime import runtime_command, runtime_identity, verify_runtime
from svcomp_race_test import classify, VERDICT_SAFE, VERDICT_RACE
from sweep_bmc_execution import validate_report
from validate_bmc_execution import Replay, Reject, Unsupported, digest_json


def check_ir(pp, work, bits):
    template = '''target datalayout = "e-p:BITS:BITS-i64:64-n8:16:32:64"
target triple = "TRIPLE"
@outside = global i32 0
declare i8* @malloc(iBITS)
declare void @abort() noreturn
declare i32 @__ikos_nondet_int()
define i8* @obtain(iBITS %size) noinline {
entry:
  %seed = call i32 @__ikos_nondet_int(), !ikos.freeze_uninit !0
  %p = call i8* @malloc(iBITS %size)
  %null = icmp eq i8* %p, null
  br i1 %null, label %fail, label %success
fail:
  call void @abort()
  unreachable
success:
  ret i8* %p
}
define i32 @main() {
  %p = call i8* @obtain(iBITS 16)
  ret i32 0
}
!0 = !{}
'''.replace('BITS', str(bits)).replace('TRIPLE', 'i386-pc-linux-gnu' if bits == 32 else 'x86_64-pc-linux-gnu')
    variants = [('valid', template, True),
                ('side-effect', template.replace('  ret i8* %p', '  store i32 1, i32* @outside\n  ret i8* %p'), False),
                ('optnone', template.replace('noinline {', 'noinline optnone {'), False),
                ('weak', template.replace('define i8* @obtain', 'define weak i8* @obtain'), False),
                ('atomic', template.replace('@obtain', '@__VERIFIER_atomic_obtain'), False),
                ('explicit-call-noinline', template.replace(f'%p = call i8* @obtain(i{bits} 16)',
                    f'%p = call i8* @obtain(i{bits} 16) noinline'), False),
                ('used-seed', template.replace('%null = icmp eq i8* %p, null',
                                               '%null = icmp eq i32 %seed, 0'), False)]
    for name, source, expanded in variants:
        path = work / 'helper.ll'
        path.write_text(source)
        result = subprocess.run([str(pp), '-opt=custom', '-S', str(path)],
                                capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, (name, result.stderr)
        main = re.search(r'define i32 @main\(\).*?\n}', result.stdout, re.S).group()
        assert ('call i8* @obtain' not in main and 'call i8* @__VERIFIER_atomic_obtain' not in main) == expanded, (name, main)
        if expanded:
            assert 'call i8* @malloc' in main and 'icmp eq' in main and 'call void @abort' in main, main
        print(f'IR {bits}/{name}: PASS', flush=True)


def corruptions(certificate):
    original = certificate['program']
    allocation = next(k for k, v in original['instructions'].items() if 'native_malloc' in v)
    compare = next(k for k, v in original['instructions'].items()
                   if v.get('kind') == 'compare' and v.get('predicate') == 36 and
                   any(x['kind'] == 'null' for x in v['operands']))
    for mode in ('missing', 'width', 'callee', 'convention', 'address-space', 'bundles', 'signature', 'guard'):
        program = copy.deepcopy(original)
        native = program['instructions'][allocation]['native_malloc']
        if mode == 'missing': program['instructions'][allocation].pop('native_malloc')
        elif mode == 'width': native['parameter_bits'] = [16]
        elif mode == 'callee': native['name'] = 'counterfeit'
        elif mode == 'convention': native['calling_convention'] = 8
        elif mode == 'address-space': native['return_address_space'] = 1
        elif mode == 'bundles': native['operand_bundles'] = True
        elif mode == 'signature': native['matching_signature'] = False
        else: program['instructions'][compare]['predicate'] = 35
        changed = dict(certificate, program_sha256=digest_json(program))
        try:
            Replay(program, changed).run()
        except (Reject, Unsupported):
            continue
        raise AssertionError('corrupt malloc execution accepted: ' + mode)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--bits', type=int, choices=(32, 64), default=32)
    args = parser.parse_args()
    runtime = runtime_identity(args.ikos.resolve())
    command = runtime_command(args.ikos.resolve())
    body = (ROOT / 'analyzer/test/regression/bmc-values/malloc-helper.c').read_text()
    with tempfile.TemporaryDirectory(prefix='ikos-malloc-helpers-') as temp:
        work = Path(temp)
        check_ir(args.ikos.resolve().parent / 'ikos-pp', work, args.bits)
        confirmed = {0, 5, 11, 16, 17} | ({10} if args.bits == 32 else set())
        for case in range(28):
            source = work / 'renamed.c'
            source.write_text(f'#define CASE {case}\n' + body.replace('obtain', 'allocate_bytes'))
            report = work / 'report.json'
            result = subprocess.run(command + ['--analyses=race', '--concurrency=auto',
                '--partitioning=guards', '--demote-race-to-unknown', '-m', str(args.bits),
                '--format=json', '--report-file=' + str(report), '-o', str(work / 'output.db'), str(source)],
                env=dict(os.environ, IKOS_BMC_RECOVER='1'), capture_output=True, text=True, timeout=30)
            verdict = classify(result.stdout + result.stderr, result.returncode, False)
            assert result.returncode == 0, (case, result.stdout, result.stderr)
            assert (verdict == VERDICT_RACE) == (case in confirmed), (case, args.bits, verdict, result.stdout, result.stderr)
            if case not in (2, 8, 21):
                assert verdict != VERDICT_SAFE, (case, args.bits, verdict)
            if case >= 24:
                data = json.loads(report.read_text())
                metadata = data['race_result']
                assert not validate_report(data), data
                assert metadata['verdict'] == 'UNKNOWN' and metadata['coverage'] == 'Blocked', metadata
                assert metadata['unresolved_count'] > 0 and not metadata['search_performed'], metadata
                assert any(o['reason'] == 'realloc outcome/storage lifetime not modeled'
                           for o in metadata['obligations']), metadata
            if case in confirmed:
                data = json.loads(report.read_text())
                checks = validate_report(data)
                assert checks and all(x['status'] == 'ACCEPT' for x in checks), (case, checks)
                if case == 0:
                    certificate = next(r['info']['counterexample'] for r in data['reports'] if 'counterexample' in r['info'])
                    corruptions(certificate)
            print(f'CASE={case}/bits={args.bits}: {verdict}', flush=True)
    verify_runtime(runtime)


if __name__ == '__main__':
    main()
