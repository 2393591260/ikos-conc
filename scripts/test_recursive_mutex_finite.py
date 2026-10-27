#!/usr/bin/env python3
"""Sealed recursive source facts: outer intervals, strict refusal, actual errors.

Without --ikos this only emits source variants; no analyzer is auto-discovered.
CTest passes --ikos explicitly. The four original-shaped FALSE obligations use
--mode=bmc only after the companion recursive BMC/checker patch is integrated.
"""
import argparse
import json
import os
from pathlib import Path
import re
import sqlite3
import subprocess
import signal
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CASES = tuple(range(35))
SAFE = {4, 5, 6, 7, 8, 9, 10, 28}
REFUSED = set(range(11, 28)) | set(range(29, 35))
RACY = {0, 1, 2, 3, 30, 32, 33}
CONTRACT = 'closed-recursive-mutex-creation-v1'


def source_variant(fixture, case, renamed):
    text = '#define CASE %d\n' % case + fixture
    if renamed:
        for old, new in [('global', 'datum_renamed'), ('mutex', 'guard_renamed'),
                         ('attr', 'attribute_renamed'), ('id1', 'handle_renamed'),
                         ('id2', 'other_handle_renamed'), ('t1', 'worker_renamed'),
                         ('t2', 'other_worker_renamed')]:
            text = re.sub(r'\b' + old + r'\b', new, text)
    return text



def run_analyzer(command, env, timeout):
    process = subprocess.Popen(command, env=env, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True, start_new_session=True)
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except BaseException:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.communicate()
        raise
    return subprocess.CompletedProcess(command, process.returncode, stdout, stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path)
    parser.add_argument('--bits', choices=('32', '64'), default='32')
    parser.add_argument('--mode', choices=('ai', 'bmc'), default='ai')
    parser.add_argument('--case', type=int, action='append', choices=CASES)
    parser.add_argument('--emit-dir', type=Path)
    args = parser.parse_args()
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/recursive-mutex-finite.c').read_text()
    cases = CASES if args.case is None else args.case
    if args.emit_dir:
        args.emit_dir.mkdir(parents=True, exist_ok=True)
        for case in cases:
            for renamed in (False, True):
                (args.emit_dir / ('case-%d-names-%d.c' % (case, renamed))).write_text(
                    source_variant(fixture, case, renamed))
    if args.ikos is None:
        if not args.emit_dir:
            parser.error('pass --emit-dir to generate or explicit --ikos to analyze')
        print('generated source only; no compilation or analysis')
        return
    sys.path.insert(0, str(ROOT))
    from ikos_runtime import runtime_command, runtime_identity, verify_runtime
    from analyzer.python.ikos.race_result import validate_race_result
    # Imported only when actual BMC validation was explicitly requested.
    if args.mode == 'bmc':
        from sweep_bmc_execution import validate_report
    identity = runtime_identity(args.ikos)
    with tempfile.TemporaryDirectory(prefix='ikos-recursive-finite-') as tmp:
        work = Path(tmp)
        for case in cases:
            for renamed in (False, True):
                source, report, database = work / 'renamed.c', work / 'report.json', work / 'output.db'
                source.write_text(source_variant(fixture, case, renamed))
                report.unlink(missing_ok=True)
                database.unlink(missing_ok=True)
                env = {k: v for k, v in os.environ.items()
                       if not k.startswith('IKOS_') and k not in ('PYTHONPATH', 'PYTHONHOME')}
                if args.mode == 'bmc':
                    env['IKOS_BMC_RECOVER'] = '1'
                proc = run_analyzer(runtime_command(args.ikos) + ['--analyses=race',
                    '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                    '-m' + args.bits, '--bmc-deadline-ms=' + ('0' if args.mode == 'ai' else '20000'),
                    '--bmc-solver-timeout-ms=1000', '--format=json',
                    '--report-file=' + str(report), '-o', str(database), str(source)],
                    env=env, timeout=60)
                assert proc.returncode == 0, (case, renamed, proc.stdout, proc.stderr)
                data = json.loads(report.read_text())
                metadata = validate_race_result(data)
                with sqlite3.connect(database) as db:
                    infos = [json.loads(row) for row, in db.execute('SELECT info FROM checks')]
                proofs = [row for row in infos if row.get('locked_creation_contract') == CONTRACT]
                verdict = metadata['verdict']
                if case in SAFE:
                    assert verdict == 'SAFE', (case, renamed, metadata, proc.stdout, proc.stderr)
                    if case != 28:
                        assert proofs, (case, renamed, metadata, infos)
                elif case in RACY:
                    assert verdict != 'SAFE', (case, renamed, metadata, infos)
                    if args.mode == 'bmc' and case < 4:
                        assert verdict == 'FALSE', (case, renamed, metadata)
                if case in REFUSED:
                    assert not proofs, (case, renamed, metadata, infos)
                if args.mode == 'bmc' and verdict == 'FALSE':
                    executions = validate_report(data)
                    assert executions and all(e['status'] == 'ACCEPT' for e in executions), executions
                print('recursive finite case=%d names=%d bits=%s: %s proofs=%d' %
                      (case, renamed, args.bits, verdict, len(proofs)), flush=True)
    verify_runtime(identity)


if __name__ == '__main__':
    main()
