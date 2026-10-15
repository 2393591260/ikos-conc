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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    args = parser.parse_args()
    fixture = (ROOT / 'analyzer/test/regression/bmc-auxiliary/atomic-completion.c').read_text()
    positive = {0, 5, 7, 9, 10}
    with tempfile.TemporaryDirectory(prefix='ikos-atomic-completion-') as temp:
        work = Path(temp)
        cases = [(case, wide, 2) for wide in (False, True) for case in range(11)]
        cases += [(0, True, 20), (11, True, 2)]
        for case, wide, count in cases:
            source = work / 'renamed.c'
            source.write_text(f'#define CASE {case}\n#define N {count}\n' +
                              ('#define WIDE\n' if wide else '') +
                              fixture.replace('worker', 'entry').replace('value', 'payload'))
            report = work / 'report.json'
            report.unlink(missing_ok=True)
            proc = subprocess.run([str(args.ikos.resolve()), '--analyses=race',
                '--concurrency=auto', '--demote-race-to-unknown', '-m32', '--format=json',
                '--report-file=' + str(report), '-o', str(work / 'output.db'), str(source)],
                env=dict(os.environ, IKOS_BMC_RECOVER='1'), capture_output=True,
                text=True, timeout=30)
            verdict = classify(proc.stdout, proc.returncode, False)
            assert proc.returncode == 0 and (verdict == VERDICT_RACE) == (case in positive), (
                case, wide, count, verdict, proc.stdout, proc.stderr)
            if case == 6:
                assert verdict == VERDICT_UNKNOWN, (case, verdict)
            if case in positive:
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
                        assert set(result['instances']) == {0, 2}, result
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
            print(f'atomic-completion case={case}/wide={wide}/N={count}: {verdict}', flush=True)


if __name__ == '__main__':
    main()
