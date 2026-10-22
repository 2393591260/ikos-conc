#!/usr/bin/env python3
"""Check consumer-only ablations and side-effect-free pair observations."""
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
from ikos_runtime import runtime_command
from svcomp_race_test import classify
from sweep_bmc_execution import validate_report

FLAGS = ('IKOS_RACE_DISABLE_OWNER_CONSUMER', 'IKOS_RACE_PAIR_OBSERVATIONS',
         'IKOS_BMC_DISABLE_SEMAPHORE_FACTS', 'IKOS_BMC_DISABLE_AI_FACTS',
         'IKOS_BMC_DISABLE_AI_BOUNDS', 'IKOS_BMC_DISABLE_AI_NO_WRAP',
         'IKOS_BMC_DISABLE_AI_SINGLE_EXECUTION', 'IKOS_RACE_RULE_REPORT')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='ikos-cooperation-') as temp:
        work = Path(temp)
        def run(name, fixture, flags=(), observe=False):
            folder = work / name
            folder.mkdir()
            source = folder / 'input.c'
            source.write_text('#define CASE 0\n' +
                (ROOT / 'analyzer/test/regression/bmc-values' / fixture).read_text())
            env = {k: v for k, v in os.environ.items() if k not in FLAGS}
            env['IKOS_BMC_RECOVER'] = '1'
            env.update(dict.fromkeys(flags, '1'))
            env['IKOS_RACE_RULE_REPORT'] = str(folder / 'rules.jsonl')
            if observe:
                env['IKOS_RACE_PAIR_OBSERVATIONS'] = str(folder / 'pairs.jsonl')
            command = runtime_command(args.ikos.resolve()) + [
                '--analyses=race', '--concurrency=auto', '--partitioning=guards',
                '--demote-race-to-unknown', '--bmc-deadline-ms=20000',
                '--bmc-solver-timeout-ms=1000', '-m32', '--format=json',
                '--report-file=' + str(folder / 'report.json'),
                '-o', str(folder / 'output.db'), str(source)]
            proc = subprocess.run(command, env=env, capture_output=True, text=True, timeout=60)
            assert proc.returncode == 0, proc.stdout + proc.stderr
            verdict = classify(proc.stdout + proc.stderr, proc.returncode, False)
            report = json.loads((folder / 'report.json').read_text())
            if verdict == 'Race Found':
                checks = validate_report(report)
                assert checks and all(c['status'] == 'ACCEPT' for c in checks), checks
            with sqlite3.connect(folder / 'output.db') as db:
                settings = {k: json.loads(v) for k, v in db.execute(
                    "SELECT name,value FROM settings WHERE name='race_result'")}
            pairs = [json.loads(line) for line in (folder / 'pairs.jsonl').read_text().splitlines()] if observe else []
            rules = [json.loads(line) for line in (folder / 'rules.jsonl').read_text().splitlines()]
            return verdict, settings['race_result'], pairs, rules

        normal = run('normal', 'mutex-array-effects.c')
        diag = run('diag', 'mutex-array-effects.c', observe=True)
        suppressed = run('suppressed', 'mutex-array-effects.c',
                         ('IKOS_RACE_DISABLE_OWNER_CONSUMER',), observe=True)
        assert normal[0] == diag[0] == 'Safe', (normal, diag)
        assert normal[1] == diag[1] and normal[3] == diag[3]
        assert suppressed[0] == 'Unknown', suppressed[0]
        assert any(p['owner_suppressed'] for p in suppressed[2])
        def keys(rows):
            return [(p['a']['statement'], p['a']['context_leaf_to_root'], p['a']['thread'],
                     p['b']['statement'], p['b']['context_leaf_to_root'], p['b']['thread'],
                     p['role_a'], p['role_b']) for p in rows]
        assert keys(diag[2]) == keys(suppressed[2])
        for key in ('obligation_count', 'unresolved_count', 'safety_only_count'):
            assert diag[1][key] == suppressed[1][key]
        sem = run('sem', 'semaphore-prefix.c')
        disconnected = run('disconnected', 'semaphore-prefix.c',
                           ('IKOS_BMC_DISABLE_SEMAPHORE_FACTS',))
        assert sem[0] == 'Race Found' and disconnected[0] == 'Unknown', (sem[0], disconnected[0])
        for key in ('obligation_count', 'unresolved_count', 'safety_only_count'):
            assert sem[1][key] == disconnected[1][key]
        print('PASS: observations preserve verdict/coverage/rule counts; stable pair keys; '
              'owner suppression and semaphore disconnection conservatively decline')


if __name__ == '__main__':
    main()
