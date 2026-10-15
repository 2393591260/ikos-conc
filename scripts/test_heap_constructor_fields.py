#!/usr/bin/env python3
"""Check per-instance constructor coverage, with BMC disabled."""
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
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/constructor-fields.c').read_text()
    with tempfile.TemporaryDirectory(prefix='ikos-constructor-fields-') as temp:
        work = Path(temp)
        for wide in (False, True):
            for case in range(14):
                source = work / 'renamed.c'
                text = fixture.replace('make_node', 'construct').replace('worker', 'entry')
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') + text)
                result = subprocess.run([str(args.ikos.resolve()), '--analyses=race',
                    '--concurrency=auto', '--demote-race-to-unknown', '--bmc-deadline-ms=0',
                    '-m32', '-o', str(work / 'output.db'), str(source)],
                    env=dict(os.environ, IKOS_BMC_RECOVER='1'), capture_output=True,
                    text=True, timeout=30)
                verdict = classify(result.stdout, result.returncode, False)
                expected = (VERDICT_SAFE,) if case == 0 else (VERDICT_UNKNOWN,)
                assert verdict in expected, (case, wide, verdict, result.stdout, result.stderr)
                print(f'{case}/wide={wide}: {verdict}', flush=True)


if __name__ == '__main__':
    main()
