#!/usr/bin/env python3
"""Check TLS data and lock identity without treating a declaration as an instance."""
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
                folder = args.output / f'{source.stem}-{bits}-{mode}'
                folder.mkdir()
                env = dict(os.environ); env.pop('IKOS_BMC_RECOVER', None)
                options = []
                if mode == 'bmc':
                    env['IKOS_BMC_RECOVER'] = '1'
                    options = ['--demote-race-to-unknown']
                command = [str(args.ikos.resolve()), '--analyses=race', '--concurrency=auto',
                           '-m', str(bits), *options, str(source.resolve())]
                proc = subprocess.run(command, cwd=folder, env=env, capture_output=True,
                                      text=True, timeout=30)
                (folder/'run.log').write_text(proc.stdout+proc.stderr)
                verdict = next((s for s in proc.stdout.splitlines() if s.startswith('The program is')), 'NONE')
                assert proc.returncode == 0 and verdict != 'NONE', (source.name, bits, mode)
                if source.stem == 'global-lock-control':
                    assert verdict == 'The program is SAFE', (source.name, bits, mode, verdict)
                elif 'control' in source.stem:
                    if mode == 'bmc':
                        assert 'definitely UNSAFE' not in verdict, (source.name, bits, verdict)
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
