#!/usr/bin/env python3
"""Check actual pointer returns, repeated calls, shared guards and lifetime gates."""
import argparse
import json
import os
from pathlib import Path
import subprocess


def main(args):
    args.output.mkdir(parents=True, exist_ok=False)
    here = Path(__file__).resolve().parent
    sources = sorted(here.glob('*.c')) + [here.parent / 'structural-offset/closed-overlap-race.c']
    rows = []
    for source in sources:
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
            if source.stem.endswith('-race'):
                assert 'definitely UNSAFE' in verdict, (tag, verdict)
            else:
                assert 'definitely UNSAFE' not in verdict, (tag, verdict)
            counts = {}
            for group in json.loads(report.read_text())['reports']:
                for key, value in group.get('info', {}).get('bmc', {}).get('counts', {}).items():
                    counts[key] = counts.get(key, 0) + value
            rows.append(dict(case=source.stem, bits=bits, verdict=verdict, counts=counts))
            print(tag, verdict, counts, flush=True)
    (args.output / 'summary.json').write_text(json.dumps(rows, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path,
                        default=Path(__file__).resolve().parents[3] / 'install/bin/ikos')
    parser.add_argument('--output', type=Path, required=True)
    main(parser.parse_args())
