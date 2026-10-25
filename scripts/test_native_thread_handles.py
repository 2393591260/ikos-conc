#!/usr/bin/env python3
"""Exercise native array values/effects without guessing join completion."""
import argparse
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--bits', type=int, choices=(32, 64), default=32)
    args = parser.parse_args()
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/native-handles.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    unsafe = {1, 2, 3, 4, 5, 6, 7, 8, 9, 15, 16, 17, 18}
    certified = {0, 1, 2, 3, 17}
    with tempfile.TemporaryDirectory(prefix='ikos-native-handles-') as temp:
        work = Path(temp)
        for wide in (False, True):
            for case in range(20):
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') + fixture)
                dbpath = work / 'output.db'
                result = subprocess.run([str(args.ikos.resolve()), '--analyses=race',
                    '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                    '--bmc-deadline-ms=0', '--log=debug', '-m' + str(args.bits), '-o', str(dbpath), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                output = result.stdout + result.stderr
                assert result.returncode == 0, (case, wide, output)
                safe = 'The program is SAFE' in output
                preflight = 'Native pthread handles: complete output-effect preflight' in output
                if case == 0:
                    assert safe and preflight, (case, wide, output)
                if case in unsafe:
                    assert not safe, (case, wide, output)
                assert preflight == (case in certified), (case, wide, output)
                with sqlite3.connect(dbpath) as db:
                    reasons = db.execute("SELECT info FROM checks WHERE info LIKE '%unknown_reason%'").fetchall()
                if case in certified:
                    assert all('synthetic pthread handle representation not modeled' not in text for text, in reasons), (case, wide, reasons)
                print(f'native case={case}/wide={wide}: safe={safe}, certified={preflight}', flush=True)
        for count in (2, 10000):
            source = work / 'renamed.c'
            source.write_text(f'#define COUNT {count}\n' + fixture)
            proc = subprocess.run([str(args.ikos.resolve()), '--analyses=race', '--concurrency=auto',
                '--partitioning=guards', '--demote-race-to-unknown', '--bmc-deadline-ms=0', '--log=debug',
                '-m' + str(args.bits), '-o', str(work / 'output.db'), str(source)], env=env, capture_output=True, text=True, timeout=30)
            output = proc.stdout + proc.stderr
            assert proc.returncode == 0 and 'The program is SAFE' in output and 'Native pthread handles: complete output-effect preflight' in output, (count, output)
            print(f'native count={count}: SAFE with effects certificate', flush=True)
        for define in ('ASSERT_MODE 1', 'ASSERT_MODE 2', 'ASSERT_MODE 3',
                       'ASSERT_MODE 4', 'FAKE_CREATE', 'pthread_create fake_pthread_create'):
            source = work / 'renamed.c'
            source.write_text(f'#define {define}\n' + fixture)
            proc = subprocess.run([str(args.ikos.resolve()), '--analyses=race', '--concurrency=auto',
                '--partitioning=guards', '--demote-race-to-unknown', '--bmc-deadline-ms=0', '--log=debug',
                '-m' + str(args.bits), '-o', str(work / 'output.db'), str(source)], env=env, capture_output=True, text=True, timeout=30)
            output = proc.stdout + proc.stderr
            preflight = 'Native pthread handles: complete output-effect preflight' in output
            # A same-name definition is analyzed normally; this empty body has
            # no effects, independently of the library declaration contract.
            admitted = define in ('ASSERT_MODE 1', 'ASSERT_MODE 3')
            assert proc.returncode == 0 and preflight == admitted, (define, output)
            if admitted:
                assert 'The program is SAFE' in output, output
            print(f'native API control={define}: certified={preflight}', flush=True)


        # Corrupt the actual storage/uses while retaining the real pthread ABI.
        native = "pthread_t handles[COUNT];"
        variants = {
            "wrong-width": fixture.replace(native,
                ("unsigned long long" if args.bits == 32 else "unsigned int") +
                " handles[COUNT];").replace("&handles[", "(pthread_t *)&handles["),
            "partial-write": fixture.replace(
                "#if CASE == 3\n  pthread_t old",
                "((unsigned char *)handles)[sizeof(pthread_t)-1] = 1;\n"
                "#if CASE == 3\n  pthread_t old"),
            "high-bits-observer": fixture.replace(
                "#if CASE == 4\n",
                "if (((unsigned long long)handles[0] >> 32) != 0) data = 2;\n"
                "#if CASE == 4\n"),
        }
        for name, program in variants.items():
            source = work / 'renamed.c'
            source.write_text(program)
            proc = subprocess.run([str(args.ikos.resolve()), '--analyses=race',
                '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                '--bmc-deadline-ms=0', '--log=debug', '-m' + str(args.bits),
                '-o', str(work / 'output.db'), str(source)],
                env=env, capture_output=True, text=True, timeout=30)
            output = proc.stdout + proc.stderr
            assert proc.returncode == 0, (name, args.bits, output)
            assert 'Native pthread handles: complete output-effect preflight' not in output, (
                name, args.bits, output)
            print(f'native storage control={name}/bits={args.bits}: rejected', flush=True)


if __name__ == '__main__':
    main()
