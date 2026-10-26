#!/usr/bin/env python3
"""Exercise timed main/provider executions and independent certificate damage."""
import argparse
import copy
import contextlib
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'scripts'))
from analyzer.python.ikos.race_result import validate_race_result
from validate_bmc_execution import check, digest_json
from ikos_runtime import runtime_command, runtime_identity

POSITIVE = {0, 7, 9, 10, 11, 13}


def verify_source_binding(program, certificate, source, case):
    # Exercise the maintained CLI with the actual generated C input. This
    # binds the same direct-API certificate to source bytes independently.
    with tempfile.TemporaryDirectory(prefix='ikos-provider-cli-') as temp:
        folder = Path(temp)
        program_path, certificate_path = folder / 'program.json', folder / 'certificate.json'
        program_path.write_text(json.dumps(program))
        cli_certificate = copy.deepcopy(certificate)
        cli_certificate['input_sha256'] = hashlib.sha256(Path(source).read_bytes()).hexdigest()
        certificate_path.write_text(json.dumps(cli_certificate))
        command = [sys.executable, '-B', str(ROOT / 'scripts/validate_bmc_execution.py'),
                   '--program', str(program_path), '--certificate', str(certificate_path),
                   '--input', str(Path(source).resolve())]
        accepted = subprocess.run(command, capture_output=True, text=True, timeout=30)
        assert accepted.returncode == 0, (case, 'CLI actual source', accepted.stdout, accepted.stderr)
        assert json.loads(accepted.stdout)['status'] == 'ACCEPT'
        cli_certificate['input_sha256'] = '0' * 64
        certificate_path.write_text(json.dumps(cli_certificate))
        rejected = subprocess.run(command, capture_output=True, text=True, timeout=30)
        assert rejected.returncode != 0, (case, 'CLI source corruption', rejected.stdout)
        assert json.loads(rejected.stdout) == dict(status='REJECT', reason='source hash mismatch')
        return 2


