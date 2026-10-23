#!/usr/bin/env python3
"""Exercise indexed heap ownership with BMC disabled, including alias escapes."""
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
    parser.add_argument('--bits', type=int, choices=(32, 64), default=32)
    args = parser.parse_args()
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/region-ownership.c').read_text()
    positives = {0, 20, 21, 27}
    complete_families = positives | {25, 26, 28, 29, 39}
    cases = list(range(17)) + [18, 20, 21, 22, 25, 26, 27, 28, 29, 30, 31, 33, 34, 35, 36, 37, 39, 40, 41]
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    with tempfile.TemporaryDirectory(prefix='ikos-region-ownership-') as temp:
        work = Path(temp)
        for wide in (False, True):
            for case in cases:
                source = work / 'renamed.c'
                text = fixture.replace('construct', 'build_element').replace('worker', 'entry')
                text = text.replace('guards', 'locks').replace('roots', 'heads')
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') + text)
                report = work / 'report.json'
                result = subprocess.run([str(args.ikos.resolve()), '--analyses=race',
                    '--concurrency=auto', '--demote-race-to-unknown', '--bmc-deadline-ms=0',
                    '--format=json', '--status-filter=*', '--report-file=' + str(report),
                    '--log=debug', f'-m{args.bits}', '-o', str(work / 'output.db'), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                verdict = classify(result.stdout, result.returncode, False)
                expected = VERDICT_SAFE if case in positives else VERDICT_UNKNOWN
                assert result.returncode == 0 and verdict == expected, (
                    case, wide, verdict, result.stdout, result.stderr)
                complete = 'Region ownership: complete family and all writer transfers certified' in result.stdout
                assert complete == (case in complete_families), (case, wide, result.stdout, result.stderr)
                proofs = [r['info'] for r in json.loads(report.read_text())['reports']
                          if r['info'].get('ownership_contract') == 'indexed-live-heap-owner-v1']
                if case in positives:
                    assert proofs and all(p['root_array'] == 'heads' and p['mutex_array'] == 'locks'
                                          for p in proofs), proofs
                if case not in positives:
                    assert not proofs, (case, wide, proofs)
                failures = [line for line in result.stdout.splitlines() if 'Region ownership unsupported:' in line]
                print(f'owner case={case}/wide={wide}: {verdict}; certified={complete}; '
                      f'proofs={len(proofs)}; {failures}', flush=True)


if __name__ == '__main__':
    main()
