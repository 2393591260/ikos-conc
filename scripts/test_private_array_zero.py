#!/usr/bin/env python3
"""Check private array effects without authorizing memset execution in BMC/AR."""
import argparse
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from analyzer.python.ikos.race_result import validate_race_result
sys.path.insert(0, str(ROOT / 'scripts'))
from sweep_bmc_execution import validate_report
from ikos_runtime import runtime_command
from validate_bmc_execution import Replay, Unsupported


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    args = parser.parse_args()
    replay = Replay.__new__(Replay)
    replay.functions = {'ar.memset': dict(declaration=True)}
    replay.atomic_owner = None
    try:
        replay.api('ar.memset', [('local:array', 0), 0, 32, 4, 0], {}, {}, 'a', 0, None, None)
    except Unsupported:
        pass
    else:
        raise AssertionError('memset must not acquire an independent AR no-op model')
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/private-array-zero.c').read_text()
    supported = [[], ['INDEX'], ['LOOP'], ['TWO_INSTANCES'], ['LOCKED'],
                 ['CONTENT'], ['CONTENT', 'INDEX']]
    blocked = ['PARTIAL', 'SHIFTED', 'NONZERO', 'ESCAPE', 'THREAD_ARG',
               'RETURN_POINTER', 'ADDRESS', 'HELPER', 'MIXED', 'RECURSIVE',
               'VOLATILE_USE', 'POINTERS', 'STRUCTS', 'DYNAMIC', 'SEQUENTIAL_CALL',
               'MODULE_ASM', 'OTHER_EFFECT']
    with tempfile.TemporaryDirectory(prefix='ikos-private-zero-') as temp:
        work = Path(temp)
        for wide in (False, True):
            for options, covered in [(o, True) for o in supported] + [([o], False) for o in blocked]:
                options = options + (['WIDE'] if wide else [])
                source = work / 'renamed.c'
                source.write_text(''.join('#define ' + o + '\n' for o in options) + fixture)
                report = work / 'report.json'
                report.unlink(missing_ok=True)
                result = subprocess.run(runtime_command(args.ikos) + ['--analyses=race',
                    '--concurrency=auto', '--demote-race-to-unknown', '-m32',
                    '--bmc-deadline-ms=20000', '--bmc-solver-timeout-ms=5000',
                    '--partitioning=guards', '--log=debug', '--format=json', '--report-file=' + str(report),
                    '-o', str(work / 'output.db'), str(source)],
                    env=dict(os.environ, IKOS_BMC_RECOVER='1'), capture_output=True,
                    text=True, timeout=30)
                assert result.returncode == 0, (options, result.stdout, result.stderr)
                data = json.loads(report.read_text())
                metadata = validate_race_result(data)
                if covered:
                    assert metadata['coverage'] == 'Checked', (options, metadata, result.stdout, result.stderr)
                    if 'LOCKED' in options:
                        assert metadata['verdict'] == 'SAFE', (options, metadata)
                    elif 'CONTENT' in options:
                        assert metadata['verdict'] == 'UNKNOWN', (options, metadata)
                    else:
                        assert metadata['verdict'] == 'FALSE', (options, metadata)
                else:
                    assert metadata['coverage'] == 'Blocked' and metadata['verdict'] == 'UNKNOWN', (options, metadata)
                    assert not metadata['search_performed'], (options, metadata)
                    reason = ('external call effects not covered' if 'OTHER_EFFECT' in options
                              else 'byte memory effects not fully modeled')
                    assert any(o['reason'] == reason
                               for o in metadata['obligations']), (options, metadata)
                    if 'OTHER_EFFECT' in options:
                        assert not any(o['reason'] == 'byte memory effects not fully modeled'
                                       for o in metadata['obligations']), metadata
                if metadata['verdict'] == 'FALSE':
                    executions = validate_report(data)
                    assert executions and all(e['status'] == 'ACCEPT' for e in executions), (options, executions)
                    for row in data['reports']:
                        execution = row['info'].get('counterexample')
                        if not execution:
                            continue
                        for trace in execution['traces']:
                            for step in trace['steps']:
                                inst = execution['program']['instructions'][step['statement']]
                                assert not (inst['kind'] == 'call' and
                                    'memset' in inst['operands'][0].get('id', '')), inst
                print(options, metadata['verdict'], metadata['coverage'], flush=True)

        source = work / 'plain.c'
        source.write_text(fixture)
        raw = work / 'plain.ll'
        subprocess.run(['clang-14', '-m32', '-O0', '-Xclang', '-disable-O0-optnone',
                        '-g', '-S', '-emit-llvm', str(source), '-o', str(raw)], check=True)
        text = raw.read_text()
        for label, mutated in (
                ('volatile memset', re.sub(r'(call void @llvm\.memset[^\n]*i1 )false', r'\g<1>true', text)),
                ('repeated allocation', text.replace('alloca [8 x i32], align',
                                                    'alloca [8 x i32], i32 2, align'))):
            assert mutated != text, label
            changed = work / 'changed.ll'
            changed.write_text(mutated)
            result = subprocess.run(runtime_command(args.ikos) + ['--analyses=race',
                '--concurrency=auto', '--demote-race-to-unknown', '-m32', '--bmc-deadline-ms=0',
                '--format=json', '--report-file=' + str(report),
                '-o', str(work / 'output.db'), str(changed)],
                env=dict(os.environ, IKOS_BMC_RECOVER='1'), capture_output=True,
                text=True, timeout=30)
            assert result.returncode == 0, (label, result.stdout, result.stderr)
            metadata = validate_race_result(json.loads(report.read_text()))
            assert metadata['coverage'] == 'Blocked' and metadata['verdict'] == 'UNKNOWN', (label, metadata)
            assert any(o['reason'] == 'byte memory effects not fully modeled'
                       for o in metadata['obligations']), (label, metadata)
            print(label, metadata['verdict'], metadata['coverage'], flush=True)


if __name__ == '__main__':
    main()
