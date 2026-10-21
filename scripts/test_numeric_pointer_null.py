#!/usr/bin/env python3
"""Closed numeric null tests retain actual bits and dynamic control dependencies."""
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    args = parser.parse_args()
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/numeric-pointer-null.c').read_text()
    corruptions = 0
    controls = 0
    with tempfile.TemporaryDirectory(prefix='ikos-numeric-null-') as folder:
        work = Path(folder)
        for wide in (False, True):
            for case in (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19):
                source, report = work / 'renamed.c', work / 'report.json'
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') + fixture)
                report.unlink(missing_ok=True)
                proc = subprocess.run(runtime_command(args.ikos) + ['--analyses=race',
                    '--concurrency=auto', '--demote-race-to-unknown', '--partitioning=guards',
                    '-m32', '--bmc-deadline-ms=20000', '--bmc-solver-timeout-ms=5000',
                    '--format=json', '--report-file=' + str(report),
                    '-o', str(work / 'output.db'), str(source)],
                    env=dict(os.environ, IKOS_BMC_RECOVER='1'), capture_output=True,
                    text=True, timeout=30)
                assert proc.returncode == 0, (case, wide, proc.stdout, proc.stderr)
                data = json.loads(report.read_text())
                m = validate_race_result(data)
                executions = validate_report(data)
                # The frontend folds CASE17's native roundtrip back to bits.
                if case >= 10 and case != 17:
                    assert m['verdict'] == 'UNKNOWN' and m['coverage'] == 'Blocked', (case, wide, m)
                    assert any(o['reason'] == 'integer-derived pointer aliasing is not modeled'
                               for o in m['obligations']), (case, wide, m)
                    assert not m['search_performed'] and not executions, m
                else:
                    assert m['coverage'] == 'Checked', (case, wide, m, proc.stdout)
                    if case in (1, 3):
                        assert m['verdict'] == 'SAFE', (case, wide, m)
                    elif case in (5, 6):
                        assert m['verdict'] in ('SAFE', 'UNKNOWN'), (case, wide, m)
                    else:
                        assert m['verdict'] == 'FALSE', (case, wide, m, proc.stdout)
                    if m['verdict'] == 'FALSE':
                        assert executions and all(e['status'] == 'ACCEPT' for e in executions), executions
                if case == 0 and m['verdict'] == 'FALSE':
                    for row in data['reports']:
                        original = row['info'].get('counterexample')
                        if not original:
                            continue
                        changed = copy.deepcopy(original)
                        program = changed.pop('program')
                        changed['program_sha256'] = digest_json(program)
                        replaced = False
                        for trace in changed['traces']:
                            for step in trace['steps']:
                                instruction = program['instructions'][step['statement']]
                                if instruction['kind'] == 'call' and '__VERIFIER_nondet_' in instruction['operands'][0].get('id', ''):
                                    assert 'encoded_result' in step, step
                                    step['encoded_result'] = '0'
                                    replaced = True
                        assert replaced, 'selected execution must demand the actual nondet value'
                        result = check(program, changed)
                        assert result['status'] == 'REJECT', result
                        corruptions += 1
                controls += 1
                print(f'CASE={case}/wide={wide}: {m["verdict"]}/{m["coverage"]}', flush=True)
    assert corruptions >= 1, corruptions
    print(f'{controls} ILP32 controls and {corruptions} corrupted executions passed')


if __name__ == '__main__':
    main()