def verify_execution(execution, case, *, allow_target_joins=False, target_entries=None, source=None):
    certificate = copy.deepcopy(execution)
    program = certificate.pop('program')
    certificate['program_sha256'] = digest_json(program)
    result = check(program, certificate)
    assert result['status'] == 'ACCEPT', (case, result)
    traces = {t['id']: t for t in certificate['traces']}
    assert set(traces) == {'main', 'provider', 'a', 'b'}, traces.keys()
    assert traces['main']['entry'] == 'main'
    assert traces['provider']['entry'] == 'provider'
    expected_entries = {'worker_a', 'worker_b'} if target_entries is None else set(target_entries)
    assert {traces[t['trace']]['entry'] for t in certificate['targets']} == expected_entries
    assert all(t['creator_trace'] == 'main' for n, t in traces.items() if n != 'main')
    assert len({t['creator_step'] for n, t in traces.items() if n != 'main'}) == 3
    instructions = program['instructions']
    provider = traces['provider']
    assert instructions[provider['steps'][-1]['statement']]['kind'] == 'return'
    main = traces['main']
    reads = [(i, s) for i, s in enumerate(main['steps'])
             if instructions[s['statement']]['kind'] == 'load' and
             any(isinstance(src, dict) and src.get('trace') == 'provider'
                 for src in s.get('read_from', []))]
    assert reads, 'main has no actual read-from provider write'
    joins = [(i, s) for i, s in enumerate(main['steps']) if 'joined_trace' in s]
    if case != 7:
        provider_joins = [(i, step) for i, step in joins
                          if step['joined_trace']['trace'] == 'provider']
        assert len(provider_joins) == 1, joins
        assert provider_joins[0][0] < reads[0][0] < min(
            traces[n]['creator_step'] for n in ('a', 'b'))
        if allow_target_joins:
            extra = [(i, step) for i, step in joins
                     if step['joined_trace']['trace'] != 'provider']
            joined_targets = [step['joined_trace']['trace'] for _, step in extra]
            assert len(joined_targets) == len(set(joined_targets))
            for i, step in extra:
                child = step['joined_trace']['trace']
                assert child in ('a', 'b') and i > traces[child]['creator_step']
                terminal = traces[child]['steps'][-1]
                assert instructions[terminal['statement']]['kind'] == 'return'
                assert step['joined_trace']['step'] == len(traces[child]['steps']) - 1
                assert int(terminal['clock']) < int(step['clock'])
        else:
            assert len(joins) == 1, joins
    else:
        assert not joins
    count = 1

    def reject(broken, description, reason=None):
        nonlocal count
        result = check(program, broken)
        assert result['status'] == 'REJECT', (case, description, result)
        if reason is not None:
            assert result.get('reason') == reason, (case, description, result)
        count += 1

    def trace_in(cert, name):
        return next(t for t in cert['traces'] if t['id'] == name)

    # Identity damage must fail at the intended contract, before incidental
    # dangling references or later CFG/clock checks can reject the mutation.
    broken = copy.deepcopy(certificate)
    trace_in(broken, 'provider')['id'] = 'unexpected'
    reject(broken, 'unknown trace identity', 'trace identities')
    broken = copy.deepcopy(certificate)
    trace_in(broken, 'main')['entry'] = 'provider'
    reject(broken, 'main label with child entry', 'timed main trace identity')
    broken = copy.deepcopy(certificate)
    trace_in(broken, 'provider')['entry'] = 'main'
    reject(broken, 'provider label with main entry', 'provider trace identity')
    broken = copy.deepcopy(certificate)
    trace_in(broken, 'a')['entry'] = 'main'
    reject(broken, 'target label with second main', 'timed target entry identity')
    broken = copy.deepcopy(certificate)
    legacy = copy.deepcopy(traces['main'])
    legacy['id'] = 'startup'
    broken['traces'].append(legacy)
    reject(broken, 'mixed startup and timed main roots', 'timed main trace identity')
    broken = copy.deepcopy(certificate)
    trace_in(broken, 'provider')['creator_step'] = traces['a']['creator_step']
    reject(broken, 'duplicate actual creation',
           'two traces share one creation occurrence')
    broken = copy.deepcopy(certificate)
    parent_create = traces['main']['steps'][provider['creator_step']]
    trace_in(broken, 'provider')['steps'][0]['clock'] = str(int(parent_create['clock']) - 1)
    reject(broken, 'provider begins before its actual create',
           'thread executes before actual create')
    if joins:
        provider_join = next(step for _, step in joins
                             if step['joined_trace']['trace'] == 'provider')
        broken = copy.deepcopy(certificate)
        trace_in(broken, 'provider')['steps'][-1]['clock'] = str(int(provider_join['clock']) + 1)
        reject(broken, 'join before actual provider Return',
               'joined instance completion not replayed')

    if source is not None:
        count += verify_source_binding(program, certificate, source, case)

    broken = copy.deepcopy(certificate)
    broken['traces'] = [t for t in broken['traces'] if t['id'] != 'provider']
    reject(broken, 'omitted actual provider')
    if joins:
        broken = copy.deepcopy(certificate)
        trace_in(broken, 'provider')['steps'].pop()
        reject(broken, 'missing provider Return before join')
        broken = copy.deepcopy(certificate)
        trace_in(broken, 'main')['steps'][joins[0][0]]['joined_trace'] = {
            'trace': 'a', 'step': len(traces['a']['steps']) - 1}
        reject(broken, 'wrong joined dynamic generation')
    write_index = next(i for i, s in enumerate(provider['steps'])
                       if instructions[s['statement']]['kind'] == 'store')
    broken = copy.deepcopy(certificate)
    del trace_in(broken, 'provider')['steps'][write_index]
    reject(broken, 'provider write omitted')
    broken = copy.deepcopy(certificate)
    trace_in(broken, 'provider')['creator_step'] = traces['a']['creator_step']
    reject(broken, 'provider bound to target creation')
    read_index, read = reads[0]
    broken = copy.deepcopy(certificate)
    trace_in(broken, 'main')['steps'][read_index]['read_from'] = ['initial'] * len(read['read_from'])
    reject(broken, 'stale initial source after provider publication')
    broken = copy.deepcopy(certificate)
    trace_in(broken, 'main')['steps'][read_index]['read_from'] = [
        {'trace': 'main', 'step': read_index}] * len(read['read_from'])
    reject(broken, 'reverse/self read-from')
    assert 'encoded_result' in read, 'guard read value was not encoded'
    broken = copy.deepcopy(certificate)
    trace_in(broken, 'main')['steps'][read_index]['encoded_result'] = str(int(read['encoded_result']) + 1)
    reject(broken, 'forged provider-dependent main value')
    if case == 10:
        for name in ('a', 'b'):
            assert instructions[traces[name]['steps'][-1]['statement']]['kind'] == 'return'
            broken = copy.deepcopy(certificate)
            trace_in(broken, name)['steps'].pop()
            reject(broken, 'atomic target omitted root Return')
    return count


