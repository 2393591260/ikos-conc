#!/usr/bin/env python3
"""Complete main/helper allocation occurrences; both-ABI controls, original629 ILP32 only."""
import argparse
import copy
import json
import os
from pathlib import Path
import sys
import tempfile
from ikos_runtime import runtime_command
from test_bmc_creator_graph import run_owned_command
from validate_bmc_execution import NativeSCExecution, Replay, check, digest_json

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from analyzer.python.ikos.race_result import validate_race_result
from svcomp_witness import build_witness

PROFILE = 'main-helper-occurrences-v1'


def corruption_checks(execution):
    c = copy.deepcopy(execution); p = c.pop('program'); c['program_sha256'] = digest_json(p)
    assert check(p, c)['status'] == 'ACCEPT'
    calls = [(t['id'], i) for t in c['traces'] for i, s in enumerate(t['steps']) if 'heap_allocation' in s]
    assert calls
    checks = 1
    def step(cert, occurrence):
        return next(t for t in cert['traces'] if t['id'] == occurrence[0])['steps'][occurrence[1]]
    def reject(cert, program=p):
        nonlocal checks
        result = check(program, cert)
        assert result['status'] in ('REJECT', 'UNSUPPORTED'), result
        checks += 1
    for field in ('schema', 'profile', 'allocator_trace', 'allocator_step', 'invocation_calls', 'extent', 'outcome'):
        bad = copy.deepcopy(c); step(bad, calls[0])['heap_allocation'].pop(field, None); reject(bad)
    for field, value in [('schema', True), ('allocator_trace', 'invented'), ('allocator_step', -1),
                         ('allocator_step', True), ('invocation_calls', []),
                         ('invocation_calls', [True]), ('extent', '0'), ('outcome', 'invented')]:
        bad = copy.deepcopy(c); step(bad, calls[0])['heap_allocation'][field] = value; reject(bad)
    # Removing the optional top-level marker cannot remove the real helper call.
    stripped = copy.deepcopy(c); stripped.pop('heap_profile', None)
    assert check(p, stripped)['status'] == 'ACCEPT'; checks += 1
    for marker in (True, False):
        bad = copy.deepcopy(c)
        if not marker: bad.pop('heap_profile', None)
        for occurrence in calls: step(bad, occurrence).pop('heap_allocation', None)
        reject(bad)
    if len(calls) >= 2:
        bad = copy.deepcopy(c)
        step(bad, calls[0])['heap_allocation']['invocation_calls'] = copy.deepcopy(
            step(bad, calls[1])['heap_allocation']['invocation_calls'])
        reject(bad)
    # Every positive uses either safe_alloc's real non-NULL guard or the real
    # allocation-failure branch. Flipping its selected outcome must break CFG.
    bad = copy.deepcopy(c); record = step(bad, calls[0])['heap_allocation']
    record['outcome'] = 'null' if record['outcome'] == 'success' else 'success'; reject(bad)
    sid = step(c, calls[0])['statement']
    name = p['instructions'][sid]['operands'][0]['id']
    native_key = 'native_calloc' if 'calloc' in name else 'native_malloc'
    for field, value in [('ordinary_abi', False), ('source_operands_bound', False),
                         ('argument_values', []), ('parameter_bits', [16])]:
        altered = copy.deepcopy(p); altered['instructions'][sid][native_key][field] = value
        bad = copy.deepcopy(c); bad['program_sha256'] = digest_json(altered); reject(bad, altered)
    for trace in c['traces']:
        heap = next((i for i, s in enumerate(trace['steps'])
                     if s.get('encoded_address', {}).get('object', {}).get('kind') == 'heap'), None)
        if heap is None: continue
        for field, value in [('allocation_event', dict(trace=trace['id'], step=-1)),
                             ('occurrence', 999), ('invocation_calls', []), ('heap_profile', 'legacy')]:
            bad = copy.deepcopy(c)
            step(bad, (trace['id'], heap))['encoded_address']['object'][field] = value; reject(bad)
        bad = copy.deepcopy(c)
        step(bad, (trace['id'], heap))['encoded_address']['offset'] = '1'; reject(bad)
        if len(calls) >= 2:
            bad = copy.deepcopy(c)
            address = step(bad, (trace['id'], heap))['encoded_address']['object']
            actual = address['allocation_event']
            other = next(call for call in calls if call != (actual['trace'], actual['step']))
            statement = step(c, other)['statement']
            ordinal = sum(call[0] == other[0] and call[1] <= other[1] and
                          step(c, call)['statement'] == statement for call in calls)
            address.update(allocation=statement, occurrence=ordinal,
                allocation_event=dict(trace=other[0], step=other[1]),
                invocation_calls=step(c, other)['heap_allocation']['invocation_calls'])
            reject(bad)
        break
    # Remove every retained main-helper Return separately. Reindex provenance
    # so a stale target/allocator index cannot mask the actual CFG/frame check.
    def erase_return(cert, trace_id, index):
        next(t for t in cert['traces'] if t['id'] == trace_id)['steps'].pop(index)
        def shifted(value):
            assert value != index, 'a Return must not be used as an allocation/creation/read source'
            return value - (value > index)
        def walk(value):
            if isinstance(value, list):
                for child in value: walk(child)
            elif isinstance(value, dict):
                if value.get('trace') == trace_id and type(value.get('step')) is int:
                    value['step'] = shifted(value['step'])
                if value.get('creator_trace') == trace_id:
                    value['creator_step'] = shifted(value['creator_step'])
                if value.get('allocator_trace') == trace_id:
                    value['allocator_step'] = shifted(value['allocator_step'])
                    value['invocation_calls'] = [shifted(i) for i in value['invocation_calls']]
                elif value.get('allocation_event', {}).get('trace') == trace_id and 'invocation_calls' in value:
                    value['invocation_calls'] = [shifted(i) for i in value['invocation_calls']]
                for child in value.values(): walk(child)
        walk(cert)
    returned = [(t['id'], i) for t in c['traces'] if t['entry'] == 'main'
                for i, s in enumerate(t['steps']) if p['instructions'][s['statement']]['kind'] == 'return']
    assert returned, 'positive helper allocation must really Return to main'
    for trace_id, index in returned:
        bad = copy.deepcopy(c); erase_return(bad, trace_id, index); reject(bad)
    return checks



