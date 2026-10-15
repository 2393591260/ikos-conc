#!/usr/bin/env python3
"""Check initialized heap fields without BMC proving abstract safety."""
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
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/heap-fields.c').read_text()
    with tempfile.TemporaryDirectory(prefix='ikos-heap-fields-') as temp:
        work = Path(temp)
        for wide in (False, True):
            for case in (0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15, 16, 17, 18):
                source = work / 'renamed.c'
                text = fixture.replace('first', 'alpha').replace('second', 'beta').replace('worker', 'entry')
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') + text)
                result = subprocess.run([str(args.ikos.resolve()), '--analyses=race',
                    '--concurrency=auto', '--demote-race-to-unknown', '--bmc-deadline-ms=0',
                    '-m32', '-o', str(work / 'output.db'), str(source)],
                    env=dict(os.environ, IKOS_BMC_RECOVER='1'), capture_output=True,
                    text=True, timeout=30)
                verdict = classify(result.stdout, result.returncode, False)
                expected = (VERDICT_SAFE,) if case in (0, 11, 12) else (
                    (VERDICT_SAFE, VERDICT_UNKNOWN) if case in (8, 17, 18) else (VERDICT_UNKNOWN,))
                assert verdict in expected, (case, wide, verdict, result.stdout, result.stderr)
                print(f'{case}/wide={wide}: {verdict}', flush=True)


if __name__ == '__main__':
    main()
