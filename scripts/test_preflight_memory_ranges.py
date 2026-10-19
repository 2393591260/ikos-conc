#!/usr/bin/env python3
"""Check range certificates against memory and control dependencies."""
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
    args = parser.parse_args()
    entry = args.ikos.resolve()
    identity = runtime_identity(entry)
    command = runtime_command(entry)
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/preflight-memory-ranges.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    with tempfile.TemporaryDirectory(prefix='ikos-preflight-memory-') as temp:
        work = Path(temp)
        for wide in (False, True):
            for case in range(20):
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') +
                                  fixture.replace('use_range', 'inspect_leaf').replace('integer_guard', 'condition_value'))
                result = subprocess.run(command + ['--analyses=race', '--concurrency=auto',
                    '--partitioning=guards', '--demote-race-to-unknown', '--bmc-deadline-ms=0',
                    '--log=debug', '-m32', '-o', str(work / 'output.db'), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                output = result.stdout + result.stderr
                assert result.returncode == 0, (case, wide, output)
                certified = 'mutex-array preflight certified' in output
                if case % 2 == 0 or case == 19:
                    # These are range-pressure controls, not safe-program labels.
                    assert not certified, (case, wide, output)
                elif case not in {11}:
                    assert certified, (case, wide, output)
                # The guard communication is unsynchronized even with valid leaves.
                assert 'The program is SAFE' not in output, (case, wide, output)
                print(f'CASE={case}/WIDE={int(wide)} certified={certified} PASS', flush=True)
    verify_runtime(identity)

if __name__ == '__main__':
    main()
