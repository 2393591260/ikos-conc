#!/usr/bin/env python3
"""Check that one abstract lock location is backed by one concrete allocation."""
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
            for mode in ('default', 'bmc'):
                folder = args.output/f'{source.stem}-{bits}-{mode}'; folder.mkdir()
                env = dict(os.environ); env.pop('IKOS_BMC_RECOVER', None)
                flags = []
                if mode == 'bmc':
                    env['IKOS_BMC_RECOVER'] = '1'; flags = ['--demote-race-to-unknown']
                proc = subprocess.run([str(args.ikos.resolve()), '--analyses=race', '--concurrency=auto',
                    '-m', str(bits), *flags, str(source.resolve())], cwd=folder, env=env,
                    capture_output=True, text=True, timeout=30)
                (folder/'run.log').write_text(proc.stdout+proc.stderr)
                verdict = next((s for s in proc.stdout.splitlines() if s.startswith('The program is')), 'NONE')
                assert proc.returncode == 0 and verdict != 'NONE', (source.name, bits, mode)
                if 'control' in source.stem:
                    assert verdict == 'The program is SAFE', (source.name, bits, mode, verdict)
                else:
                    assert verdict != 'The program is SAFE', (source.name, bits, mode, verdict)
                rows.append(dict(case=source.stem, bits=bits, mode=mode, verdict=verdict))
                print(source.stem, bits, mode, verdict, flush=True)
    (args.output/'summary.json').write_text(json.dumps(rows, indent=2)+'\n')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ikos', type=Path, default=Path(__file__).resolve().parents[3]/'install/bin/ikos')
    p.add_argument('--output', type=Path, required=True)
    main(p.parse_args())
