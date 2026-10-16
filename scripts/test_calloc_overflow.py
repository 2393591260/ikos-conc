#!/usr/bin/env python3
"""calloc size overflow must preserve the caller's failure continuation."""
import argparse
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    args = parser.parse_args()
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/calloc-overflow.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    with tempfile.TemporaryDirectory(prefix='ikos-calloc-overflow-') as temp:
        work = Path(temp)
        for wide in (False, True):
            for case in range(16):
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') + fixture)
                proc = subprocess.run([str(args.ikos.resolve()), '--analyses=race',
                    '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                    '--bmc-deadline-ms=0', '-m32', '-o', str(work / 'output.db'), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                output = proc.stdout + proc.stderr
                safe = 'The program is SAFE' in output
                assert proc.returncode == 0 and safe == (case in {6, 8, 13}), (case, wide, output)
                if case == 12:
                    with sqlite3.connect(work / 'output.db') as db:
                        locations = [json.loads(row[0]).get('memory_location')
                                     for row in db.execute('SELECT info FROM checks')]
                    assert 'GlobalMemoryLocation{@shared}' in locations, (wide, output, locations)
                print(f'calloc overflow={case}/wide={wide}: safe={safe}', flush=True)


if __name__ == '__main__':
    main()
