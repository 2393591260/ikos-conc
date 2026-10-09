#!/usr/bin/env python3
"""Memory-using stdio must not hide races; conservative warnings are not FALSE."""
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
                tag = f'{source.stem}-{bits}-{mode}'
                command = [str(args.ikos.resolve()), '--analyses=race', '--concurrency=auto',
                           '-m', str(bits), '-o', str(args.output / (tag + '.db')),
                           str(source.resolve())]
                env = dict(os.environ)
                env.pop('IKOS_BMC_RECOVER', None)
                if mode == 'bmc':
                    command.insert(1, '--demote-race-to-unknown')
                    env['IKOS_BMC_RECOVER'] = '1'
                result = subprocess.run(command, env=env, capture_output=True,
                                        text=True, timeout=30)
                (args.output / (tag + '.log')).write_text(result.stdout + result.stderr)
                verdict = next((s for s in result.stdout.splitlines()
                                if s.startswith('The program is')), 'NONE')
                assert result.returncode == 0, (tag, result.returncode)
                if 'control' in source.stem:
                    assert verdict == 'The program is SAFE', (tag, verdict)
                else:
                    assert verdict not in ('NONE', 'The program is SAFE'), (tag, verdict)
                    if mode == 'bmc':
                        assert verdict == 'The program is potentially UNSAFE', (tag, verdict)
                rows.append(dict(case=source.stem, bits=bits, mode=mode, verdict=verdict))
                print(tag, verdict, flush=True)
    (args.output / 'summary.json').write_text(json.dumps(rows, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path,
                        default=Path(__file__).resolve().parents[3] / 'install/bin/ikos')
    parser.add_argument('--output', type=Path, required=True)
    main(parser.parse_args())
