#!/usr/bin/env python3
"""Check creation cardinality through loops, recursion, helpers and escapes."""
import argparse
import json
import os
from pathlib import Path
import subprocess


def main(args):
    args.output.mkdir(parents=True, exist_ok=False)
    rows = []
    for source in sorted(Path(__file__).parent.glob('*.c')):
        for bits in (32, 64):
            tag = source.stem + '-' + str(bits)
            report = args.output / (tag + '.json')
            command = [str(args.ikos.resolve()), '--analyses=race', '--concurrency=auto',
                       '--demote-race-to-unknown', '-m', str(bits), '--format=json',
                       '--report-file=' + str(report), '-o',
                       str(args.output / (tag + '.db')), str(source.resolve())]
            result = subprocess.run(command, capture_output=True, text=True, timeout=30,
                                    env=dict(os.environ, IKOS_BMC_RECOVER='1',
                                             IKOS_BMC_DIAGNOSTICS='1'))
            (args.output / (tag + '.log')).write_text(result.stdout + result.stderr)
            verdict = next((s for s in result.stdout.splitlines()
                            if s.startswith('The program is')), 'NONE')
            assert result.returncode == 0 and verdict != 'NONE', (tag, result.returncode)
            if 'control' in source.stem:
                assert 'definitely UNSAFE' not in verdict, (tag, verdict)
                if source.stem == 'once-helper-join-control':
                    assert verdict == 'The program is SAFE', (tag, verdict)
            else:
                assert verdict != 'The program is SAFE', (tag, verdict)
                if source.stem != 'escaped-helper-race':
                    assert 'definitely UNSAFE' in verdict, (tag, verdict)
            rows.append(dict(case=source.stem, bits=bits, verdict=verdict))
            print(tag, verdict, flush=True)
    (args.output / 'summary.json').write_text(json.dumps(rows, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path,
                        default=Path(__file__).resolve().parents[3] / 'install/bin/ikos')
    parser.add_argument('--output', type=Path, required=True)
    main(parser.parse_args())
