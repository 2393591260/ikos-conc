#!/usr/bin/env python3
"""Exercise stable array slices with BMC disabled and all byte widths checked."""
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
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/array-slices.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    cases = list(range(14)) + [15, 18, 19, 20, 23, 30, 31, 32, 33, 34]
    with tempfile.TemporaryDirectory(prefix='ikos-array-slices-') as temp:
        work = Path(temp)
        for wide in (False, True):
            for case in cases:
                text = fixture.replace('make', 'allocate_values').replace('cut', 'division')
                text = text.replace('output', 'buffer').replace('first', 'left_part')
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
                expected = VERDICT_SAFE if case in (0, 18, 23) else VERDICT_UNKNOWN
                diagnostics = '\n'.join(line for line in proc.stdout.splitlines()
                                        if 'Array slices' in line or 'Summary' in line)
                assert proc.returncode == 0 and verdict == expected, (
                    case, wide, verdict, diagnostics, proc.stderr)
                proofs = [r['info'] for r in json.loads(report.read_text())['reports']
                          if r['info'].get('array_slices_contract') == 'closed-stable-flat-array-slices-v1']
                if case in (0, 23):
                    assert proofs, (case, wide, diagnostics)
                print(f'slices case={case}/wide={wide}: {verdict}; proofs={len(proofs)}', flush=True)


if __name__ == '__main__':
    main()
