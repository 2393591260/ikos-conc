#!/usr/bin/env python3
"""Joined descendants remain inside a retained main-lock scope; no lock inheritance."""
import argparse
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile
from ikos_runtime import runtime_command, runtime_identity, verify_runtime

ROOT = Path(__file__).resolve().parents[1]
CASES = (0, 1, 2, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
         21, 22, 24, 26, 28, 29, 31, 32, 33, 34)
SAFE = {0, 1, 2}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, required=True)
    parser.add_argument('--case', type=int, choices=CASES, action='append')
    parser.add_argument('--bits', type=int, choices=(32, 64), required=True)
    parser.add_argument('--evidence-dir', type=Path)
    args = parser.parse_args()
    identity = runtime_identity(args.ikos)
    command = runtime_command(args.ikos)
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/nested-joined-scope.c').read_text()
    env = {k:v for k,v in os.environ.items()
           if not k.startswith('IKOS_') and k not in ('PYTHONPATH', 'PYTHONHOME')}
    temporary = None
    if args.evidence_dir:
        work = args.evidence_dir
        work.mkdir(parents=True, exist_ok=True)
    else:
        temporary = tempfile.TemporaryDirectory(prefix='ikos-nested-join-')
        work = Path(temporary.name)
    records = []
    for wide in (False, True):
        for case in args.case or CASES:
            for mode in (('enabled', 'scope-off') if case in SAFE else ('enabled',)):
                directory = work / f'{case}-{int(wide)}-{mode}'
                directory.mkdir(exist_ok=True)
                source = directory / 'renamed.c'
                source.write_text(f'#define CASE {case}\n' +
                                  ('#define WIDE\n' if wide else '') + fixture)
                arm = dict(env)
                trace = directory / 'rules.jsonl'
                trace.unlink(missing_ok=True)
                arm['IKOS_RACE_RULE_REPORT'] = str(trace)
                if mode == 'scope-off':
                    arm['IKOS_RACE_DISABLE_NESTED_JOIN_SCOPE'] = '1'
                database = directory / 'output.db'
                database.unlink(missing_ok=True)
                proc = subprocess.run(command + [
                    '--analyses=race', '--concurrency=auto', '--partitioning=guards',
                    '--demote-race-to-unknown', '--bmc-deadline-ms=0', f'-m{args.bits}',
                    '-o', str(database), str(source)], env=arm,
                    capture_output=True, text=True, timeout=40)
                output = proc.stdout + proc.stderr
                (directory / 'analysis.log').write_text(output)
                assert proc.returncode == 0, (case, args.bits, wide, mode, output)
                with sqlite3.connect(database) as db:
                    settings = dict(db.execute('SELECT name,value FROM settings'))
                    result = json.loads(settings['race_result'])
                    assert settings['machine-int-domain'] == 'interval'
                safe = case in SAFE and mode == 'enabled'
                assert result['verdict'] == ('SAFE' if safe else 'UNKNOWN'), (case, args.bits, wide, mode, result, output)
                if safe or mode == 'scope-off':
                    assert result['coverage'] == 'Checked', (case, result)
                elif result['coverage'] == 'Blocked':
                    assert case in (15, 16, 19, 21, 28) and result['unresolved_count'] > 0, (case, result)
                else:
                    assert result['coverage'] == 'Checked', (case, result)
                if not safe and result['coverage'] == 'Checked':
                    assert result['remaining_pairs'] > 0, (case, result)
                rows = [json.loads(line) for line in trace.read_text().splitlines()] if trace.exists() else []
                proofs = sum(row.get('counts', {}).get('locked-creation-v1', 0) for row in rows)
                if safe:
                    assert proofs > 0, (case, rows)
                records.append(dict(case=case, bits=args.bits, wide=wide, mode=mode,
                                    result=result, locked_creation_proofs=proofs))
                print(case, args.bits, wide, mode, result['verdict'], proofs, flush=True)
    verify_runtime(identity)
    (work / 'summary.json').write_text(json.dumps(dict(passed=True, analyses=len(records), records=records), indent=2)+'\n')
    print('PASS', len(records), 'nested joined-scope controls')
    if temporary:
        temporary.cleanup()


if __name__ == '__main__':
    main()
