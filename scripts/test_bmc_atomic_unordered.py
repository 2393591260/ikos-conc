#!/usr/bin/env python3
"""Check complete verifier-atomic unordered queries and same-runtime ablation."""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from ikos_runtime import runtime_command, runtime_identity, verify_runtime
from validate_bmc_execution import check, digest_json
from test_bmc_sibling_atomic import publication_contract

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from svcomp_race_test import classify, VERDICT_RACE
from analyzer.python.ikos.race_result import validate_race_result

CASES = {
    0: 'named-interior', 1: 'explicit-interior', 4: 'both-atomic',
    5: 'publication', 6: 'reverse-publication', 7: 'writer-missing-return',
    8: 'reader-missing-return', 10: 'common-mutex', 11: 'stdout-refused',
    12: 'native-atomic-refused', 13: 'infinite-suffix', 14: 'atomic-aux-join-chain',
    15: 'main-before-join', 16: 'main-after-join', 17: 'create-orders-targets',
    18: 'repeated-static-create', 19: 'later-atomic-does-not-order-earlier-read',
    21: 'semaphore-refused', 22: 'rwlock-refused', 23: 'condition-refused',
    24: 'once-refused', 25: 'barrier-refused',
}
POSITIVE = {0, 1, 15, 18, 19}
REFUSED = {4, 7, 8, 10, 11, 12, 13, 21, 22, 23, 24, 25}
ORIGINALS = {
    '85': ('04_incdec_cas-race.i', 'a025fe4ca65874bde9a09e6a2d9fa9eceda581ed1bc5b23e0ff5285795ed115b'),
    '138': ('38_rand_cas_vs_concur.i', '628ef5ad19754647fb807fc1cc13e384334d380a6e3691b7b5f1842f42e5f662'),
    '90': ('08_rand_cas-race.i', 'adb74bb4036ab42451aee4c4c2c855416fb34747e80c1114ce822e44c6b19376'),
    '95': ('10_fmaxsym_cas.i', 'a11d0cba01cb66911cac56797cb36d325dc008752ffb55c25c965bd0644c21c3'),
    '99': ('12_fmaxsymopt_cas.i', '7a0803bc25bdee4cec741e6286f7294c3704d91bb65e5684830693d517eafafb'),
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def trace_of(certificate, label):
    return next(t for t in certificate['traces'] if t['id'] == label)


def boundary(program, step):
    synthetic = step.get('synthetic_event')
    if synthetic in ('atomic_begin', 'atomic_end'):
        return synthetic
    inst = program['instructions'][step['statement']]
    if inst['kind'] == 'call':
        operand = inst['operands'][0]
        if operand['kind'] == 'function':
            return {'__VERIFIER_atomic_begin': 'atomic_begin',
                    '__VERIFIER_atomic_end': 'atomic_end'}.get(operand['id'])
    return None


def replay_execution(execution, damage=True):
    certificate = copy.deepcopy(execution)
    program = certificate.pop('program')
    certificate['program_sha256'] = digest_json(program)
    accepted = check(program, certificate)
    assert accepted['status'] == 'ACCEPT', accepted
    assertions = []
    if not damage:
        return accepted, assertions

    def rejected(name, changed):
        result = check(program, changed)
        assert result['status'] == 'REJECT', (name, result)
        assertions.append(dict(name=name, result=result))

    for label in ('a', 'b'):
        trace = trace_of(certificate, label)
        assert program['instructions'][trace['steps'][-1]['statement']]['kind'] == 'return'
        changed = copy.deepcopy(certificate)
        trace_of(changed, label)['steps'].pop()
        rejected('missing-root-return-' + label, changed)

    # Each creation is an executed occurrence, not just an entry function name.
    for trace in certificate['traces']:
        if 'creator_step' not in trace:
            continue
        changed = copy.deepcopy(certificate)
        trace_of(changed, trace['id'])['creator_step'] = 10**9
        rejected('wrong-create-occurrence-' + trace['id'], changed)
    children = [t for t in certificate['traces'] if 'creator_step' in t]
    if len(children) >= 2:
        changed = copy.deepcopy(certificate)
        altered = trace_of(changed, children[1]['id'])
        altered['creator_trace'] = children[0]['creator_trace']
        altered['creator_step'] = children[0]['creator_step']
        rejected('duplicate-creation-instance', changed)

    changed = copy.deepcopy(certificate)
    target = changed['targets'][0]
    target['step'] = len(trace_of(changed, target['trace'])['steps']) - 1
    rejected('target-is-return-not-access', changed)

    read = next(((t['id'], i) for t in certificate['traces']
                 for i, step in enumerate(t['steps']) if step.get('read_from')), None)
    assert read, 'positive must export a real read source'
    changed = copy.deepcopy(certificate)
    trace_of(changed, read[0])['steps'][read[1]]['read_from'][0] = dict(trace='absent-instance', step=0)
    rejected('wrong-read-from-occurrence', changed)

    for kind in ('atomic_begin', 'atomic_end'):
        position = next(((t['id'], i) for t in certificate['traces']
                         for i, step in enumerate(t['steps']) if boundary(program, step) == kind), None)
        assert position, ('missing atomic boundary', kind)
        changed = copy.deepcopy(certificate)
        del trace_of(changed, position[0])['steps'][position[1]]
        rejected('deleted-' + kind, changed)

    # Preserve per-trace order but move the whole ordinary target trace into the
    # other worker's indivisible section. Limit this mutation to fixed startup.
    if all(trace_of(certificate, t['trace'])['entry'] != 'main' for t in certificate['targets']):
        atomic = next((target for target, actual in zip(certificate['targets'], accepted['targets'])
                       if actual['atomic']), None)
        plain = next((target for target, actual in zip(certificate['targets'], accepted['targets'])
                      if not actual['atomic']), None)
        assert atomic and plain
        owner = trace_of(certificate, atomic['trace'])
        begin = max(i for i in range(atomic['step'])
                    if boundary(program, owner['steps'][i]) == 'atomic_begin')
        changed = copy.deepcopy(certificate)
        by_name = {t['id']: t for t in changed['traces']}
        events = sorted((int(s['clock']), t['id'], i) for t in changed['traces']
                        if t['id'] != 'startup' for i, s in enumerate(t['steps']))
        moved = [e for e in events if e[1] == plain['trace']]
        events = [e for e in events if e[1] != plain['trace']]
        anchor = next(i for i, e in enumerate(events) if e[1:] == (atomic['trace'], begin))
        events[anchor + 1:anchor + 1] = moved
        for clock, (_, name, i) in enumerate(events):
            by_name[name]['steps'][i]['clock'] = str(clock)
        rejected('ordinary-trace-interleaved-inside-atomic', changed)
    return accepted, assertions


def run(ikos, source, bits, work, disabled=False, expected=None,
        require_new=False, refuse_new=False, damage=False):
    work.mkdir(parents=True, exist_ok=True)
    report = work / 'report.json'
    command = runtime_command(ikos) + [
        '--analyses=race', '--concurrency=auto', '--demote-race-to-unknown',
        f'-m{bits}', '--bmc-deadline-ms=20000', '--bmc-solver-timeout-ms=1000',
        '--format=json', '--report-file=' + str(report),
        '-o', str(work / 'output.db'), str(source)]
    environment = {k: v for k, v in os.environ.items() if not k.startswith('IKOS_')}
    environment.update(IKOS_BMC_RECOVER='1', IKOS_BMC_DIAGNOSTICS='1')
    environment.pop('PYTHONPATH', None)
    environment.pop('PYTHONHOME', None)
    environment.pop('IKOS_BMC_DISABLE_ATOMIC_UNORDERED', None)
    if disabled:
        environment['IKOS_BMC_DISABLE_ATOMIC_UNORDERED'] = '1'
    record = dict(input=str(source), input_sha256=sha(source), bits=bits,
                  environment_sha256={key: hashlib.sha256(value.encode()).hexdigest()
                    for key, value in sorted(environment.items())
                    if key.startswith('IKOS_') or key in (
                        'PATH', 'LD_LIBRARY_PATH', 'LD_PRELOAD', 'LD_AUDIT', 'CPATH',
                        'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'LIBRARY_PATH',
                        'LANG', 'LC_ALL', 'TMPDIR')},
                  command=command, ablation=disabled, wall_seconds=30,
                  bmc_deadline_ms=20000, solver_timeout_ms=1000)
    save(work / 'invocation.json', record)
    proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, env=environment, start_new_session=True)
    try:
        stdout, stderr = proc.communicate(timeout=30)
    except BaseException:
        import signal
        os.killpg(proc.pid, signal.SIGKILL)
        proc.communicate()
        raise
    proc = subprocess.CompletedProcess(command, proc.returncode, stdout, stderr)
    (work / 'stdout.txt').write_text(proc.stdout)
    (work / 'stderr.txt').write_text(proc.stderr)
    assert proc.returncode == 0, (source, proc.returncode, proc.stdout, proc.stderr)
    data = json.loads(report.read_text())
    metadata = validate_race_result(data)
    verdict = classify(proc.stdout, proc.returncode, False)
    assert expected is None or (verdict == VERDICT_RACE) == expected, (source, disabled, verdict)
    counts = {}
    executions = []
    for row in data['reports']:
        info = row['info']
        for key, value in info.get('bmc', {}).get('counts', {}).items():
            counts[key] = counts.get(key, 0) + value
        if 'counterexample' in info:
            accepted, corruptions = replay_execution(info['counterexample'], damage=damage)
            executions.append(dict(replay=accepted, corruptions=corruptions))
    assert bool(executions) == (verdict == VERDICT_RACE), (source, verdict, executions)
    if disabled or refuse_new:
        assert not counts.get('atomic_unordered_query') and not counts.get('atomic_unordered_sat'), counts
    if require_new:
        assert counts.get('atomic_unordered_query') and counts.get('atomic_unordered_sat'), counts
    record.update(verdict=verdict, metadata=metadata, counts=counts, executions=executions,
                  report_sha256=sha(report))
    save(work / 'result.json', record)
    print(f'{source.name}/{bits}/disabled={disabled}: {verdict}', flush=True)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--bits', choices=('32', '64', 'both'), default='32')
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--original-dir', type=Path)
    parser.add_argument('--with-related', action='store_true',
                        help='also run unchanged completion/AR and pinned historical sibling controls')
    args = parser.parse_args()
    ikos = args.ikos.resolve()
    temporary = None if args.output_dir else tempfile.TemporaryDirectory(prefix='ikos-atomic-unordered-')
    work = args.output_dir.resolve() if args.output_dir else Path(temporary.name)
    work.mkdir(parents=True, exist_ok=True)
    identity = runtime_identity(ikos)
    save(work / 'runtime-before.json', identity)
    validators = {name: sha(ROOT / 'scripts' / name) for name in (
        'validate_bmc_execution.py', 'test_bmc_atomic_completion.py',
        'test_bmc_sibling_atomic.py', 'test_bmc_execution.py',
        'test_bmc_atomic_unordered.py')}
    save(work / 'validator-identity.json', validators)
    publication_contract()
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/atomic-unordered.c').read_text()
    old = (ROOT / 'analyzer/test/regression/bmc-correctness/sibling-atomic.c').read_text()
    results = []
    for bits in ((32, 64) if args.bits == 'both' else (int(args.bits),)):
        for wide in (False, True):
            for case in CASES:
                name = f'case-{case}-wide-{int(wide)}'
                source = work / f'{name}.c'
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') +
                                  fixture.replace('payload', 'shared_datum'))
                positive = case in POSITIVE
                results.append(run(ikos, source, bits, work / str(bits) / name / 'enabled',
                                   expected=positive, require_new=positive,
                                   refuse_new=case in REFUSED, damage=positive))
                # Every input has one same-artifact, same-budget switch control.
                results.append(run(ikos, source, bits, work / str(bits) / name / 'disabled',
                                   disabled=True, expected=False))
        for case in (0, 10, 15):
            source = work / f'historical-sibling-{case}.c'
            source.write_text(f'#define CASE {case}\n' + old)
            results.append(run(ikos, source, bits, work / str(bits) / source.stem,
                               expected=True, require_new=case != 15, damage=True))
        source = ROOT / 'analyzer/test/regression/bmc-correctness/atomic-publication.c'
        results.append(run(ikos, source, bits, work / str(bits) / 'historical-publication',
                           expected=False))
        if args.original_dir and bits == 32:
            for task, (name, digest) in ORIGINALS.items():
                source = args.original_dir.resolve() / name
                assert sha(source) == digest, ('original source identity', task, source)
                for disabled in (False, True):
                    # 95/99 are diagnostic enumeration controls; 90 has a separate
                    # nonreturning-provider blocker. No recovery assertion for them.
                    result = run(ikos, source, bits, work / str(bits) / f'original-{task}' /
                                 ('disabled' if disabled else 'enabled'),
                                 disabled=disabled,
                                 expected=not disabled if task in ('85', '138') else None,
                                 require_new=not disabled and task in ('85', '138'),
                                 damage=not disabled and task in ('85', '138'))
                    result['task'] = task
                    results.append(result)
        if args.with_related:
            environment = dict(os.environ)
            environment.pop('IKOS_BMC_DISABLE_ATOMIC_UNORDERED', None)
            for runner in ('test_bmc_atomic_completion.py', 'test_bmc_sibling_atomic.py',
                           'test_bmc_execution.py'):
                command = [sys.executable, str(ROOT / 'scripts' / runner),
                           '--ikos', str(ikos), '--bits', str(bits)]
                proc = subprocess.run(command, capture_output=True, text=True, env=environment)
                (work / f'{runner}-{bits}.log').write_text(proc.stdout + proc.stderr)
                assert proc.returncode == 0, (runner, bits, proc.stdout, proc.stderr)
    verify_runtime(identity)
    assert validators == {name: sha(ROOT / 'scripts' / name) for name in validators}
    save(work / 'runtime-after.json', runtime_identity(ikos))
    save(work / 'summary.json', dict(status='PASS', runs=len(results), results=results,
                                    official_source_witness=False))
    print(f'{len(results)} runs passed; evidence: {work}', flush=True)


if __name__ == '__main__':
    main()
