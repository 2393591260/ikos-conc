#!/usr/bin/env python3
"""Check full atomic target tails, actual join generations, and auxiliary writes."""
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
from svcomp_race_test import classify, VERDICT_RACE, VERDICT_UNKNOWN
from validate_bmc_execution import check, digest_json
from analyzer.python.ikos.race_result import validate_race_result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    args = parser.parse_args()
    fixture = (ROOT / 'analyzer/test/regression/bmc-auxiliary/atomic-completion.c').read_text()
    positive = {0, 5, 7, 9, 10}
    with tempfile.TemporaryDirectory(prefix='ikos-atomic-completion-') as temp:
        work = Path(temp)
        cases = [(case, wide, 2, False) for wide in (False, True) for case in range(11)]
        cases += [(0, True, 20, False), (11, True, 2, False)]
        cases += [(case, wide, 2, True) for wide in (False, True)
                  for case in range(11) if case != 6]
        cases += [(0, True, 20, True)]
        for case, wide, count, covered in cases:
            source = work / 'renamed.c'
            body = fixture
            if covered:
                # Retain completion/corruption checks with covered local control and NULL arguments.
                body = body.replace('extern void __VERIFIER_assume(int);',
                    '#define __VERIFIER_assume(c) do { if (!(c)) __builtin_abort(); } while (0)')
                body = body.replace('intptr_t id = (intptr_t)argument;', 'intptr_t id = 2;')
                body = body.replace('worker, (void *)(intptr_t)i)', 'worker, 0)')
            source.write_text(f'#define CASE {case}\n#define N {count}\n' +
                              ('#define WIDE\n' if wide else '') +
                              body.replace('worker', 'entry').replace('value', 'payload'))
            report = work / 'report.json'
            report.unlink(missing_ok=True)
            proc = subprocess.run([str(args.ikos.resolve()), '--analyses=race',
                '--concurrency=auto', '--demote-race-to-unknown', '-m32', '--format=json',
                '--report-file=' + str(report), '-o', str(work / 'output.db'), str(source)],
                env=dict(os.environ, IKOS_BMC_RECOVER='1'), capture_output=True,
                text=True, timeout=30)
            verdict = classify(proc.stdout, proc.returncode, False)
            data = json.loads(report.read_text())
            metadata = validate_race_result(data)
            if not covered:
                assert metadata['verdict'] == 'UNKNOWN' and not metadata['search_performed'], metadata
                expected_reason = 'external call effects not covered'
                assert any(o['reason'] == expected_reason and not o['contract']
                           for o in metadata['obligations']), metadata
            confirmed = covered and case in positive
            assert proc.returncode == 0 and (verdict == VERDICT_RACE) == confirmed, (
                case, wide, count, verdict, proc.stdout, proc.stderr)
            if case == 6:
                assert verdict == VERDICT_UNKNOWN, (case, verdict)
            if confirmed:
                executions = [r['info']['counterexample'] for r in json.loads(report.read_text())['reports']
                              if 'counterexample' in r['info']]
                assert executions
                for execution in executions:
                    certificate = copy.deepcopy(execution)
                    program = certificate.pop('program')
                    certificate['program_sha256'] = digest_json(program)
                    result = check(program, certificate)
                    assert result['status'] == 'ACCEPT', (case, wide, result)
                    if case == 5:
                        assert set(result['instances']) in ({0, 1}, {0, 2}), result
                    for trace in certificate['traces']:
                        if trace['id'] == 'startup':
                            continue
                        assert program['instructions'][trace['steps'][-1]['statement']]['kind'] == 'return'
                        broken = copy.deepcopy(certificate)
                        changed = next(t for t in broken['traces'] if t['id'] == trace['id'])
                        changed['steps'].pop()
                        assert check(program, broken)['status'] == 'REJECT'
                        if trace['id'].startswith('aux_'):
                            broken = copy.deepcopy(certificate)
                            changed = next(t for t in broken['traces'] if t['id'] == trace['id'])
                            stores = [i for i, step in enumerate(changed['steps'])
                                      if program['instructions'][step['statement']]['kind'] == 'store']
                            if stores:
                                del changed['steps'][stores[0]]
                                assert check(program, broken)['status'] == 'REJECT'
            print(f'atomic-completion case={case}/wide={wide}/N={count}/covered={covered}: {verdict}', flush=True)


if __name__ == '__main__':
    main()
