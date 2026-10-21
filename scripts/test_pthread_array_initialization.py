#!/usr/bin/env python3
"""Finite array initialization preserves byte effects and consumer obligations."""
import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import sqlite3
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from ikos_runtime import runtime_command, runtime_identity, verify_runtime
from analyzer.python.ikos.race_result import validate_race_result
from sweep_bmc_execution import validate_report
from validate_bmc_execution import check, digest_json

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--case', type=int, action='append', choices=range(31))
    args = parser.parse_args()
    identity = runtime_identity(args.ikos)
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/pthread-array-initialization.c').read_text()
    corruptions = 0
    with tempfile.TemporaryDirectory(prefix='ikos-pthread-array-') as folder:
        work = Path(folder)
        for wide in (False, True):
            for case in (range(31) if args.case is None else args.case):
                source, report = work / 'renamed.c', work / 'report.json'
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') + fixture)
                report.unlink(missing_ok=True)
                proc = subprocess.run(runtime_command(args.ikos) + ['--analyses=race',
                    '--concurrency=auto', '--demote-race-to-unknown', '--partitioning=guards',
                    '-m32', '--bmc-deadline-ms=20000', '--bmc-solver-timeout-ms=5000',
                    '--log=debug', '--format=json', '--report-file=' + str(report),
                    '-o', str(work / 'output.db'), str(source)],
                    env=dict(os.environ, IKOS_BMC_RECOVER='1'), capture_output=True,
                    text=True, timeout=30)
                assert proc.returncode == 0, (case, wide, proc.stdout, proc.stderr)
                data = json.loads(report.read_text())
                metadata = validate_race_result(data)
                executions = validate_report(data)
                with sqlite3.connect(work / 'output.db') as db:
                    infos = [json.loads(row) for row, in db.execute('SELECT info FROM checks')]
                proofs = [row for row in infos if row.get(
                    'pthread_initialization_contract') == 'finite-global-array-runtime-byte-frame-v1']
                if case in (0, 1, 2, 3, 27):
                    assert proofs and metadata['coverage'] == 'Checked', (case, wide, metadata, proc.stderr)
                    if case in (0, 2, 27):
                        assert metadata['verdict'] == 'FALSE', (case, wide, metadata, proc.stdout)
                    elif case == 1:
                        assert metadata['verdict'] == 'SAFE', (case, wide, metadata, proc.stdout)
                    else:
                        assert metadata['verdict'] != 'FALSE', (case, wide, metadata)
                elif case == 29:
                    # This user body uses the existing NULL-attribute model;
                    # only the new source certificate is being refused.
                    assert not proofs and metadata['verdict'] == 'UNKNOWN', (case, wide, metadata)
                else:
                    assert metadata['verdict'] == 'UNKNOWN' and metadata['coverage'] == 'Blocked', (case, wide, metadata)
                    if case == 4:
                        assert proofs and any(o['reason'] ==
                            'mutex array consumer does not establish an initialized leaf'
                            for o in metadata['obligations']), (case, wide, metadata)
                    else:
                        assert not proofs, (case, wide, metadata, proc.stdout)
                if metadata['verdict'] == 'FALSE':
                    assert executions and all(e['status'] == 'ACCEPT' for e in executions), executions
                if case == 0:
                    selected = next(row['info']['counterexample'] for row in data['reports']
                                    if row['info'].get('counterexample'))
                    broken = copy.deepcopy(selected)
                    program = broken.pop('program')
                    broken['program_sha256'] = digest_json(program)
                    removed = False
                    for trace in broken['traces']:
                        for position, step in enumerate(trace['steps']):
                            inst = program['instructions'][step['statement']]
                            if inst['kind'] == 'call' and 'pthread_mutexattr_init' in inst['operands'][0].get('id', ''):
                                del trace['steps'][position]
                                removed = True
                                break
                        if removed:
                            break
                    assert removed, 'counterexample must execute real attribute initialization'
                    result = check(program, broken)
                    assert result['status'] == 'REJECT', result
                    corruptions += 1
                    changed_program = copy.deepcopy(program)
                    changed_type = False
                    for instruction in changed_program['instructions'].values():
                        if instruction['kind'] == 'call' and 'pthread_mutexattr_settype' in instruction['operands'][0].get('id', ''):
                            assert instruction['operands'][2]['value'] == '2'
                            instruction['operands'][2]['value'] = '0'
                            changed_type = True
                    assert changed_type
                    changed_execution = copy.deepcopy(selected)
                    changed_execution.pop('program')
                    changed_execution['program_sha256'] = digest_json(changed_program)
                    result = check(changed_program, changed_execution)
                    assert result == {'status': 'UNSUPPORTED', 'reason': 'mutex attribute kind'}, result
                    corruptions += 1
                print(f'CASE={case}/wide={wide}: {metadata["verdict"]}/{metadata["coverage"]} proof={len(proofs)}', flush=True)
    if args.case is None or 0 in args.case:
        assert corruptions >= 4
    verify_runtime(identity)
    print(f"independent execution failure checks: {corruptions}", flush=True)

if __name__ == '__main__':
    main()
