#!/usr/bin/env python3
"""Check protected integer field feedback and its conservative boundaries."""
import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from svcomp_race_test import classify, VERDICT_RACE, VERDICT_SAFE, VERDICT_UNKNOWN
from validate_bmc_execution import check, digest_json
from ikos_runtime import runtime_command


CASES = {
    0: ('same-section store/load', VERDICT_SAFE),
    1: ('reachable equality branch', VERDICT_UNKNOWN),
    2: ('nonzero offset and neighboring leaf', VERDICT_SAFE),
    3: ('negative signed int', VERDICT_SAFE),
    4: ('high unsigned int', VERDICT_SAFE),
    5: ('negative 64-bit datum', VERDICT_SAFE),
    6: ('high unsigned 64-bit datum', VERDICT_SAFE),
    7: ('release and reacquire', VERDICT_SAFE),
    8: ('intermediate value before restoration', VERDICT_UNKNOWN),
    9: ('writer under another lock', VERDICT_UNKNOWN),
    10: ('unlocked writer', VERDICT_UNKNOWN),
    11: ('pointer argument to helper', VERDICT_UNKNOWN),
    12: ('overlapping byte write', VERDICT_UNKNOWN),
    13: ('memset write', VERDICT_UNKNOWN),
    14: ('volatile field', VERDICT_UNKNOWN),
    15: ('dynamic leaf offset', VERDICT_UNKNOWN),
    16: ('nonzero static initializer', VERDICT_SAFE),
    17: ('reachable initializer equality', VERDICT_UNKNOWN),
    18: ('writer after first creation', VERDICT_UNKNOWN),
    19: ('join of distinct values', VERDICT_UNKNOWN),
    20: ('join of equal values', VERDICT_SAFE),
    21: ('runtime aggregate write', VERDICT_UNKNOWN),
    22: ('no mutex common to all writers', VERDICT_UNKNOWN),
    23: ('late same-value writer under another lock', VERDICT_UNKNOWN),
    24: ('nested acquire retains previously held mutex', VERDICT_SAFE),
    25: ('foreign release reaches equality branch', VERDICT_UNKNOWN),
    26: ('foreign release stays within initial/write range', VERDICT_SAFE),
    27: ('Polka local relation reconstruction', VERDICT_SAFE),
    28: ('broken Polka local relation reconstruction', VERDICT_UNKNOWN),
    29: ('Polka same-critical-section field relation', VERDICT_SAFE),
    30: ('broken Polka same-critical-section field relation', VERDICT_UNKNOWN),
    31: ('cross-thread joint field handoff', VERDICT_SAFE),
    32: ('broken cross-thread field relation', VERDICT_UNKNOWN),
}
JOINT_CASES = {
    33: (0, 'joint release/acquire equality', VERDICT_SAFE),
    34: (1, 'unequal release with equal marginal ranges', VERDICT_UNKNOWN),
    35: (2, 'unequal initializer', VERDICT_UNKNOWN),
    36: (3, 'joint writer uses a different mutex', VERDICT_UNKNOWN),
    37: (4, 'joint writer is unlocked', VERDICT_UNKNOWN),
    38: (5, 'joint byte overlap', VERDICT_UNKNOWN),
    39: (6, 'joint pointer helper escape', VERDICT_UNKNOWN),
    40: (7, 'captured private value is reassigned', VERDICT_UNKNOWN),
    41: (8, 'reads split over two critical sections', VERDICT_UNKNOWN),
    42: (9, 'local read observes intermediate inequality', VERDICT_UNKNOWN),
    43: (10, 'nested unrelated acquire cannot restore equality', VERDICT_UNKNOWN),
    44: (11, 'unrelated nested unlock is not a joint handoff', VERDICT_SAFE),
    45: (12, 'reacquire declines a fresh joint certificate', VERDICT_UNKNOWN),
    46: (13, 'prior unrelated acquire declines a fresh joint certificate', VERDICT_UNKNOWN),
    47: (14, 'late unequal writer with unchanged marginals', VERDICT_UNKNOWN),
    48: (15, 'writer updates only one field', VERDICT_UNKNOWN),
    49: (16, 'release join takes the intersection of equalities', VERDICT_UNKNOWN),
    50: (18, 'volatile joint fields', VERDICT_UNKNOWN),
    51: (19, '64-bit joint data on either ABI', VERDICT_SAFE),
    52: (20, 'unequal 64-bit release', VERDICT_UNKNOWN),
    53: (21, 'repeated acquire has no success proof', VERDICT_UNKNOWN),
    54: (22, 'trylock has no definite acquisition', VERDICT_UNKNOWN),
    55: (23, 'unknown call invalidates joint facts', VERDICT_UNKNOWN),
    56: (24, 'different-width cells refuse a template', VERDICT_UNKNOWN),
    57: (25, 'different-signedness cells refuse a template', VERDICT_UNKNOWN),
    58: (26, 'three-field equalities are imported together', VERDICT_SAFE),
    59: (27, 'merged ERRORCHECK ownership is not a fresh handoff', VERDICT_UNKNOWN),
    60: (0, 'skipped initialization cannot supply zero equality', VERDICT_UNKNOWN),
    61: (28, 'nonzero equality from initialized native memory', VERDICT_SAFE),
}
CASES.update({case: (label, verdict) for case, (_, label, verdict) in JOINT_CASES.items()})
BMC_CASES = {1, 8, 17}
IMPORT_ABLATION_CASES = {25, 26, 33, 34, 47, 58, 61}


