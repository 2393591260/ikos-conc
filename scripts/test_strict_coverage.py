#!/usr/bin/env python3
"""Strict coverage gates apply to the task, before evidence selection."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from svcomp_race_test import classify, export_database_report
from ikos_runtime import runtime_command, runtime_identity, verify_runtime
from analyzer.python.ikos.race_result import validate_race_result, VERDICT_LINES
from sweep_bmc_execution import validate_report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--bits', type=int, choices=(32, 64), default=32)
    args = parser.parse_args()
    command = runtime_command(args.ikos.resolve())
    identity = runtime_identity(args.ikos.resolve())
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/strict-coverage.c').read_text()
    with tempfile.TemporaryDirectory(prefix='strict-coverage-') as folder:
        work = Path(folder)
        for wide in (False, True):
            for case in range(11):
                for filtered in (False, True):
                    source = work / 'renamed.c'
                    source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') + fixture)
                    db, report = work / 'output.db', work / 'report.json'
                    options = ['--status-filter=safe', '--log=error'] if filtered else []
                    proc = subprocess.run(command + ['--analyses=race', '--concurrency=auto',
                        '--partitioning=guards', '--demote-race-to-unknown', f'-m{args.bits}',
                        '--format=json', '--report-file=' + str(report), *options,
                        '-o', str(db), str(source)], env=dict(os.environ, IKOS_BMC_RECOVER='1'),
                        capture_output=True, text=True, timeout=30)
                    assert proc.returncode == 0, (case, wide, filtered, proc.stdout, proc.stderr)
                    data = json.loads(report.read_text())
                    metadata = validate_race_result(data, check_rows=not filtered)
                    expected = 'SAFE' if case in (0, 5) else 'FALSE' if case in (1, 8, 10) else 'UNKNOWN'
                    assert metadata['verdict'] == expected, (case, wide, filtered, metadata, proc.stdout)
                    assert VERDICT_LINES[expected] in proc.stdout, (case, filtered, proc.stdout)
                    if expected == 'UNKNOWN':
                        assert metadata['unresolved_count'] > 0 and not metadata['search_performed'], metadata
                        assert not any('counterexample' in r['info'] or 'confirmed_pair_id' in r['info']
                                       for r in data['reports']), data
                    if not filtered:
                        offline = work / 'offline.json'
                        export_database_report(db, offline, command)
                        extracted = json.loads(offline.read_text())
                        assert extracted['race_result'] == metadata
                        assert extracted['reports'] == data['reports']
                        if expected == 'FALSE':
                            executions = validate_report(data)
                            assert executions and all(e['status'] == 'ACCEPT' and e['strict_false_publishable']
                                                      for e in executions), executions
                    print(f'coverage CASE={case}/WIDE={int(wide)}/filtered={filtered}: {expected}', flush=True)
        # Safety-only contracts retain raw effects and cannot authorize BMC.
        for fixture_name, cases, contract in (
                ('readonly-heap-reads.c', (0, 7), 'coverage-readonly-heap-v1'),
                ('bucket-list-ownership.c', (0,), 'coverage-bucket-ownership-v1')):
            body = (ROOT / 'analyzer/test/regression/bmc-values' / fixture_name).read_text()
            for case in cases:
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n' + body)
                proc = subprocess.run(command + ['--analyses=race', '--concurrency=auto',
                    '--partitioning=guards', '--demote-race-to-unknown', f'-m{args.bits}',
                    '--format=json', '--report-file=' + str(report), '-o', str(db), str(source)],
                    env=dict(os.environ, IKOS_BMC_RECOVER='1'), capture_output=True, text=True, timeout=30)
                assert proc.returncode == 0, (fixture_name, case, proc.stdout, proc.stderr)
                metadata = validate_race_result(json.loads(report.read_text()))
                assert metadata['coverage'] == 'Checked' and metadata['safety_only_count'] > 0, metadata
                assert any(o['contract'] == contract and o['safety_only'] for o in metadata['obligations']), metadata
                assert metadata['verdict'] == ('SAFE' if case == 0 else 'UNKNOWN'), metadata
                assert not metadata['search_performed'], metadata
                if case == 7:
                    assert metadata['remaining_pairs'] > 0, metadata
        # Required extern roots have no source statement to anchor a warning.
        for kind in ('ctors', 'dtors'):
            source = work / 'missing-root.ll'
            source.write_text("""target datalayout = "e-m:e-p:32:32-i64:64-n8:16:32-S128"
 target triple = "i386-pc-linux-gnu"
 @llvm.global_KIND = appending global [1 x { i32, void ()*, i8* }] [{ i32, void ()*, i8* } { i32 65535, void ()* @absent, i8* null }]
 declare void @absent()
 define i32 @main() { ret i32 0 }
""".replace('KIND', kind).replace('p:32:32', f'p:{args.bits}:{args.bits}')
                .replace('i386-pc-linux-gnu', 'i386-pc-linux-gnu' if args.bits == 32
                         else 'x86_64-pc-linux-gnu'))
            proc = subprocess.run(command + ['--analyses=race', f'-m{args.bits}', '--format=json',
                '--report-file=' + str(report), '-o', str(db), str(source)],
                capture_output=True, text=True, timeout=30)
            assert proc.returncode == 0, (kind, proc.stdout, proc.stderr)
            metadata = validate_race_result(json.loads(report.read_text()))
            assert metadata['verdict'] == 'UNKNOWN' and not metadata['search_performed'], metadata
            assert any(o['source'] == 'required_entry' for o in metadata['obligations']), metadata
        source = work / 'renamed.c'
        source.write_text('#define CASE 1\n' + fixture)
        proc = subprocess.run(command + ['--analyses=race', '--no-checks', f'-m{args.bits}',
            '--format=json', '--report-file=' + str(report), '-o', str(db), str(source)],
            capture_output=True, text=True, timeout=30)
        assert proc.returncode == 0, (proc.stdout, proc.stderr)
        result = validate_race_result(json.loads(report.read_text()))
        assert result['coverage'] == 'Pending' and not result['checks_complete']
        assert result['verdict'] == 'UNKNOWN' and 'The program is UNKNOWN' in proc.stdout
        # A different checker must retain its real property error.
        source.write_text('#define CASE 4\n' + fixture.replace('missing_effect();\n  return 0;',
            'missing_effect();\n  return 1 / (__VERIFIER_nondet_int() * 0);'))
        proc = subprocess.run(command + ['--analyses=race,dbz', f'-m{args.bits}',
            '--format=json', '--report-file=' + str(report), '-o', str(db), str(source)],
            capture_output=True, text=True, timeout=30)
        assert proc.returncode == 0, (proc.stdout, proc.stderr)
        assert validate_race_result(json.loads(report.read_text()))['verdict'] == 'UNKNOWN'
        assert 'The program is definitely UNSAFE' in proc.stdout and 'Race property: UNKNOWN' in proc.stdout
    verify_runtime(identity)


if __name__ == '__main__':
    main()
