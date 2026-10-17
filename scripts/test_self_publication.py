#!/usr/bin/env python3
"""Single state publication, exhaustive guarded arguments and dynamic gate phases."""
import argparse
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile

from ikos_runtime import runtime_command, runtime_identity, verify_runtime

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--case', type=int, choices=range(31), action='append',
                        help='Run only the selected case (repeatable).')
    args = parser.parse_args()
    identity = runtime_identity(args.ikos.resolve())
    command = runtime_command(args.ikos.resolve())
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/self-publication.c').read_text()
    target_lines = {i for i, line in enumerate(fixture.splitlines(), 1) if line.strip() == 'datum after = state;'}
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    with tempfile.TemporaryDirectory(prefix='ikos-self-publication-') as temporary:
        work = Path(temporary)
        for wide, opt in ((0, 0), (0, 1), (1, 0), (1, 1)):
            for case in (args.case if args.case is not None else range(31)):
                source = work / 'renamed.c'
                definitions = f'#define CASE {case}\n#define WIDE {wide}\n#define OPT {opt}\n'
                if case in {27, 28}:
                    definitions += '#define LENGTH 64\n#define STEP 1\n'
                source.write_text(definitions + '#line 1\n' + fixture)
                proc = subprocess.run(command + ['--analyses=race',
                    '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                    '--bmc-deadline-ms=0', '--log=debug', '-m32', '-o', str(work / 'output.db'), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                output = proc.stdout + proc.stderr
                assert proc.returncode == 0, (case, wide, opt, output)
                with sqlite3.connect(work / 'output.db') as db:
                    infos = [json.loads(r[0]) for r in db.execute('SELECT info FROM checks')]
                proofs = [r for r in infos if r.get('self_publication_contract')]
                positive = case in {0, 7, 21, 24, 26, 27, 30} or (opt and case in {6, 18})
                target_proofs = [r for r in proofs if target_lines.intersection((r.get('access_a_line'), r.get('access_b_line')))]
                assert bool(target_proofs) == positive, (case, wide, opt, proofs, output)
                if positive:
                    tuples = {r["argument_tuples"] for r in target_proofs}
                    expected_tuples = 6 if case == 24 else 64 if case in {27, 30} else 3
                    assert tuples == {expected_tuples}, (case, wide, opt, tuples)
                if positive and case != 7:
                    assert 'The program is SAFE' in output, (case, wide, opt, output)
                if case in {1, 2, 3, 7, 8, 9, 11, 12, 23}:
                    assert 'The program is SAFE' not in output, (case, wide, opt, output)
                print(f'CASE={case}/WIDE={wide}/OPT={opt}: proofs={len(proofs)}', flush=True)
    verify_runtime(identity)


if __name__ == '__main__':
    main()
