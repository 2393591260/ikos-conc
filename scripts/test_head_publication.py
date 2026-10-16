#!/usr/bin/env python3
"""Check heap-family conditional ownership independently of BMC confirmation."""
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
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/head-publication.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    with tempfile.TemporaryDirectory(prefix='ikos-head-publication-') as temp:
        work = Path(temp)
        for wide in (False, True):
            for case in range(21):
                text = fixture.replace('make', 'build_node').replace('worker', 'entry')
                text = text.replace('heads', 'roots').replace('guards', 'locks')
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') + text)
                report = work / 'report.json'
                result = subprocess.run([str(args.ikos.resolve()), '--analyses=race',
                    '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                    '--bmc-deadline-ms=0', '--format=json', '--status-filter=*',
                    '--report-file=' + str(report), '--log=debug', '-m32',
                    '-o', str(work / 'output.db'), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                verdict = classify(result.stdout, result.returncode, False)
                expected = VERDICT_SAFE if case in (0, 17, 18) else VERDICT_UNKNOWN
                assert result.returncode == 0 and verdict == expected, (
                    case, wide, verdict, result.stdout, result.stderr)
                proofs = [r['info'] for r in json.loads(report.read_text())['reports']
                          if r['info'].get('ownership_contract') == 'indexed-head-publication-owner-v1']
                if expected == VERDICT_SAFE:
                    assert proofs and all(p['root_array'] == 'roots' and
                                          p['mutex_array'] == 'locks' for p in proofs), proofs
                print(f'head case={case}/wide={wide}: {verdict}; proofs={len(proofs)}', flush=True)


if __name__ == '__main__':
    main()
