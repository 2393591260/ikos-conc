#!/usr/bin/env python3
"""Check public verdicts, installed entry, and incomplete-output rejection."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from svcomp_race_test import classify, VERDICT_RACE, VERDICT_SAFE, VERDICT_UNKNOWN

SOURCE = """#include <pthread.h>
int payload;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
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


class VerdictTests(unittest.TestCase):
    def test_complete_lines_only(self):
        for output in ('potential data race', 'diagnostic: The program is SAFE',
                       'The program is SAFE-ish', '', 'analysis failed'):
            with self.subTest(output=output):
                self.assertTrue(classify(output, 0, False).startswith('Error'))
        for line, expected in (('SAFE', VERDICT_SAFE),
                               ('definitely UNSAFE', VERDICT_RACE),
                               ('UNKNOWN', VERDICT_UNKNOWN),
                               ('potentially UNSAFE', VERDICT_UNKNOWN)):
            output = 'potential data race\nThe program is ' + line + '\n'
            self.assertEqual(classify(output, 0, False), expected)
            self.assertTrue(classify(output, 1, False).startswith('Error'))
            self.assertTrue(classify(output, -9, False).startswith('Error'))
            self.assertEqual(classify(output, 0, True), 'Error (timeout)')
        self.assertTrue(classify('The program is SAFE\nThe program is UNKNOWN',
                                 0, False).startswith('Error'))
        self.assertEqual(classify('\x1b[32mThe program is SAFE\x1b[0m\n',
                                  0, False), VERDICT_SAFE)

    def test_modes_and_installed_entry(self):
        with tempfile.TemporaryDirectory(prefix='ikos public modes ') as temp:
            work = Path(temp)
            source = work / 'renamed.c'
            entry = self.ikos.parent / 'svcomp_witness.py'
            self.assertTrue(entry.is_file(), 'complete installation must include the entry')
            for bits in (self.bits,):
                for locked in (False, True):
                    source.write_text(SOURCE.replace('BEFORE',
                        'pthread_mutex_lock(&mutex);' if locked else '').replace('AFTER',
                        'pthread_mutex_unlock(&mutex);' if locked else ''))
                    for mode in ('default', 'demote', 'bmc'):
                        env = dict(os.environ)
                        env.pop('IKOS_BMC_RECOVER', None)
                        options = [] if mode == 'default' else ['--demote-race-to-unknown']
                        if mode == 'bmc':
                            env['IKOS_BMC_RECOVER'] = '1'
                        report = work / 'report.json'
                        proc = subprocess.run([str(self.ikos), '--analyses=race',
                            '--concurrency=auto', '-m', bits, *options, '--format=json',
                            '--report-file=' + str(report), '-o', str(work / 'output.db'),
                            str(source)], cwd=work, env=env, capture_output=True,
                            text=True, timeout=30)
                        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
                        actual = classify(proc.stdout, proc.returncode, False)
                        expected = VERDICT_SAFE if locked else (
                            VERDICT_RACE if mode == 'bmc' else VERDICT_UNKNOWN)
                        self.assertEqual(actual, expected, (bits, locked, mode, proc.stdout))
                        checks = json.loads(report.read_text())['reports']
                        confirmed = [r for r in checks if r.get('info', {}).get(
                            'bmc_result', {}).get('status') == 'CONFIRMED']
                        self.assertEqual(bool(confirmed), not locked and mode == 'bmc')
                        for row in checks:
                            if 'access_a' in row.get('info', {}):
                                self.assertEqual(row['status'] == 2, row in confirmed)
                    witness = work / 'witness.yml'
                    witness.write_text('stale output')
                    proc = subprocess.run([str(entry), '--data-model',
                        'ILP32' if bits == '32' else 'LP64', '--out', str(witness),
                        str(source)], cwd=work, capture_output=True, text=True, timeout=30)
                    self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
                    self.assertEqual(classify(proc.stdout, 0, False),
                                     VERDICT_SAFE if locked else VERDICT_RACE)
                    self.assertEqual(witness.exists(), not locked)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bits', choices=('32', '64'), default='32')
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    args = parser.parse_args()
    VerdictTests.ikos = args.ikos.resolve()
    VerdictTests.bits = args.bits
    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(VerdictTests))
    raise SystemExit(not result.wasSuccessful())
