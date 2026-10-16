#!/usr/bin/env python3
"""Check mutex-protected guard protocols without bounded safety inference."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from svcomp_race_test import classify, VERDICT_SAFE, VERDICT_UNKNOWN


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    args = parser.parse_args()
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/protected-protocol.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    cases = list(range(29)) + [30, 31, 32, 33, 35, 37, 40, 41, 42, 44]
    count = 0
    with tempfile.TemporaryDirectory(prefix='ikos-protected-protocol-') as temp:
        work = Path(temp)
        for wide in (False, True):
            for case in cases + ([38] if wide else []):
                text = fixture.replace('active', 'phase').replace('payload', 'shared_data')
                text = text.replace('producer', 'publish').replace('consumer', 'retire')
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') + text)
                report = work / 'report.json'
                proc = subprocess.run([str(args.ikos.resolve()), '--analyses=race',
                    '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                    '--bmc-deadline-ms=0', '--format=json', '--status-filter=*',
                    '--report-file=' + str(report), '--log=debug', '-m32',
                    '-o', str(work / 'output.db'), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                verdict = classify(proc.stdout, proc.returncode, False)
                expected = VERDICT_SAFE if case in (0, 30, 37, 38, 42) else VERDICT_UNKNOWN
                proofs = [r['info'] for r in json.loads(report.read_text())['reports']
                          if r['info'].get('protected_protocol_contract') == 'closed-two-root-protected-scalar-mutex-hb-v1'] if report.exists() else []
                assert proc.returncode == 0 and verdict == expected, (case, wide, verdict, proc.stdout[-5000:], proc.stderr)
                if expected == VERDICT_SAFE:
                    assert proofs, (case, wide)
                count += 1
                print(f'protected case={case}/wide={wide}: {verdict}; proofs={len(proofs)}', flush=True)
    print(f'PASS {count} protected protocol controls', flush=True)


if __name__ == '__main__':
    main()