def verify_zero_provider_execution(execution, source):
    certificate = copy.deepcopy(execution)
    program = certificate.pop('program')
    certificate['program_sha256'] = digest_json(program)
    accepted = check(program, certificate)
    assert accepted['status'] == 'ACCEPT', accepted
    traces = {trace['id']: trace for trace in certificate['traces']}
    assert set(traces) == {'main', 'a', 'b'}, traces.keys()
    assert traces['main']['entry'] == 'main'
    assert {traces[t['trace']]['entry'] for t in certificate['targets']} == {
        'worker_a', 'worker_b'}
    children = [traces['a'], traces['b']]
    assert all(child['creator_trace'] == 'main' for child in children)
    assert len({child['creator_step'] for child in children}) == 2
    instructions = program['instructions']
    assert not any(instructions[step['statement']]['kind'] == 'return'
                   for step in traces['main']['steps'])
    publisher = next(child for child in children if child['entry'] == 'worker_a')
    reader = next(child for child in children if child['entry'] == 'worker_b')
    reads = [(i, step) for i, step in enumerate(traces['main']['steps'])
             if instructions[step['statement']]['kind'] == 'load'
             and step.get('encoded_address', {}).get('object', {}).get('kind') == 'global'
             and step.get('encoded_address', {}).get('object', {}).get('id') == 'ready'
             and any(isinstance(source, dict) and source.get('trace') == publisher['id']
                     for source in step.get('read_from', []))]
    assert reads, 'zero-provider main lacks real read-from target A publication'
    i, read = reads[0]
    assert publisher['creator_step'] < i < reader['creator_step']
    assert int(read['encoded_result']) == 1
    sources = [item for item in read['read_from'] if isinstance(item, dict)]
    assert sources and all(item['trace'] == publisher['id'] for item in sources)
    count = 1 + verify_source_binding(program, certificate, source, 13)

    def reject(changed, label):
        nonlocal count
        result = check(program, changed)
        assert result['status'] == 'REJECT', (label, result)
        count += 1

    changed = copy.deepcopy(certificate)
    next(t for t in changed['traces'] if t['id'] == 'main')['steps'][i][
        'read_from'] = ['initial'] * len(read['read_from'])
    reject(changed, 'zero-provider main borrowed initial ready')
    changed = copy.deepcopy(certificate)
    next(t for t in changed['traces'] if t['id'] == reader['id'])[
        'creator_step'] = publisher['creator_step']
    reject(changed, 'zero-provider targets borrowed same create')
    changed = copy.deepcopy(certificate)
    write_index = sources[0]['step']
    next(t for t in changed['traces'] if t['id'] == publisher['id'])[
        'steps'][write_index]['clock'] = str(int(read['clock']) + 1)
    reject(changed, 'zero-provider publication occurs after actual main read')
    changed = copy.deepcopy(certificate)
    changed['traces'] = [t for t in changed['traces'] if t['id'] != publisher['id']]
    reject(changed, 'zero-provider omitted actual publishing target')
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--bits', choices=('32', '64', 'both'), default='both')
    parser.add_argument('--case', type=int, action='append')
    parser.add_argument('--evidence-dir', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    fixture = (ROOT / 'analyzer/test/regression/bmc-correctness/startup-provider.c').read_text()
    total = 0
    rows = []
    identity = runtime_identity(args.ikos.resolve())
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith('IKOS_') and key not in ('PYTHONPATH', 'PYTHONHOME')}
    environment.update(IKOS_BMC_RECOVER='1', IKOS_BMC_DIAGNOSTICS='1')
    if args.evidence_dir is not None:
        args.evidence_dir.mkdir(parents=True, exist_ok=False)
    workspace = (contextlib.nullcontext(str(args.evidence_dir))
                 if args.evidence_dir is not None else
                 tempfile.TemporaryDirectory(prefix='ikos-startup-provider-'))
    with workspace as temp:
        root = Path(temp)
        for bits in (('32', '64') if args.bits == 'both' else (args.bits,)):
            for case in range(14):
                if args.case and case not in args.case:
                    continue
                work = root / f'case{case}-{bits}'
                work.mkdir()
                source, report = work / 'input.c', work / 'report.json'
                assertions_before = total
                source.write_text(f'#define CASE {case}\n' + fixture)
                report.unlink(missing_ok=True)
                process = subprocess.run([
                    *runtime_command(args.ikos.resolve()), '--analyses=race', '--concurrency=auto',
                    '--partitioning=guards', '--demote-race-to-unknown', '-m' + bits,
                    '--bmc-deadline-ms=20000', '--bmc-solver-timeout-ms=1000',
                    '--format=json', '--report-file=' + str(report),
                    '-o', str(work / 'output.db'), str(source)],
                    cwd=work, env=environment,
                    capture_output=True, text=True, timeout=60)
                (work / 'stdout.txt').write_text(process.stdout)
                (work / 'stderr.txt').write_text(process.stderr)
                assert process.returncode == 0 and report.is_file(), (case, bits, process.stdout, process.stderr)
                data = json.loads(report.read_text())
                metadata = validate_race_result(data)
                executions = [row['info']['counterexample'] for row in data['reports']
                              if 'counterexample' in row.get('info', {})]
                expected = case in POSITIVE
                assert (metadata['verdict'] == 'FALSE') == expected and bool(executions) == expected, (
                    case, bits, metadata, process.stdout, process.stderr)
                if not expected:
                    assert metadata['verdict'] in ('SAFE', 'UNKNOWN'), (case, bits, metadata)
                    assert metadata['analysis_complete'] and metadata['checks_complete'], (
                        case, bits, 'incomplete analysis cannot pass a negative', metadata)
                if expected:
                    for execution in executions:
                        total += (verify_zero_provider_execution(execution, source)
                                  if case == 13 else
                                  verify_execution(execution, case, source=source))
                    counts = [row.get('info', {}).get('bmc', {}).get('counts', {}) for row in data['reports']]
                    assert any(c.get('provider_timeline_confirmed', 0) for c in counts), counts
                total += 1
                rows.append(dict(case=case, bits=int(bits), verdict=metadata['verdict'],
                                 assertions=total-assertions_before, executions=len(executions),
                                 source=str(source), report=str(report),
                                 source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                                 report_sha256=hashlib.sha256(report.read_bytes()).hexdigest()))
                print(f'startup-provider case={case} bits={bits}: {metadata["verdict"]}', flush=True)
    assert runtime_identity(args.ikos.resolve()) == identity
    if args.output is not None:
        args.output.write_text(json.dumps(dict(runtime=identity, assertions=total, rows=rows),
                                          indent=2) + '\n')
    print(f'startup-provider: {total} analyzer/replay assertions passed')


if __name__ == '__main__':
    main()
