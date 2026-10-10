#!/usr/bin/env python3
"""Check width/offset interference preserves shared-guard reachability."""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import subprocess


def main(args):
    args.output.mkdir(parents=True, exist_ok=False)
    rows = []
    for source in sorted(Path(__file__).parent.glob('*.c')):
        for bits in (32, 64):
            folder = args.output / f'{source.stem}-{bits}'
            folder.mkdir()
            command = [str(args.ikos.resolve()), '--analyses=race', '--concurrency=auto',
                       '--demote-race-to-unknown', '-m', str(bits), '--format=json',
                       '--report-file=' + str(folder / 'report.json'), '-o', str(folder / 'output.db'),
                       str(source.resolve())]
            proc = subprocess.run(command, cwd=folder, capture_output=True, text=True, timeout=30,
                                  env=dict(os.environ, IKOS_BMC_RECOVER='1', IKOS_BMC_DIAGNOSTICS='1'))
            (folder / 'run.log').write_text(proc.stdout + proc.stderr)
            verdict = next((s for s in proc.stdout.splitlines() if s.startswith('The program is')), 'NONE')
            assert proc.returncode == 0 and verdict != 'NONE', (source.name, bits, proc.returncode)
            if 'control' in source.stem:
                assert 'definitely UNSAFE' not in verdict, (source.name, bits, verdict)
            elif source.stem in ('low-byte-race', 'high-byte-race', 'renamed-helper-race'):
                assert verdict != 'The program is SAFE', (source.name, bits, verdict)
            else:
                assert verdict == 'The program is definitely UNSAFE', (source.name, bits, verdict)
            counts = Counter()
            for group in json.loads((folder / 'report.json').read_text())['reports']:
                counts.update(group.get('info', {}).get('bmc', {}).get('counts', {}))
            rows.append(dict(case=source.stem, bits=bits, verdict=verdict, counts=dict(counts)))
            print(source.stem, bits, verdict, flush=True)
    (args.output / 'summary.json').write_text(json.dumps(rows, indent=2)+'\n')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ikos', type=Path, default=Path(__file__).resolve().parents[3] / 'install/bin/ikos')
    p.add_argument('--output', type=Path, required=True)
    main(p.parse_args())
