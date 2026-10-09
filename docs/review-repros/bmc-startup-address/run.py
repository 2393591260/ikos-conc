#!/usr/bin/env python3
"""Check executed startup indices and correlated shared initial values."""
import argparse
import json
import os
from pathlib import Path
import subprocess


def main(args):
    args.output.mkdir(parents=True, exist_ok=False)
    observations = []
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
            race = 'control' not in source.stem
            assert result.returncode == 0, (tag, result.returncode)
            assert ('definitely UNSAFE' in verdict) == race, (tag, verdict)
            offsets = []
            for group in json.loads(report.read_text())['reports']:
                for candidate in group.get('info', {}).get('bmc', {}).get('candidates', []):
                    replay = candidate.get('replay')
                    if not replay:
                        continue
                    for trace in replay['traces']:
                        if trace['id'] != 'startup':
                            continue
                        obj = 'cells' if source.stem.startswith('helper') else 'seed'
                        offsets = [int(step['encoded_address']['offset'])
                                   for step in trace['steps']
                                   if step.get('encoded_address', {}).get('object', {}).get('id') == obj]
            if race:
                expected = [8, 4, 0] if source.stem.startswith('helper') else [0, 4, 8]
                assert offsets == expected, (tag, offsets)
            observations.append(dict(case=source.stem, bits=bits, verdict=verdict,
                                     startup_offsets=offsets))
            print(tag, verdict, offsets, flush=True)
    (args.output / 'summary.json').write_text(json.dumps(observations, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path,
                        default=Path(__file__).resolve().parents[3] / 'install/bin/ikos')
    parser.add_argument('--output', type=Path, required=True)
    main(parser.parse_args())
