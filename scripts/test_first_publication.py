#!/usr/bin/env python3
"""Check birth ordering separately from ordinary shared-data race exclusion."""
import argparse
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    args = parser.parse_args()
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/first-publication.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    positives = {0, 16, 20}
    with tempfile.TemporaryDirectory(prefix='ikos-first-publication-') as temp:
        work = Path(temp)
        for wide in (False, True):
            for case in range(22):
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') + fixture)
                dbpath = work / 'output.db'
                result = subprocess.run([str(args.ikos.resolve()), '--analyses=race',
                    '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                    '--bmc-deadline-ms=0', '--log=debug', '-m32', '-o', str(dbpath), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                output = result.stdout + result.stderr
                assert result.returncode == 0, (case, wide, output)
                safe = 'The program is SAFE' in output
                with sqlite3.connect(dbpath) as db:
                    proofs = [json.loads(text) for text, in db.execute(
                        "SELECT info FROM checks WHERE info LIKE '%closed-first-publication-v1%'")]
                if case in positives:
                    assert safe and proofs, (case, wide, output, proofs)
                else:
                    assert not safe, (case, wide, output)
                if case == 5:
                    # Good/Good is deliberately not an exclusion, even if some
                    # birth pairs received a sound order certificate.
                    assert proofs, (case, wide, output)
                print(f'first publication={case}/wide={wide}: safe={safe}, proofs={len(proofs)}', flush=True)

        for wide in (False, True):
            source = work / 'permuted.c'
            renamed = fixture
            for old, new in (('initialize', 'construct'), ('insert', 'publish'),
                             ('take', 'traverse'), ('head', 'anchor'), ('next', 'link_field')):
                renamed = renamed.replace(old, new)
            source.write_text('#define COUNT 10000\n' + ('#define WIDE\n' if wide else '') + renamed)
            proc = subprocess.run([str(args.ikos.resolve()), '--analyses=race', '--concurrency=auto',
                '--partitioning=guards', '--demote-race-to-unknown', '--bmc-deadline-ms=0', '-m32',
                '-o', str(work / 'output.db'), str(source)], env=env, capture_output=True, text=True, timeout=30)
            assert proc.returncode == 0 and 'The program is SAFE' in proc.stdout + proc.stderr, (wide, proc.stdout, proc.stderr)
            print(f'renamed publication/10000/wide={wide}: SAFE', flush=True)

        fixture = (ROOT / 'analyzer/test/regression/bmc-values/first-publication-arrays.c').read_text()
        for wide in (False, True):
            for case in (*range(8), 9):
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') + fixture)
                dbpath = work / 'output.db'
                result = subprocess.run([str(args.ikos.resolve()), '--analyses=race',
                    '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                    '--bmc-deadline-ms=0', '--log=debug', '-m32', '-o', str(dbpath), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                output = result.stdout + result.stderr
                assert result.returncode == 0, (case, wide, output)
                with sqlite3.connect(dbpath) as db:
                    proofs = list(db.execute("SELECT info FROM checks WHERE info LIKE '%closed-first-publication-v1%'"))
                safe = 'The program is SAFE' in output
                if case == 0:
                    assert safe and proofs, (case, wide, output, proofs)
                elif case == 9:
                    assert not safe and not proofs, (case, wide, output)
                else:
                    assert not safe, (case, wide, output)
                print(f'array publication={case}/wide={wide}: safe={safe}, proofs={len(proofs)}', flush=True)


if __name__ == '__main__':
    main()
