#!/usr/bin/env python3
"""Check long concrete prefixes and position-sensitive value provenance."""
import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from ikos_runtime import runtime_command

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from svcomp_race_test import classify, VERDICT_RACE
from validate_bmc_execution import check, digest_json


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--bits', type=int, choices=(32, 64), default=32)
    args = parser.parse_args()
    fixture = (ROOT / 'analyzer/test/regression/bmc-correctness/loop-prefix.c').read_text()
    cases = [(0, 20, False, False), (0, 43, False, False),
             (0, 57, True, False), (0, 43, False, True)]
    cases += [(case, 43, False, False) for case in range(1, 6)]
    with tempfile.TemporaryDirectory(prefix='ikos-prefix-search-') as temp:
        work = Path(temp)
        for case, limit, wide, helper in cases:
            source = work / 'renamed.c'
            source.write_text(f'#define CASE {case}\n#define LIMIT {limit}\n' +
                              ('#define WIDE\n' if wide else '') +
                              ('#define HELPER\n' if helper else '') +
                              fixture.replace('worker', 'entry').replace('flag', 'ready'))
            report = work / 'report.json'
            report.unlink(missing_ok=True)
            proc = subprocess.run(runtime_command(args.ikos.resolve()) + ['--analyses=race',
                '--concurrency=auto', '--demote-race-to-unknown', f'-m{args.bits}', '--format=json',
                '--report-file=' + str(report), '-o', str(work / 'output.db'), str(source)],
                env=dict(os.environ, IKOS_BMC_RECOVER='1'), capture_output=True,
                text=True, timeout=30)
            verdict = classify(proc.stdout, proc.returncode, False)
            assert proc.returncode == 0 and (verdict == VERDICT_RACE) == (case in (0, 5)), (
                case, limit, wide, helper, verdict, proc.stdout, proc.stderr)
            if case in (0, 5):
                executions = [r['info']['counterexample'] for r in json.loads(report.read_text())['reports']
                              if 'counterexample' in r['info']]
                assert executions
                for execution in executions:
                    certificate = copy.deepcopy(execution)
                    program = certificate.pop('program')
                    certificate['program_sha256'] = digest_json(program)
                    result = check(program, certificate)
                    assert result['status'] == 'ACCEPT', (case, limit, result)
                    # A longer search must preserve every actual loop write.
                    stores = [step for trace in certificate['traces'] for step in trace['steps']
                              if program['instructions'][step['statement']]['kind'] == 'store'
                              and step.get('encoded_address', {}).get('object', {}).get('id') == 'progress']
                    assert len(stores) == limit + 1, (case, limit, len(stores))
                    broken = copy.deepcopy(certificate)
                    for trace in broken['traces']:
                        if stores[0] in trace['steps']:
                            trace['steps'].remove(stores[0])
                            break
                    assert check(program, broken)['status'] == 'REJECT'
            print(f'prefix case={case}/limit={limit}/wide={wide}/helper={helper}: {verdict}', flush=True)


if __name__ == '__main__':
    main()
