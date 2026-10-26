#!/usr/bin/env python3
"""Check constructor-only LLVM expansion and preserved source definedness."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from ikos_runtime import runtime_command

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from svcomp_race_test import classify
from sweep_bmc_execution import validate_report


def check_ir(pp, work):
    # This handwritten ILP32 IR unit is separate from the selected-ABI C controls.
    template = '''target datalayout = "e-p:32:32-i64:64-n8:16:32"
target triple = "i386-pc-linux-gnu"
%cell = type { i32, i8* }
@outside = global i32 0
declare i8* @malloc(i32)
declare void @opaque()
declare i32 @__ikos_nondet_int()
define void @sink(i32 %x) alwaysinline { ret void }
define i8* @factory(i32 %x) {
BODY
}
define i32 @main() {
  %a = call i8* @factory(i32 3)
  %b = call i8* @factory(i32 5)
  call void @sink(i32 undef)
  ret i32 0
}
!0 = !{}
'''
    body = '''entry:
  %p = call i8* @malloc(i32 8)
  %q = bitcast i8* %p to %cell*
  %v = getelementptr inbounds %cell, %cell* %q, i32 0, i32 0
  store i32 %x, i32* %v, align 4
  %n = getelementptr inbounds %cell, %cell* %q, i32 0, i32 1
  store i8* null, i8** %n, align 4
  ret i8* %p'''
    cases = {
        'valid': (body, True),
        'unused-formal': (body.replace('store i32 %x', 'store i32 7'), False),
        'overlap': (body.replace('  ret i8* %p', '  store i32 9, i32* %v\n  ret i8* %p'), False),
        'extent': (body.replace('@malloc(i32 8)', '@malloc(i32 4)'), False),
        'dynamic-extent': (body.replace('@malloc(i32 8)', '@malloc(i32 %x)'), False),
        'zero-extent': (body.replace('@malloc(i32 8)', '@malloc(i32 0)'), False),
        'large-extent': (body.replace('@malloc(i32 8)', '@malloc(i32 1048577)'), False),
        'load': (body.replace('  ret i8* %p', '  %read = load i32, i32* @outside\n  ret i8* %p'), False),
        'unknown-call': (body.replace('  ret i8* %p', '  call void @opaque()\n  ret i8* %p'), False),
        'used-freeze': (body.replace('entry:', 'entry:\n  %seed = call i32 @__ikos_nondet_int(), !ikos.freeze_uninit !0').replace('store i32 %x', 'store i32 %seed'), False),
        'dead-freeze': (body.replace('entry:', 'entry:\n  %seed = call i32 @__ikos_nondet_int(), !ikos.freeze_uninit !0'), True),
        'narrow-store': (body.replace('store i32 %x, i32* %v, align 4', '%small = trunc i32 %x to i8\n  %byte = bitcast i32* %v to i8*\n  store i8 %small, i8* %byte'), False),
        'branch': (body.replace('entry:', 'entry:\n  br label %body\nbody:'), False),
        'excess-alignment': (body.replace('align 4', 'align 16'), False),
        'interior-return': (body.replace('ret i8* %p', '%interior = bitcast i8** %n to i8*\n  ret i8* %interior'), False),
        'defined-malloc': (body, False),
    }
    for name, (code, accepted) in cases.items():
        source = work / 'constructor.ll'
        text = template.replace('BODY', code)
        if name == 'defined-malloc':
            text = text.replace('declare i8* @malloc(i32)',
                                'define i8* @malloc(i32 %bytes) { ret i8* null }')
        source.write_text(text)
        inventory = work / 'inventory.json'
        plain = subprocess.run([str(pp), '-opt=custom', '-S', str(source)],
                               capture_output=True, text=True, timeout=30)
        probe = subprocess.run([str(pp), '-opt=custom', '-S',
                                '-allocator-inventory=' + str(inventory), str(source)],
                               capture_output=True, text=True, timeout=30)
        assert plain.returncode == probe.returncode == 0, (name, probe.stderr)
        assert plain.stdout == probe.stdout, (name, 'inventory changed LLVM')
        assert json.loads(inventory.read_text()) == {'schema': 1, 'eligible_calls': 2 if accepted else 0}, name
        run = subprocess.run([str(pp), '-opt=custom', '-inline-allocators', '-S', str(source)],
                             capture_output=True, text=True, timeout=30)
        assert run.returncode == 0, (name, run.stderr)
        main = re.search(r'define i32 @main\(\).*?\n}', run.stdout, re.S).group()
        assert ('call i8* @factory' not in main) == accepted, (name, main)
        assert 'call void @sink(i32 undef)' in main, (name, main)
        if accepted:
            assert main.count('call i8* @malloc') == 2, (name, main)
            assert main.count('store i32') == 2, (name, main)
            if name == 'dead-freeze':
                assert main.count('!ikos.freeze_uninit') == 2, main
        print('IR', name, 'PASS', flush=True)
    run = subprocess.run([str(pp), '-inline-all', '-inline-allocators', str(source)],
                         capture_output=True, text=True, timeout=30)
    assert run.returncode != 0 and 'mutually exclusive' in run.stderr, run.stderr



def check_ir64_alignment(pp, work):
    # Native LP64 aggregate alignment may exceed an individual i32 field's
    # standalone ABI alignment. Over-aligned/misaligned stores remain ineligible.
    template = '''target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
target triple = "x86_64-pc-linux-gnu"
%cell = type { i32, i8* }
declare i8* @malloc(i64)
define i8* @factory(i32 %x) {
entry:
  %p = call i8* @malloc(i64 16)
  %q = bitcast i8* %p to %cell*
  %v = getelementptr inbounds %cell, %cell* %q, i64 0, i32 0
  store i32 %x, i32* %v, align 8
  %n = getelementptr inbounds %cell, %cell* %q, i64 0, i32 1
  store i8* null, i8** %n, align 8
  ret i8* %p
}
define i32 @main() {
  %a = call i8* @factory(i32 3)
  %b = call i8* @factory(i32 5)
  ret i32 0
}
'''
    misaligned = template.replace('%cell = type { i32, i8* }',
                                  '%cell = type { i32, i32, i8* }')
    misaligned = misaligned.replace('%q, i64 0, i32 1',
                                    '%q, i64 0, i32 2')
    misaligned = misaligned.replace('%q, i64 0, i32 0',
                                    '%q, i64 0, i32 1')
    cases = {
        'native-field-align8': (template, True),
        'excess-align16': (template.replace('align 8', 'align 16'), False),
        'excess-align32': (template.replace('align 8', 'align 32'), False),
        'offset4-align8': (misaligned, False),
        'defined-malloc': (template.replace(
            'declare i8* @malloc(i64)',
            'define i8* @malloc(i64 %bytes) { ret i8* null }'), False),
        'non-native-pointer-layout': (template.replace(
            'e-p:64:64-i64:64', 'e-p:64:128-i64:64').replace(
            'align 8', 'align 16').replace('i64 16)', 'i64 32)'), False),
    }
    for name, (code, accepted) in cases.items():
        source = work / 'constructor64.ll'
        source.write_text(code)
        inventory = work / 'inventory64.json'
        plain = subprocess.run([str(pp), '-opt=custom', '-S', str(source)],
                               capture_output=True, text=True, timeout=30)
        probe = subprocess.run([str(pp), '-opt=custom', '-S',
                                '-allocator-inventory=' + str(inventory), str(source)],
                               capture_output=True, text=True, timeout=30)
        assert plain.returncode == probe.returncode == 0, (name, probe.stderr)
        assert plain.stdout == probe.stdout, (name, 'inventory changed LLVM')
        assert json.loads(inventory.read_text()) == {
            'schema': 1, 'eligible_calls': 2 if accepted else 0}, name
        run = subprocess.run([str(pp), '-opt=custom', '-inline-allocators',
                              '-S', str(source)],
                             capture_output=True, text=True, timeout=30)
        assert run.returncode == 0, (name, run.stderr)
        main = re.search(r'define i32 @main\(\).*?\n}', run.stdout, re.S).group()
        assert ('call i8* @factory' not in main) == accepted, (name, main)
        if accepted:
            assert main.count('call i8* @malloc') == 2, (name, main)
            assert main.count('store i32') == 2, (name, main)
        print('IR64', name, 'PASS', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--bits', type=int, choices=(32, 64), default=32)
    args = parser.parse_args()
    pp = args.ikos.resolve().parent / 'ikos-pp'
    body = (ROOT / 'analyzer/test/regression/bmc-values/allocator-inlining.c').read_text()
    with tempfile.TemporaryDirectory(prefix='ikos-allocator-inlining-') as directory:
        work = Path(directory)
        check_ir(pp, work)
        check_ir64_alignment(pp, work)
        run = subprocess.run(runtime_command(args.ikos.resolve()) + ['--inline-all', '--inline-allocators', '--version'],
                             capture_output=True, text=True, timeout=30)
        assert run.returncode != 0 and 'not allowed with argument' in run.stderr, run.stderr
        for wide in (False, True):
            for case in range(8):
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') +
                                  body.replace('construct', 'build_object').replace('worker', 'entry'))
                report = work / 'report.json'
                report.unlink(missing_ok=True)
                run = subprocess.run(runtime_command(args.ikos.resolve()) + ['--inline-allocators',
                    '--analyses=race', '--concurrency=auto', '--partitioning=guards',
                    '--demote-race-to-unknown', f'-m{args.bits}', '--format=json',
                    '--report-file=' + str(report), '-o', str(work / 'output.db'), str(source)],
                    env=dict(os.environ, IKOS_BMC_RECOVER='1'), capture_output=True, text=True, timeout=40)
                verdict = classify(run.stdout + run.stderr, run.returncode, False)
                assert run.returncode == 0, run.stdout + run.stderr
                assert (verdict == 'Race Found') == (case in (0, 3)), (case, wide, verdict, run.stdout + run.stderr)
                if case in (2, 4, 5, 6, 7):
                    assert verdict == 'Unknown', (case, wide, verdict)
                data = json.loads(report.read_text())
                checks = validate_report(data)
                if case in (0, 3):
                    assert checks and all(x['status'] == 'ACCEPT' for x in checks), (case, wide, checks)
                print(f'CASE={case}/WIDE={int(wide)} {verdict} PASS', flush=True)


if __name__ == '__main__':
    main()
