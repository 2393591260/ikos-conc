#!/usr/bin/env python3
"""Check atomic sibling boundaries and reject ordered publication evidence."""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from ikos_runtime import runtime_command

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from svcomp_race_test import classify, VERDICT_RACE, VERDICT_UNKNOWN
from validate_bmc_execution import Replay, Thread, Unsupported, check, digest_json


def publication_contract():
    directory = ROOT / 'analyzer/test/regression/bmc-correctness'
    saved = json.loads((directory / 'atomic-publication-execution.json').read_text())
    assert hashlib.sha256((directory / 'atomic-publication.c').read_bytes()).hexdigest() == saved['source_sha256']
    certificate = saved['counterexample']
    program = certificate.pop('program')
    certificate['program_sha256'] = digest_json(program)
    result = check(program, certificate)
    assert result == dict(status='UNSUPPORTED', reason='targets ordered by conservative verifier-atomic synchronization'), result

    # An atomic block must not publish synchronization before its end.
    replay = Replay.__new__(Replay)
    replay.atomic_owner = 1
    for name in ('pthread_mutex_unlock', 'pthread_create', 'pthread_join', 'puts', 'malloc', 'free'):
        replay.functions = {name: {}}
        try:
            replay.api(name, [], {}, {}, 'a', 0, Thread(1, [], {1: 1}), None)
        except Unsupported as error:
            assert str(error) == 'external API inside verifier atomic: ' + name
        else:
            raise AssertionError('accepted unmodeled atomic API: ' + name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--bits', type=int, choices=(32, 64), default=32)
    args = parser.parse_args()
    publication_contract()
    fixture = (ROOT / 'analyzer/test/regression/bmc-correctness/sibling-atomic.c').read_text()
    cases = [(case, False) for case in range(17)] + [(case, True) for case in (0, 2, 3, 4, 7, 8, 10, 11, 15, 16)]
    positive = {15}
    with tempfile.TemporaryDirectory(prefix='ikos-sibling-atomic-') as temp:
        work = Path(temp)
        for case, wide in cases:
            source = work / 'renamed.c'
            source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') +
                              fixture.replace('worker', 'entry').replace('payload', 'datum'))
            report = work / 'report.json'
            report.unlink(missing_ok=True)
            proc = subprocess.run(runtime_command(args.ikos.resolve()) + ['--analyses=race',
                '--concurrency=auto', '--demote-race-to-unknown', f'-m{args.bits}', '--format=json',
                '--report-file=' + str(report), '-o', str(work / 'output.db'), str(source)],
                env=dict(os.environ, IKOS_BMC_RECOVER='1', IKOS_BMC_DIAGNOSTICS='1'),
                capture_output=True, text=True, timeout=30)
            verdict = classify(proc.stdout, proc.returncode, False)
            assert proc.returncode == 0 and (verdict == VERDICT_RACE) == (case in positive), (
                case, wide, verdict, proc.stdout, proc.stderr)
            rows = json.loads(report.read_text())['reports']
            if case == 16:
                assert verdict == VERDICT_UNKNOWN, (case, wide, verdict)
                assert any(r['info'].get('bmc', {}).get('counts', {}).get('atomic_external_effect')
                           for r in rows), rows
            infos = [r['info'] for r in rows if 'counterexample' in r['info']]
            for info in infos:
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
        source = ROOT / 'analyzer/test/regression/bmc-correctness/atomic-publication.c'
        proc = subprocess.run(runtime_command(args.ikos.resolve()) + ['--analyses=race', '--concurrency=auto',
            '--demote-race-to-unknown', f'-m{args.bits}', '-o', str(work / 'publication.db'), str(source)],
            env=dict(os.environ, IKOS_BMC_RECOVER='1'), capture_output=True, text=True, timeout=30)
        assert proc.returncode == 0 and classify(proc.stdout, proc.returncode, False) != VERDICT_RACE, (proc.stdout, proc.stderr)
        print('atomic publication: no false race', flush=True)


if __name__ == '__main__':
    main()
