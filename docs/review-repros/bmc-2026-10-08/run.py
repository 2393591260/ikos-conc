#!/usr/bin/env python3
"""Reproduce the review observations with isolated outputs; no source edits."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path,
                        default=here.parents[2] / 'install/bin/ikos')
    parser.add_argument('--bits', choices=['32', '64'], default='32')
    args = parser.parse_args()
    observations = []
    with tempfile.TemporaryDirectory(prefix='ikos-bmc-review-') as tmp:
        out = Path(tmp)
        for source in sorted(here.glob('*.c')):
            modes = ['demote', 'bmc'] if source.stem == 'exclusive-flag' else ['default', 'bmc']
            for mode in modes:
                env = dict(os.environ)
                env.pop('IKOS_BMC_RECOVER', None)
                options = []
                if mode != 'default':
                    options.append('--demote-race-to-unknown')
                if mode == 'bmc':
                    env['IKOS_BMC_RECOVER'] = '1'
                command = [str(args.ikos.resolve()), '--analyses=race',
                           '--concurrency=auto', '-m', args.bits, *options,
                           '-o', str(out / 'output.db'), str(source)]
                try:
                    result = subprocess.run(command, cwd=out, env=env,
                                            capture_output=True, text=True, timeout=30)
                    verdict = next((line for line in result.stdout.splitlines()
                                    if line.startswith('The program is')), 'NO VERDICT')
                    exit_code = result.returncode
                except subprocess.TimeoutExpired:
                    verdict, exit_code = 'TIMEOUT', None
                observations.append(dict(source=source.name, mode=mode,
                                         data_model='ILP32' if args.bits == '32' else 'LP64',
                                         exit_code=exit_code, verdict=verdict))
                print(f'{source.name} [{mode}]: {verdict}', flush=True)
    print(json.dumps(observations, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
