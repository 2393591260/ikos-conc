#!/usr/bin/env python3
"""Check serial child generations without BMC or a loop-unrolling safety rule."""
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
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/thread-lifecycle.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    cases = [0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 15, 16, 17, 18, 20, 21, 22, 25, 27, 29, 31, 33, 34, 36, 38, 39]
    with tempfile.TemporaryDirectory(prefix='ikos-lifecycle-') as temp:
        work = Path(temp)
        for wide in (False, True):
            for case in cases:
                text = fixture.replace('phase', 'stage_value').replace('payload', 'shared_data')
                text = text.replace('register_child', 'activate').replace('parent', 'middle')
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
                expected = VERDICT_SAFE if case in (0, 18, 36, 38) else VERDICT_UNKNOWN
                diagnostics = '\n'.join(line for line in proc.stdout.splitlines()
                                        if 'Thread lifecycle' in line or 'Summary' in line)
                assert proc.returncode == 0 and verdict == expected, (
                    case, wide, verdict, diagnostics, proc.stderr)
                proofs = [r['info'] for r in json.loads(report.read_text())['reports']
                          if r['info'].get('thread_lifecycle_contract') == 'serial-structured-thread-lifecycle-v1']
                if case in (0, 18, 36, 38):
                    assert proofs, (case, wide, diagnostics)
                print(f'lifecycle case={case}/wide={wide}: {verdict}; proofs={len(proofs)}', flush=True)


if __name__ == '__main__':
    main()
