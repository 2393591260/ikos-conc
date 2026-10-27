#!/usr/bin/env python3
"""Check source-exact scalar completion with BMC disabled; use --bits=64 explicitly."""
import argparse
import json
import os
from pathlib import Path
import signal
import sqlite3
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from svcomp_race_test import classify, VERDICT_SAFE, VERDICT_UNKNOWN
from ikos_runtime import runtime_command, runtime_identity, verify_runtime

CONTRACT = 'independent-scalar-thread-completion-v1'
# The ordinary copy in case 15 is eliminated before the audited source IR.
POSITIVE = set(range(8)) | {15, 28, 30, 32, 33, 34}
RACY = {8, 9, 10, 12, 14, 16, 19, 22, 23, 26}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--bits', choices=('32', '64'), default='32')
    args = parser.parse_args()
    identity = runtime_identity(args.ikos)
    command = runtime_command(args.ikos)
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/scalar-thread-completion.c').read_text()
    env = {k: v for k, v in os.environ.items() if not k.startswith('IKOS_')}
    with tempfile.TemporaryDirectory(prefix='ikos-scalar-completion-') as temp:
        work = Path(temp)
        for renamed in (False, True):
            for wide in (False, True):
                for case in range(37):
                    if case == 28 and not wide:
                        continue
                    text = fixture
                    if renamed:
                        text = text.replace('worker', 'entry').replace('payload', 'data_cell')
                        text = text.replace('buffer', 'dynamic_bytes').replace('touch', 'assign_value')
                    source = work / 'control.c'
                    source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') + text)
                    database = work / 'control.db'
                    invocation = command + ['--analyses=race', '--concurrency=auto',
                        '--partitioning=guards', '--demote-race-to-unknown', '--bmc-deadline-ms=0',
                        '-m' + args.bits, '-o', str(database), str(source)]
                    process = subprocess.Popen(invocation, env=env, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, text=True, start_new_session=True)
                    try:
                        stdout, stderr = process.communicate(timeout=60)
                    except BaseException:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.communicate()
                        raise
                    proc = subprocess.CompletedProcess(invocation, process.returncode, stdout, stderr)
                    assert proc.returncode == 0, (case, wide, renamed, proc.stdout, proc.stderr)
                    verdict = classify(proc.stdout, proc.returncode, False)
                    with sqlite3.connect(database) as db:
                        infos = [json.loads(row[0]) for row in db.execute('SELECT info FROM checks')]
                    proofs = [info for info in infos if info.get('thread_lifecycle_contract') == CONTRACT]
                    if case in POSITIVE:
                        assert verdict == VERDICT_SAFE and proofs, (case, wide, renamed, verdict, infos)
                        if case == 7:
                            assert any(p.get('implicit_call_endpoint') for p in proofs), proofs
                    elif case in (8, 9, 26, 31):
                        # Valid main postjoin exclusions may coexist with real worker/prejoin races.
                        assert verdict == VERDICT_UNKNOWN, (case, wide, renamed, verdict, infos)
                    else:
                        assert not proofs, (case, wide, renamed, proofs)
                        if case in RACY:
                            assert verdict == VERDICT_UNKNOWN, (case, wide, renamed, verdict, infos)
                    print(f'scalar completion case={case}/wide={wide}/renamed={renamed}: '
                          f'{verdict}; proofs={len(proofs)}', flush=True)
    verify_runtime(identity)


if __name__ == '__main__':
    main()