def check_executions(report):
    executions = [row['info']['counterexample'] for row in report['reports']
                  if 'counterexample' in row.get('info', {})]
    assert executions, 'FALSE result has no independent execution evidence'
    for execution in executions:
        certificate = copy.deepcopy(execution)
        program = certificate.pop('program')
        certificate['program_sha256'] = digest_json(program)
        result = check(program, certificate)
        assert result['status'] == 'ACCEPT', result
    return len(executions)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--bits', type=int, choices=(32, 64), default=32)
    parser.add_argument('--case', type=int, choices=tuple(CASES), action='append',
                        help='select one case; repeat to select several')
    parser.add_argument('--skip-bmc', action='store_true',
                        help='run only the unbounded AI controls')
    args = parser.parse_args()
    cases = list(dict.fromkeys(args.case)) if args.case else list(CASES)
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/protected-field-values.c').read_text()
    relation_fixture = (ROOT / 'analyzer/test/regression/bmc-values/protected-field-relations.c').read_text()
    joint_fixture = (ROOT / 'analyzer/test/regression/bmc-values/mutex-field-equalities.c').read_text()
    env = {key: value for key, value in os.environ.items()
           if not key.startswith('IKOS_') and key not in {'PYTHONHOME', 'PYTHONPATH'}}
    env['IKOS_BMC_RECOVER'] = '1'
    count = 0
    executions = 0
    with tempfile.TemporaryDirectory(prefix='ikos-protected-fields-') as temporary:
        work = Path(temporary)
        for case in cases:
            relational = case >= 27
            preamble = f'#define CASE {case}\n'
            if case in JOINT_CASES:
                preamble += f'#define VARIANT {JOINT_CASES[case][0]}\n'
            elif relational:
                mode, broken = divmod(case - 27, 2)
                preamble += f'#define MODE {mode}\n#define BROKEN {broken}\n'
            # Rename source symbols to keep the oracle independent of names.
            text = (joint_fixture if case in JOINT_CASES else
                    relation_fixture if relational else fixture)
            text = text.replace('state', 'shared_record').replace('payload', 'shared_data')
            text = text.replace('worker', 'consume').replace('save_through_pointer', 'store_indirect')
            source = work / 'renamed.c'
            source.write_text(preamble + text)
            analysis_options = (['--partitioning=no', '--domain=apron-polka-polyhedra']
                                if relational else ['--partitioning=guards'])
            if case == 60:
                analysis_options.append('--globals-init=none')
            for bmc in ((False, True) if case in BMC_CASES and not args.skip_bmc else (False,)):
                report = work / 'report.json'
                report.unlink(missing_ok=True)
                database = work / 'output.db'
                database.unlink(missing_ok=True)
                command = runtime_command(args.ikos) + ['--analyses=race', '--concurrency=auto'] + analysis_options + [
                           '--demote-race-to-unknown',
                           '--bmc-deadline-ms=' + ('20000' if bmc else '0'),
                           '--bmc-solver-timeout-ms=1000', f'-m{args.bits}',
                           '--format=json', '--status-filter=*',
                           '--report-file=' + str(report), '-o', str(database), str(source)]
                if case in IMPORT_ABLATION_CASES:
                    command.append('--log=debug')
                result = subprocess.run(command, env=env, capture_output=True,
                                        text=True, timeout=40 if bmc else 30)
                verdict = classify(result.stdout, result.returncode, False)
                expected = VERDICT_RACE if bmc else CASES[case][1]
                assert result.returncode == 0 and verdict == expected, (
                    case, CASES[case][0], args.bits, bmc, verdict,
                    result.stdout[-10000:], result.stderr[-10000:])
                if bmc:
                    assert report.exists(), (case, 'missing JSON report')
                    executions += check_executions(json.loads(report.read_text()))
                count += 1
                print(f'protected field bits={args.bits}/case={case}/bmc={bmc}: '
                      f'{verdict} ({CASES[case][0]})', flush=True)
                if case in IMPORT_ABLATION_CASES:
                    ablated_env = dict(env, IKOS_DISABLE_MUTEX_FIELD_EQUALITY_IMPORT='1')
                    database.unlink(missing_ok=True)
                    report.unlink(missing_ok=True)
                    ablated = subprocess.run(command, env=ablated_env, capture_output=True,
                                             text=True, timeout=30)
                    ablated_verdict = classify(ablated.stdout, ablated.returncode, False)
                    ablated_expected = VERDICT_SAFE if case == 26 else VERDICT_UNKNOWN
                    assert ablated.returncode == 0 and ablated_verdict == ablated_expected, (
                        case, 'import-only ablation', ablated_verdict,
                        ablated.stdout[-10000:], ablated.stderr[-10000:])
                    if case in {33, 58, 61}:
                        diagnostics = ablated.stdout + ablated.stderr
                        for event in ('initialized', 'release-check', 'import-suppressed'):
                            assert 'Mutex field equality: ' + event in diagnostics, (
                                case, 'missing relation event', event, diagnostics[-10000:])
                    count += 1
                    print(f'protected field bits={args.bits}/case={case}/import-disabled: '
                          f'{ablated_verdict}', flush=True)
    print(f'PASS {count} protected field controls; {executions} AR executions ACCEPT', flush=True)


if __name__ == '__main__':
    main()
