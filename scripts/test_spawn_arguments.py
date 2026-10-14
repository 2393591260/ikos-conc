#!/usr/bin/env python3
"""Check that NULL spawn arguments cannot erase reachable worker accesses."""
import argparse
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
    fixture = ROOT / 'analyzer/test/regression/bmc-values/spawn-null.c'
    with tempfile.TemporaryDirectory(prefix='ikos-spawn-arguments-') as temp:
        work = Path(temp)
        for wide in (False, True):
            for case in range(6):
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n' +
                                  ('#define WIDE\n' if wide else '') +
                                  fixture.read_text().replace('worker', 'entry'))
                result = subprocess.run([str(args.ikos.resolve()), '--analyses=race',
                    '--concurrency=auto', '--demote-race-to-unknown',
                    '--bmc-deadline-ms=0', '-m32', '-o', str(work / 'output.db'),
                    str(source)], capture_output=True, text=True, timeout=30,
                    env=dict(os.environ, IKOS_BMC_RECOVER='1'))
                actual = classify(result.stdout, result.returncode, False)
                if case < 4:
                    expected = (VERDICT_UNKNOWN,)
                elif case == 4:
                    expected = (VERDICT_SAFE,)
                else:
                    expected = (VERDICT_SAFE, VERDICT_UNKNOWN)
                assert actual in expected, (case, wide, actual, result.stdout, result.stderr)
                if case != 4:
                    assert (": In function 'entry':" in result.stdout) == (case < 4), result.stdout
                print(f'{case}/wide={wide}: {actual}', flush=True)
    print('12 spawn argument checks passed (BMC disabled)')


if __name__ == '__main__':
    main()
