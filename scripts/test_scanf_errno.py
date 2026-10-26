#!/usr/bin/env python3
"""Retain races on formatted-input error paths through cached errno pointers."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from ikos_runtime import runtime_command, runtime_identity, verify_runtime

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--bits', type=int, choices=(32, 64), default=32)
    args = parser.parse_args()
    entry = args.ikos.resolve()
    env = {k: v for k, v in os.environ.items()
           if not k.startswith('IKOS_') and k not in {'PYTHONPATH', 'PYTHONHOME'}}
    identity = runtime_identity(entry)
    command = runtime_command(entry)
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/scanf-errno.c').read_text()
    count = 0
    with tempfile.TemporaryDirectory(prefix='ikos-scanf-errno-') as temp:
        work = Path(temp)
        for kind in range(3):
            for mapped in (False, True):
                for wide in (False, True):
                    for case in range(3):
                        source = work / 'renamed.c'
                        source.write_text(f'#define KIND {kind}\n#define CASE {case}\n' +
                            ('#define MAPPED\n' if mapped else '') +
                            ('#define WIDE\n' if wide else '') + fixture)
                        proc = subprocess.run(command + ['--analyses=race', '--concurrency=auto',
                            '--partitioning=guards', '--demote-race-to-unknown', '--bmc-deadline-ms=0',
                            f'-m{args.bits}', '-o', str(work / 'out.db'), str(source)], env=env, text=True,
                            capture_output=True, timeout=30)
                        assert proc.returncode == 0, (kind, mapped, wide, case, proc.stdout, proc.stderr)
                        safe = 'The program is SAFE' in proc.stdout
                        assert safe == (case == 2), (kind, mapped, wide, case, proc.stdout, proc.stderr)
                        count += 1
                        print(f'scanf-errno kind={kind}/mapped={mapped}/wide={wide}/case={case}: SAFE={safe}', flush=True)
    verify_runtime(identity)
    print(f'PASS bits={args.bits} {count} scanf-errno controls', flush=True)


if __name__ == '__main__':
    main()
