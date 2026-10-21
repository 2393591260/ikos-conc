#!/usr/bin/env python3
"""Check supported behavior and conservative boundaries with real target headers."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT), str(ROOT / 'scripts')]
from ikos_runtime import runtime_command, runtime_identity, verify_runtime
from svcomp_race_test import classify
from sweep_bmc_execution import validate_report

# Property expectations are independent of the implementation's observed verdict.
CASES = [
    ('join-copy', 'safe', ('Safe', 'Safe')),
    ('detach-overlap', 'race', ('Race Found', 'Unknown')),
    ('heap-native-layout', 'race', ('Race Found', 'Race Found')),
    ('heap-overlap', 'race', ('Race Found', 'Race Found')),
    ('numeric-truncation', 'mixed', ('Race Found', 'Unknown')),
    ('numeric-sign-extension', 'race', ('Race Found', 'Unknown')),
    ('mutex-before-create', 'safe', ('Safe', 'Safe')),
    ('once-common-control', 'safe', ('Safe', 'Safe')),
    ('once-distinct-controls', 'race', ('Unknown', 'Unknown')),
    ('barrier-ordered', 'safe', ('Unknown', 'Unknown')),
    ('barrier-overlap', 'race', ('Unknown', 'Unknown')),
    ('recursive-mutex', 'safe', ('Unknown', 'Unknown')),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--bits', type=int, choices=(32, 64))
    args = parser.parse_args()
    env = dict(os.environ, IKOS_BMC_RECOVER='1', PYTHONDONTWRITEBYTECODE='1')
    for key in ('PYTHONPATH', 'PYTHONHOME'):
        env.pop(key, None)
    identity = runtime_identity(args.ikos.resolve())
    fixture = ROOT / 'analyzer/test/regression/concurrency/native-mechanisms.c'
    with tempfile.TemporaryDirectory(prefix='ikos-native-mechanisms-') as temp:
        work = Path(temp)
        for bits in ([args.bits] if args.bits else (32, 64)):
            for index, (name, prop, observed) in enumerate(CASES):
                source = work / 'case.c'
                source.write_text(f'#define CASE {index}\n' + fixture.read_text())
                report = work / 'report.json'
                report.unlink(missing_ok=True)
                cmd = runtime_command(args.ikos.resolve()) + [
                    '--analyses=race', '--concurrency=auto', '--partitioning=guards',
                    '--demote-race-to-unknown', '-m', str(bits), '--format=json',
                    '--report-file=' + str(report), '-o', str(work / 'output.db'), str(source)]
                run = subprocess.run(cmd, cwd=work, env=env, capture_output=True,
                                     text=True, timeout=30)
                output = run.stdout + run.stderr
                assert run.returncode == 0, (name, bits, run.returncode, output)
                verdict = classify(output, run.returncode, False)
                expected = ('race' if bits == 32 else 'safe') if prop == 'mixed' else prop
                forbidden = 'Safe' if expected == 'race' else 'Race Found'
                assert verdict != forbidden, (name, bits, expected, output)
                assert verdict == observed[bits == 64], (name, bits, verdict, output)
                data = json.loads(report.read_text())
                if verdict == 'Race Found':
                    checks = validate_report(data)
                    assert checks and all(c['status'] == 'ACCEPT' for c in checks), checks
                print(f'{bits} {name}: {verdict}', flush=True)
    verify_runtime(identity)


if __name__ == '__main__':
    main()