def mixed_replay_checks(execution):
    original = copy.deepcopy(execution)
    program = original.pop('program')
    original['program_sha256'] = digest_json(program)
    assert check(program, original)['status'] == 'ACCEPT'
    extended = copy.deepcopy(original)
    main = next(t for t in extended['traces'] if t['entry'] == 'main')
    instructions = program['instructions']
    last = main['steps'][-1]['statement']
    block = program['functions']['main']['blocks'][instructions[last]['block']]
    tail = block['statements'][block['statements'].index(last) + 1:]
    clock = max(int(step['clock']) for t in extended['traces'] for step in t['steps'])
    native = []
    for sid in tail:
        inst = instructions[sid]
        if inst['kind'] == 'return':
            break
        clock += 1
        main['steps'].append(dict(statement=sid, clock=str(clock)))
        if inst.get('native_sc'):
            native.append(sid)
    assert len(native) == 1 and instructions[native[0]]['native_sc']['operation'] == 'store'
    # Bypass only the constructor's deliberately unsupported combination in
    # this positive test; normal execution still checks CFG/frames/values/RF.
    replay = Replay(program, copy.deepcopy(original))
    replay.certificate = extended
    replay.traces = {t['id']: t for t in extended['traces']}
    replay.native_sc = NativeSCExecution(replay)
    assert replay.native_sc.groups
    assert replay.run()['status'] == 'ACCEPT'
    for stripped in (0, 1, 2):
        certificate = copy.deepcopy(extended)
        if stripped:
            certificate.pop('heap_profile', None)
        if stripped == 2:
            # This last arm checks discovery from actual helper instructions;
            # missing outcome records are not a complete executable certificate.
            for trace in certificate['traces']:
                for step in trace['steps']:
                    step.pop('heap_allocation', None)
        result = check(program, certificate)
        assert result == dict(status='REJECT', reason='heap helper/native SC mixed profile'), result
    return 5


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT/'install/bin/ikos')
    parser.add_argument('--bits', choices=('32', '64', 'both'), default='both')
    parser.add_argument('--source-629', type=Path, help='Unmodified original629, always manifest ILP32')
    parser.add_argument('--fixture', choices=('helper-occurrences', 'wrong-abi', 'worker-scope', 'native-scope', 'library-scope', 'original629'), action='append')
    parser.add_argument('--case', type=int, action='append')
    parser.add_argument('--artifacts', type=Path)
    args = parser.parse_args()
    fixtures = ROOT/'analyzer/test/regression/bmc-heap-helper-occurrences'
    selected = set(args.fixture or ())
    cases = [(fixtures/'helper-occurrences.c', i,
              'race' if i in (0, 3, 5, 13, 15, 20) else 'safe' if i in (1, 2, 4, 6, 7, 14) else 'unsupported')
             for i in range(21) if i != 11 and (not args.case or i in args.case)]
    if not args.case: cases.append((fixtures/'wrong-abi.c', 0, 'unsupported'))
    cases += [(fixtures/'worker-scope.c', i, 'safe' if i == 0 else 'unsupported')
              for i in (0, 1) if not args.case or i in args.case]
    cases += [(fixtures/'native-scope.c', i, 'unsupported')
              for i in range(4) if not args.case or i in args.case]
    cases += [(fixtures/'library-scope.c', i, 'unsupported' if i == 0 else 'race')
              for i in (0, 1) if not args.case or i in args.case]
    cases = [c for c in cases if not selected or c[0].stem in selected]
    plan = [(bits, path, case, expected) for bits in (('32', '64') if args.bits == 'both' else (args.bits,))
            for path, case, expected in cases]
    if 'original629' in selected and not args.source_629: parser.error('original629 requires --source-629')
    if args.source_629 and (not selected or 'original629' in selected):
        plan.append(('32', args.source_629.resolve(), 0, 'race'))
    if not plan: parser.error('no selected cases')
    artifacts = args.artifacts.resolve() if args.artifacts else Path(tempfile.mkdtemp(prefix='ikos-heap-helper-evidence-'))
    artifacts.mkdir(parents=True, exist_ok=True); (artifacts/'inputs').mkdir(exist_ok=True)
    print('heap helper evidence: '+str(artifacts), file=sys.stderr)
    observations, checks, current = [], 0, {}
    try:
        with tempfile.TemporaryDirectory(prefix='ikos-heap-helper-work-') as temp:
            work = Path(temp)
            for bits, original, case, expected in plan:
                source = original
                if original.parent == fixtures:
                    source = artifacts/'inputs'/(original.stem+'-'+str(case)+'.c')
                    source.write_text(original.read_text().replace('#define CASE 0', '#define CASE '+str(case)))
                for disabled in (False, True):
                    label = bits+'-'+original.stem+'-case'+str(case)+('-off' if disabled else '-on')
                    folder = artifacts/label; folder.mkdir(exist_ok=True)
                    report = folder/'report.json'; report.unlink(missing_ok=True)
                    current = dict(bits=bits, source=str(original), case=case, disabled=disabled, artifacts=str(folder))
                    env = dict(os.environ, IKOS_BMC_RECOVER='1', IKOS_BMC_DIAGNOSTICS='1')
                    env.pop('IKOS_BMC_DISABLE_HEAP_HELPER_OCCURRENCES', None)
                    if disabled: env['IKOS_BMC_DISABLE_HEAP_HELPER_OCCURRENCES'] = '1'
                    command = runtime_command(args.ikos.resolve()) + ['--analyses=race', '--concurrency=auto',
                        '--demote-race-to-unknown', '-m', bits, '--format=json', '--report-file='+str(report),
                        '-o', str(work/'output.db'), str(source)]
                    (folder/'command.json').write_text(json.dumps(dict(command=command, wall_timeout=30,
                        model='ILP32' if bits == '32' else 'LP64'), indent=2)+'\n')
                    proc, timeout = run_owned_command(command, work, env, 30)
                    (folder/'stdout.log').write_text(proc.stdout); (folder/'stderr.log').write_text(proc.stderr)
                    assert not timeout, (current, 'wall timeout; owned process group terminated')
                    assert proc.returncode == 0, (current, proc.returncode, proc.stderr)
                    data = json.loads(report.read_text()); verdict = validate_race_result(data)['verdict']
                    executions = [r['info']['counterexample'] for r in data['reports']
                                  if 'counterexample' in r.get('info', {})]
                    profiles = [e for e in executions if e.get('heap_profile') == PROFILE]
                    scope_hits = sum(row.get('info', {}).get('bmc', {}).get('counts', {}).get(
                        'heap_helper_native_sc_scope', 0) for row in data['reports'])
                    if original.stem == 'native-scope' and not disabled:
                        assert scope_hits > 0, (current, 'native/helper mixed-scope guard not reached')
                        checks += 1
                    if verdict == 'FALSE': assert executions, (current, 'FALSE without execution')
                    for execution in executions:
                        c = copy.deepcopy(execution); p = c.pop('program'); c['program_sha256'] = digest_json(p)
                        result = check(p, c); assert result['status'] == 'ACCEPT', (current, result); checks += 1
                    observations.append(dict(**current, verdict=verdict, executions=len(executions), profiles=len(profiles), native_scope_hits=scope_hits,
                        diagnostics=[{k: v for k, v in row.get('info', {}).items() if k != 'counterexample'}
                                     for row in data['reports']]))
                    if disabled: assert not profiles, current
                    elif expected == 'race':
                        assert verdict == 'FALSE' and profiles, (current, verdict)
                        for execution in profiles:
                            checks += corruption_checks(execution)
                            if original.stem == 'helper-occurrences' and case == 20:
                                checks += mixed_replay_checks(execution)
                            build_witness(str(source), {'counterexample': execution},
                                          'ILP32' if bits == '32' else 'LP64', str(args.ikos.resolve()))
                            checks += 1
                            if original.stem == 'helper-occurrences' and case in (0, 13, 15):
                                calls = [(t['id'], i, s) for t in execution['traces']
                                         for i, s in enumerate(t['steps']) if 'heap_allocation' in s]
                                assert len(calls) == 2 and calls[0][2]['statement'] == calls[1][2]['statement']
                                assert calls[0][2]['heap_allocation']['invocation_calls'] != calls[1][2]['heap_allocation']['invocation_calls']
                                checks += 1
                    elif expected == 'safe': assert verdict != 'FALSE', (current, verdict)
                    else: assert not profiles, current
                    if expected == 'safe': assert verdict != 'FALSE', (current, verdict)
                    checks += 1
    except Exception as error:
        (artifacts/'failure.json').write_text(json.dumps(dict(current=current, exception=type(error).__name__,
            reason=str(error)), indent=2)+'\n'); raise
    finally:
        (artifacts/'evidence.json').write_text(json.dumps(dict(checks=checks, observations=observations), indent=2)+'\n')
    print(json.dumps(dict(checks=checks, bits=args.bits, artifacts=str(artifacts))))


if __name__ == '__main__':
    main()
