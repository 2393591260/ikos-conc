#!/usr/bin/env python3
"""Check that positive first-cell freezing and scalar correlation survive."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

from ikos_runtime import runtime_command, runtime_identity, verify_runtime

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    args = parser.parse_args()
    entry = args.ikos.resolve()
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/freeze-array.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    identity = runtime_identity(entry)
    command = runtime_command(entry)
    count = 0
    with tempfile.TemporaryDirectory(prefix='ikos-freeze-array-') as temp:
        for wide in (False, True):
            for scalar in (False, True):
                for case in range(4 if not scalar else 3):
                    work = Path(temp) / f'{wide}-{scalar}-{case}'
                    work.mkdir()
                    source = work / 'control.c'
                    source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') +
                                      ('#define SCALAR\n' if scalar else '') + fixture)
                    ir = work / 'ir'
                    ir.mkdir()
                    proc = subprocess.run(command + ['--analyses=race', '--concurrency=auto',
                        '--partitioning=guards', '--demote-race-to-unknown', '--bmc-deadline-ms=0',
                        '--save-temps', '--temp-dir', str(ir), '--display-ar', '-m32',
                        '-o', str(work / 'output.db'), str(source)], env=env, text=True,
                        capture_output=True, timeout=30)
                    assert proc.returncode == 0, (wide, scalar, case, proc.stdout, proc.stderr)
                    safe = case == 2 or (scalar and case == 1)
                    assert ('The program is SAFE' in proc.stdout) == safe, (wide, scalar, case, proc.stdout)
                    raw = ir / 'control.bc'
                    pp = subprocess.check_output([str(entry.with_name('ikos-pp')), '-opt=basic',
                        '-freeze-undef', '-S', str(raw)], env=env, text=True, timeout=30)
                    arrays = re.findall(r'alloca i(?:32|64), i(\d+) (%[-\w.]+)', pp)
                    if not scalar:
                        assert arrays, (wide, case, pp)
                        for bits, value in arrays:
                            assert f'icmp ne i{bits} {value}, 0' in pp, (wide, case, pp)
                        assert 'allocate' in proc.stdout, (wide, case, proc.stdout)
                    count += 1
                    print(f'freeze wide={wide}/scalar={scalar}/case={case}: ' +
                          ('SAFE' if safe else 'UNKNOWN') + f'; array-counts={arrays}', flush=True)
    verify_runtime(identity)
    print(f'PASS {count} freeze value/continuation controls', flush=True)


if __name__ == '__main__':
    main()
