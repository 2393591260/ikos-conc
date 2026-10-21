#!/usr/bin/env python3
"""A closed private pthread handle can be detached without a completion edge."""
import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from ikos_runtime import runtime_command
from analyzer.python.ikos.race_result import validate_race_result
from sweep_bmc_execution import validate_report
from validate_bmc_execution import check, digest_json


def reject_export_corruptions(data):
    checks = 0
    for row in data['reports']:
        execution = row['info'].get('counterexample')
        if not execution:
            continue
        certificate = copy.deepcopy(execution)
        program = certificate.pop('program')
        executed = {step['statement'] for trace in certificate['traces'] for step in trace['steps']}
        calls = [key for key, inst in program['instructions'].items() if key in executed and
                 inst['kind'] == 'call' and inst['operands'][0].get('id') == 'pthread_detach']
        assert calls, 'positive sibling execution must detach before its second create'
        for change in ('width', 'return', 'calling-convention'):
            changed = copy.deepcopy(program)
            if change == 'width':
                changed['functions']['pthread_detach']['parameter_types'][0].update(bits=64, size=8, extent=8)
            elif change == 'return':
                changed['functions']['pthread_detach']['return_type'].update(bits=64, size=8, extent=8)
            else:
                changed['instructions'][calls[0]]['calling_convention'] = 8
            selected = dict(certificate, program_sha256=digest_json(changed))
            result = check(changed, selected)
            assert result['status'] == 'REJECT', (change, result)
            checks += 1
    assert checks, 'missing exported detach execution'
    return checks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    args = parser.parse_args()
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/detach-coverage.c').read_text()
    command = runtime_command(args.ikos.resolve())
    compiler = json.loads(subprocess.check_output(command[:-1] + ['-c',
        'import json; from ikos import settings, analyzer; '
        'print(json.dumps([settings.clang(), analyzer.clang_ikos_flags()]))'], text=True))
    corruptions = 0
    with tempfile.TemporaryDirectory(prefix='detach-coverage-') as folder:
        work = Path(folder)
        for wide in (False, True):
            for case in range(24):
                source, report = work / 'renamed.c', work / 'report.json'
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') + fixture)
                analyzed = source
                if case == 18:
                    # Clang -O0 omits declaration nonnull; test the actual IR premise.
                    analyzed = work / 'nonnull.ll'
                    subprocess.run([compiler[0], *compiler[1], '-S', '-emit-llvm', '-m32',
                        str(source), '-o', str(analyzed)], capture_output=True, check=True)
                    lines = analyzed.read_text().splitlines()
                    declarations = [i for i, line in enumerate(lines)
                                    if line.startswith('declare ') and '@pthread_create(' in line]
                    assert len(declarations) == 1, lines
                    index = declarations[0]
                    parts = lines[index].split(', ')
                    parts[1] += ' nonnull'
                    lines[index] = ', '.join(parts)
                    analyzed.write_text('\n'.join(lines) + '\n')
                proc = subprocess.run(command + ['--analyses=race', '--concurrency=auto',
                    '--demote-race-to-unknown', '--partitioning=guards', '-m32',
                    '--format=json', '--report-file=' + str(report),
                    '-o', str(work / 'output.db'), str(analyzed)],
                    capture_output=True, text=True, timeout=30,
                    env=dict(os.environ, IKOS_BMC_RECOVER='1'))
                assert proc.returncode == 0, (case, wide, proc.stdout, proc.stderr)
                data = json.loads(report.read_text())
                metadata = validate_race_result(data)
                expected = 'SAFE' if case in (1, 23) else 'FALSE' if case in (0, 22) else 'UNKNOWN'
                assert metadata['verdict'] == expected, (case, wide, metadata, proc.stdout)
                executions = validate_report(data)
                if case == 13:
                    # Analyze the real definition; current BMC cannot replay it.
                    assert metadata['coverage'] == 'Checked' and not metadata['obligations'], metadata
                    assert metadata['search_performed'] and metadata['remaining_pairs'] > 0 and not executions, metadata
                    body_line = (source.read_text().splitlines().index('  shared++;') + 1)
                    assert any(row['info'].get('bmc_result', {}).get('status') == 'UNSUPPORTED' and
                        any(pair['access_a']['thread_id'] == 'main' and
                            pair['access_a']['line'] == body_line
                            for pair in row['info'].get('conflicting_accesses', []))
                        for row in data['reports']), data
                elif expected == 'UNKNOWN':
                    # Invalid-lifecycle controls exercise rejection, not a source label.
                    assert metadata['coverage'] == 'Blocked' and any(
                        o['reason'] == 'external call effects not covered' and not o['contract']
                        for o in metadata['obligations']), metadata
                    assert not metadata['search_performed'] and not executions, metadata
                else:
                    assert metadata['coverage'] == 'Checked' and not metadata['obligations'], metadata
                    if expected == 'FALSE':
                        assert executions and all(e['status'] == 'ACCEPT' for e in executions), executions
                        if case == 0:
                            corruptions += reject_export_corruptions(data)
                print(f'CASE={case}/wide={wide}: {expected}', flush=True)
    print(f'48 ILP32 detach controls and {corruptions} exported ABI corruptions passed')


if __name__ == '__main__':
    main()
