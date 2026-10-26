#!/usr/bin/env python3
"""Validate actual mutex initialization lifetimes, not just current owners."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from ikos_runtime import runtime_command

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT), str(ROOT / 'scripts')]
from svcomp_race_test import classify
from sweep_bmc_execution import validate_report
from validate_bmc_execution import Replay, Thread, Reject, digest_json
from analyzer.python.ikos.race_result import validate_race_result


def api_contract(program, certificate):
    certificate = dict(certificate, program_sha256=digest_json(program))
    def instruction(name):
        return next(i for i in program['instructions'].values()
                    if i['kind'] == 'call' and i['operands'][0].get('id') == name)
    init = instruction('pthread_mutex_init')
    lock = instruction('ar.pthread.mutex.lock')
    unlock = instruction('ar.pthread.mutex.unlock')
    name = next(n for n, g in program['globals'].items()
                if g['type']['kind'] == 'array' and int(g['type']['count']) == 2)
    pointer = ('global:' + name, 0)
    main = Thread(0, [], {0: 1})
    for offset in (0, 4):
        replay = Replay(program, certificate)
        replay.api('pthread_mutex_init', [pointer, ('null', 0)], init, {}, 'a', 0, main, None)
        try:
            replay.api('pthread_mutex_init', [(pointer[0], offset), ('null', 0)],
                       init, {}, 'a', 1, main, None)
        except Reject:
            pass
        else:
            raise AssertionError('reinitialized live/overlapping mutex accepted')
    replay = Replay(program, certificate)
    mutex_extent = int(program['globals'][name]['type']['element']['extent'])
    for offset in (0, mutex_extent):
        replay.api('pthread_mutex_init', [(pointer[0], offset), ('null', 0)],
                   init, {}, 'a', 0, main, None)
    replay = Replay(program, certificate)
    replay.api('ar.pthread.mutex.lock', [pointer], lock, {}, 'a', 0, main, None)
    replay.api('ar.pthread.mutex.unlock', [pointer], unlock, {}, 'a', 1, main, None)
    try:
        replay.api('pthread_mutex_init', [pointer, ('null', 0)], init, {}, 'a', 2, main, None)
    except Reject:
        pass
    else:
        raise AssertionError('initialization after first use accepted')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--bits', type=int, choices=(32, 64), default=32)
    args = parser.parse_args()
    body = (ROOT / 'analyzer/test/regression/bmc-values/mutex-lifetime.c').read_text()
    positive = {0, 3, 4, 5, 12, 13, 14, 16}
    with tempfile.TemporaryDirectory(prefix='ikos-mutex-lifetime-') as tmp:
        work = Path(tmp)
        for wide in (False, True):
            for case in range(17):
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') +
                                  body.replace('guards', 'locks').replace('worker', 'entry'))
                report = work / 'report.json'
                report.unlink(missing_ok=True)
                proc = subprocess.run(runtime_command(args.ikos.resolve()) + ['--analyses=race',
                    '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                    '--bmc-deadline-ms=20000', '--bmc-solver-timeout-ms=5000', f'-m{args.bits}',
                    '--format=json', '--report-file=' + str(report), '-o', str(work / 'output.db'), str(source)],
                    env=dict(os.environ, IKOS_BMC_RECOVER='1'), capture_output=True, text=True, timeout=35)
                output = proc.stdout + proc.stderr
                assert proc.returncode == 0, (case, wide, output)
                verdict = classify(output, proc.returncode, False)
                data = json.loads(report.read_text())
                metadata = validate_race_result(data)
                if case in {8, 9}:
                    assert metadata['coverage'] == 'Blocked' and not metadata['search_performed'], metadata
                    assert any(o['reason'] == 'external call effects not covered' and not o['contract']
                               for o in metadata['obligations']), metadata
                assert (verdict == 'Race Found') == (case in positive), (case, wide, verdict, output)
                assert verdict != 'Safe', (case, wide, verdict)
                if case in positive:
                    checks = validate_report(data)
                    assert checks and all(c['status'] == 'ACCEPT' for c in checks), checks
                    if case == 0:
                        certificate = next(r['info']['counterexample'] for r in data['reports']
                                           if 'counterexample' in r['info'])
                        api_contract(certificate['program'], certificate)
                print(f'CASE={case}/WIDE={int(wide)} {verdict} PASS', flush=True)


if __name__ == '__main__':
    main()
