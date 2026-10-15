#!/usr/bin/env python3
"""Check sync-free sibling executions without weakening actual HB validation."""
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
from svcomp_race_test import classify, VERDICT_RACE
from validate_bmc_execution import check, digest_json


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    args = parser.parse_args()
    fixture = (ROOT / 'analyzer/test/regression/bmc-correctness/sibling-atomic.c').read_text()
    cases = [(case, False) for case in range(15)] + [(case, True) for case in (0, 2, 3, 4, 7, 8, 10, 11)]
    positive = {0, 10, 11}
    with tempfile.TemporaryDirectory(prefix='ikos-sibling-atomic-') as temp:
        work = Path(temp)
        for case, wide in cases:
            source = work / 'renamed.c'
            source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') +
                              fixture.replace('worker', 'entry').replace('payload', 'datum'))
            report = work / 'report.json'
            report.unlink(missing_ok=True)
            proc = subprocess.run([str(args.ikos.resolve()), '--analyses=race',
                '--concurrency=auto', '--demote-race-to-unknown', '-m32', '--format=json',
                '--report-file=' + str(report), '-o', str(work / 'output.db'), str(source)],
                env=dict(os.environ, IKOS_BMC_RECOVER='1', IKOS_BMC_DIAGNOSTICS='1'),
                capture_output=True, text=True, timeout=30)
            verdict = classify(proc.stdout, proc.returncode, False)
            assert proc.returncode == 0 and (verdict == VERDICT_RACE) == (case in positive), (
                case, wide, verdict, proc.stdout, proc.stderr)
            infos = [r['info'] for r in json.loads(report.read_text())['reports'] if 'counterexample' in r['info']]
            for info in infos:
                assert info['bmc']['counts'].get('synchronization_free_siblings', 0) > 0
                certificate = copy.deepcopy(info['counterexample'])
                program = certificate.pop('program')
                certificate['program_sha256'] = digest_json(program)
                result = check(program, certificate)
                assert result['status'] == 'ACCEPT', (case, wide, result)
                assert set(result['instances']) == {1, 2}
                assert sum(t['atomic'] for t in result['targets']) == 1
                for label in ('a', 'b'):
                    broken = copy.deepcopy(certificate)
                    trace = next(t for t in broken['traces'] if t['id'] == label)
                    trace['steps'].pop()
                    assert check(program, broken)['status'] == 'REJECT'
                broken = copy.deepcopy(certificate)
                a, b = (next(t for t in broken['traces'] if t['id'] == label) for label in ('a', 'b'))
                a['creator_step'] = b['creator_step']
                assert check(program, broken)['status'] == 'REJECT'
            assert bool(infos) == (case in positive)
            print(f'sibling case={case}/wide={wide}: {verdict}', flush=True)


if __name__ == '__main__':
    main()
