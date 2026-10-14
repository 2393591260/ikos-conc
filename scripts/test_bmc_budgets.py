#!/usr/bin/env python3
"""Check budget forwarding, defaults, conservative exhaustion, and bad inputs."""
import argparse
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT), str(ROOT / 'scripts')]
from svcomp_race_test import classify, VERDICT_RACE, VERDICT_SAFE, VERDICT_UNKNOWN
from sweep_bmc_execution import validate_report

SOURCE = """#include <pthread.h>
int payload;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
void *writer(void *arg) {
  BEFORE
  payload = 1;
  AFTER
  return 0;
}
int main(void) {
  pthread_t child;
  pthread_create(&child, 0, writer, 0);
  BEFORE
  payload = 2;
  AFTER
  pthread_join(child, 0);
  return 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    args = parser.parse_args()
    entry = args.ikos.resolve()
    env = dict(os.environ, IKOS_BMC_RECOVER='1', IKOS_BMC_DIAGNOSTICS='1')
    assertions = 0
    with tempfile.TemporaryDirectory(prefix='ikos budgets ') as temp:
        work = Path(temp)
        source = work / 'renamed.c'
        report = work / 'report.json'
        database = work / 'output.db'

        def analyze(bits, locked, options):
            source.write_text(SOURCE.replace('BEFORE',
                'pthread_mutex_lock(&lock);' if locked else '').replace('AFTER',
                'pthread_mutex_unlock(&lock);' if locked else ''))
            report.unlink(missing_ok=True)
            proc = subprocess.run([str(entry), '--analyses=race', '--concurrency=auto',
                '--demote-race-to-unknown', '-m', str(bits), '--format=json',
                '--report-file=' + str(report), '-o', str(database), *options, str(source)],
                cwd=work, env=env, capture_output=True, text=True, timeout=30)
            assert proc.returncode == 0, proc.stdout + proc.stderr
            with sqlite3.connect(database) as db:
                settings = dict(db.execute('SELECT name, value FROM settings'))
            return classify(proc.stdout, proc.returncode, False), json.loads(report.read_text()), settings

        for bits in (32, 64):
            for locked in (False, True):
                for options, deadline, solver in (([], 10000, 1000),
                        (['--bmc-deadline-ms=10000', '--bmc-solver-timeout-ms=1000'], 10000, 1000),
                        (['--bmc-deadline-ms=0'], 0, 1000),
                        (['--bmc-deadline-ms=20000', '--bmc-solver-timeout-ms=2000'], 20000, 2000)):
                    verdict, data, settings = analyze(bits, locked, options)
                    expected = VERDICT_SAFE if locked else VERDICT_UNKNOWN if deadline == 0 else VERDICT_RACE
                    assert verdict == expected, (bits, locked, options, verdict)
                    assert int(settings['bmc-deadline-ms']) == deadline, settings
                    assert int(settings['bmc-solver-timeout-ms']) == solver, settings
                    assertions += 3
                    infos = [r['info'] for r in data['reports'] if 'bmc_result' in r['info']]
                    for info in infos:
                        assert info['bmc']['budget']['deadline_ms'] == deadline, info
                        assert info['bmc']['budget']['solver_timeout_ms'] == solver, info
                        assertions += 2
                    if not locked and deadline == 0:
                        assert infos and any(i['bmc']['counts'].get('search_deadline') for i in infos), infos
                        assert any('BUDGET_EXCEEDED' in i['bmc_result']['observed'] for i in infos), infos
                        assert not any('counterexample' in i for i in infos), infos
                        assertions += 3
                    if verdict == VERDICT_RACE:
                        checks = validate_report(data)
                        assert checks and all(c['status'] == 'ACCEPT' for c in checks), checks
                        assertions += 1
                    print(bits, locked, options or ['defaults'], verdict, flush=True)

        for flag, values in (('--bmc-deadline-ms', ('-1', '4294967296', 'x')),
                ('--bmc-solver-timeout-ms', ('0', '-1', '4294967296', 'x'))):
            for value in values:
                for executable in (entry, entry.with_name('ikos-analyzer')):
                    required = [] if executable == entry else ['-a=race', '--entry-points=main']
                    proc = subprocess.run([str(executable), *required, flag + '=' + value, str(source)],
                        cwd=work, env=env, capture_output=True, text=True, timeout=10)
                    assert proc.returncode != 0, (executable, flag, value, proc.stdout)
                    assert flag in proc.stderr, (executable, flag, value, proc.stderr)
                    assert 'The program is' not in proc.stdout, proc.stdout
                    assertions += 3
    print(f'{assertions} budget/verdict/execution/input assertions passed')


if __name__ == '__main__':
    main()
