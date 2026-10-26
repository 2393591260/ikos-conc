#!/usr/bin/env python3
"""Check creator release endpoints and independent selected executions.

Each run binds runtime/input/report identity and independently replays every
positive. --ablate disables only the new Unlock searches; it records results
without asserting gains. Official source-witness validation is a separate gate.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

POSITIVE = {0, 1, 2, 12, 13}
# Numeric-pointer coverage is outside this endpoint extension.
COVERAGE_BOUNDARY = {11}
EXPLORATORY = {3}

def verify_execution(execution, source, case, check, digest_json, source_binding):
    cert = copy.deepcopy(execution)
    program = cert.pop('program')
    cert['program_sha256'] = digest_json(program)
    assert check(program, cert)['status'] == 'ACCEPT'
    traces = {t['id']: t for t in cert['traces']}
    assert set(traces) == ({'main', 'provider', 'a', 'b'} if case == 12 else {'main', 'a', 'b'})
    instructions = program['instructions']
    def callee(step):
        inst = instructions[step['statement']]
        ops = inst.get('operands', [])
        return (ops[0].get('id') if inst.get('kind') == 'call' and ops and
                ops[0].get('kind') == 'function' else None)
    main = traces['main']
    release = len(main['steps']) - 1
    assert callee(main['steps'][release]) in ('pthread_mutex_unlock', 'ar.pthread.mutex.unlock')
    assert max(traces[n]['creator_step'] for n in ('a', 'b')) < release
    assert all(traces[n]['creator_trace'] == 'main' for n in ('a', 'b'))
    assert len({traces[n]['creator_step'] for n in traces if n != 'main'}) == len(traces) - 1
    assert not any(instructions[s['statement']]['kind'] == 'return' for s in main['steps'])
    assert all(s.get('joined_trace', {}).get('trace') not in ('a', 'b') for s in main['steps'])
    count = 1 + source_binding(program, cert, source, case)
    def trace(c, name):
        return next(t for t in c['traces'] if t['id'] == name)
    def reject(c, label):
        nonlocal count
        result = check(program, c)
        assert result['status'] == 'REJECT', (case, label, result)
        count += 1
    bad = copy.deepcopy(cert)
    trace(bad, 'main')['steps'].pop()
    reject(bad, 'actual release omitted')
    bad = copy.deepcopy(cert)
    trace(bad, 'main')['steps'][release]['clock'] = str(max(int(s['clock']) for s in traces['a']['steps']) + 1)
    reject(bad, 'release after target A mutex acquisition')
    bad = copy.deepcopy(cert)
    address = trace(bad, 'main')['steps'][release].get('encoded_address')
    assert address is not None, 'release lacks encoded object binding'
    address['object'] = {'kind': 'global', 'id': 'other'}
    reject(bad, 'release bound to different object')
    bad = copy.deepcopy(cert)
    trace(bad, 'b')['creator_step'] = traces['a']['creator_step']
    reject(bad, 'two targets borrow one dynamic Create')
    bad = copy.deepcopy(cert)
    del trace(bad, 'main')['steps'][traces['b']['creator_step']]
    reject(bad, 'actual selected Create omitted')
    values = [i for i, s in enumerate(main['steps']) if 'encoded_result' in s and callee(s) not in ('pthread_mutex_unlock', 'ar.pthread.mutex.unlock')]
    if values:
        bad = copy.deepcopy(cert)
        i = values[-1]
        step = trace(bad, 'main')['steps'][i]
        step['encoded_result'] = str(int(step['encoded_result']) + 1)
        reject(bad, 'retained main branch/value forged')
    if case == 12:
        provider = traces['provider']
        assert instructions[provider['steps'][-1]['statement']]['kind'] == 'return'
        bad = copy.deepcopy(cert)
        trace(bad, 'provider')['steps'].pop()
        reject(bad, 'joined provider Return omitted')
        joins = [i for i, s in enumerate(main['steps']) if 'joined_trace' in s]
        assert len(joins) == 1
        bad = copy.deepcopy(cert)
        trace(bad, 'main')['steps'][joins[0]]['joined_trace'] = {'trace': 'a', 'step': len(traces['a']['steps']) - 1}
        reject(bad, 'provider join borrows target generation')
        reads = [i for i, s in enumerate(main['steps']) if any(isinstance(src, dict) and src.get('trace') == 'provider' for src in s.get('read_from', []))]
        assert reads, 'provider-dependent guard lacks actual read-from'
        bad = copy.deepcopy(cert)
        step = trace(bad, 'main')['steps'][reads[0]]
        step['read_from'] = ['initial'] * len(step['read_from'])
        reject(bad, 'completed provider write replaced by initial value')
    return count

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[1])
    p.add_argument('--ikos', type=Path)
    p.add_argument('--bits', choices=['32', '64', 'both'], default='both')
    p.add_argument('--case', type=int, action='append')
    p.add_argument('--ablate', action='store_true')
    p.add_argument('--evidence-dir', type=Path)
    args = p.parse_args()
    temporary = None
    if args.evidence_dir is None:
        temporary = tempfile.TemporaryDirectory(prefix='ikos-creator-release-')
        args.evidence_dir = Path(temporary.name)/'evidence'
    repo = args.repo.resolve()
    sys.path[:0] = [str(repo), str(repo / 'scripts')]
    from analyzer.python.ikos.race_result import validate_race_result
    from validate_bmc_execution import check, digest_json
    from ikos_runtime import runtime_command, runtime_identity
    from test_bmc_startup_provider import verify_source_binding
    ikos = (args.ikos or repo / 'install/bin/ikos').resolve()
    identity = runtime_identity(ikos)
    env = {k: v for k, v in os.environ.items() if not k.startswith('IKOS_') and k not in ('PYTHONPATH', 'PYTHONHOME')}
    env.update(IKOS_BMC_RECOVER='1', IKOS_BMC_DIAGNOSTICS='1')
    if args.ablate:
        env['IKOS_BMC_DISABLE_CREATOR_RELEASE'] = '1'
    args.evidence_dir.mkdir(parents=True, exist_ok=False)
    fixture = (repo / 'analyzer/test/regression/bmc-values/creator-release.c').read_text()
    rows = []
    for bits in (['32', '64'] if args.bits == 'both' else [args.bits]):
        for case in (args.case or range(14)):
            work = args.evidence_dir / f'case{case}-{bits}'
            work.mkdir()
            source, report = work / 'input.c', work / 'report.json'
            source.write_text(f'#define CASE {case}\n' + fixture)
            command = [*runtime_command(ikos), '--analyses=race', '--concurrency=auto',
                       '--partitioning=guards', '--demote-race-to-unknown', '-m' + bits,
                       '--bmc-deadline-ms=20000', '--bmc-solver-timeout-ms=1000',
                       '--format=json', '--report-file=' + str(report), '-o', str(work / 'output.db'), str(source)]
            process = subprocess.run(command, cwd=work, env=env, capture_output=True, text=True, timeout=60)
            (work / 'stdout.txt').write_text(process.stdout)
            (work / 'stderr.txt').write_text(process.stderr)
            assert process.returncode == 0 and report.is_file(), (case, bits, process.stdout, process.stderr)
            data = json.loads(report.read_text())
            meta = validate_race_result(data)
            executions = [r['info']['counterexample'] for r in data['reports'] if 'counterexample' in r.get('info', {})]
            counts = [r.get('info', {}).get('bmc', {}).get('counts', {}) for r in data['reports']]
            if args.ablate:
                assert not any(c.get('creator_release_timeline_query', 0) for c in counts)
            elif case in POSITIVE:
                assert meta['verdict'] == 'FALSE' and executions, (case, bits, meta)
                assert any(c.get('creator_release_timeline_confirmed', 0) for c in counts)
            elif case in COVERAGE_BOUNDARY:
                assert meta['verdict'] == 'UNKNOWN' and not executions, (case,bits,meta)
                assert meta['coverage'] == 'Blocked', (case,bits,meta)
            elif case in EXPLORATORY:
                assert meta['verdict'] in ('FALSE', 'UNKNOWN'), (case, bits, meta)
            else:
                assert meta['verdict'] in ('SAFE', 'UNKNOWN') and not executions, (case, bits, meta)
                assert meta['analysis_complete'] and meta['checks_complete'], meta
                if case == 10: assert meta['verdict'] != 'SAFE', (case,bits,meta)
            assertions = 0
            if not args.ablate:
                for execution in executions:
                    assertions += verify_execution(execution, source, case, check, digest_json, verify_source_binding)
            print('CASE',case,bits,meta['verdict'],assertions,'PASS',flush=True)
            rows.append(dict(case=case, bits=bits, verdict=meta['verdict'], assertions=assertions,
                             executions=len(executions), counts=counts,
                             input_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                             report_sha256=hashlib.sha256(report.read_bytes()).hexdigest()))
    assert runtime_identity(ikos) == identity
    (args.evidence_dir / 'summary.json').write_text(json.dumps(dict(runtime=identity, ablation=args.ablate, rows=rows), indent=2)+'\n')

if __name__ == '__main__':
    main()
