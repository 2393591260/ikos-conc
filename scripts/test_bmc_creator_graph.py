#!/usr/bin/env python3
"""Exercise complete creator graphs and corrupt actual certificates; default both ABIs."""
import argparse
import copy
import json
import os
import signal
from pathlib import Path
import subprocess
import sys
import tempfile
from ikos_runtime import runtime_command
from validate_bmc_execution import check, digest_json

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from analyzer.python.ikos.race_result import validate_race_result
from svcomp_witness import build_witness


def inject_resolved_call(program, certificate, create_statement, api):
    """Insert an actual indirect call before a retained Create, without a child binding.

    Add both the function-valued definition and call to the real CFG and every
    executed occurrence. Preserve all existing step references and relative
    clocks so rejection must reach the resolved-dispatch boundary.
    """
    p, c = copy.deepcopy(program), copy.deepcopy(certificate)
    original = p['instructions'][create_statement]
    constant = copy.deepcopy(original['operands'][0])
    create_api = constant['id']
    constant['id'] = api
    if api not in p['functions']:
        # Adversarial snapshot: the scope check must run before API arity/model
        # checks. The copied declaration is not a source-level ABI claim.
        p['functions'][api] = copy.deepcopy(p['functions'][create_api])
    value = dict(kind='variable', id=create_statement + ':g0-resolved-callee', type=constant['type'])
    adapter_id, call_id = create_statement + ':g0-adapter', create_statement + ':g0-indirect-call'
    assert adapter_id not in p['instructions'] and call_id not in p['instructions']
    p['instructions'][adapter_id] = dict(kind='assign', result=value, operands=[constant])
    injected = copy.deepcopy(original); injected['operands'][0] = value
    if 'result' in injected:
        injected['result']['id'] += ':g0-injected'
    p['instructions'][call_id] = injected
    owners = 0
    for function in p['functions'].values():
        for block in function.get('blocks', {}).values():
            if create_statement in block['statements']:
                at = block['statements'].index(create_statement)
                block['statements'][at:at] = [adapter_id, call_id]
                owners += 1
    assert owners == 1
    remap = {}
    for trace in c['traces']:
        additions = 0
        for index, step in enumerate(trace['steps']):
            if step['statement'] == create_statement:
                additions += 2
            remap[(trace['id'], index)] = index + additions
    def references(node):
        if isinstance(node, dict):
            if 'trace' in node and 'step' in node and (node['trace'], node['step']) in remap:
                node['step'] = remap[(node['trace'], node['step'])]
            if 'creator_trace' in node and 'creator_step' in node:
                node['creator_step'] = remap[(node['creator_trace'], node['creator_step'])]
            for value in node.values(): references(value)
        elif isinstance(node, list):
            for value in node: references(value)
    references(c)
    for trace in c['traces']:
        steps = []
        for step in trace['steps']:
            clock = int(step['clock']) * 3 + 3
            if step['statement'] == create_statement:
                steps.append(dict(statement=adapter_id, clock=str(clock - 2)))
                extra = copy.deepcopy(step); extra['statement'] = call_id; extra['clock'] = str(clock - 1)
                steps.append(extra)
            step['clock'] = str(clock); steps.append(step)
        trace['steps'] = steps
    c['program_sha256'] = digest_json(p)
    return p, c


def corruptions(execution):
    cert = copy.deepcopy(execution)
    program = cert.pop('program')
    cert['program_sha256'] = digest_json(program)
    assert check(program, cert)['status'] == 'ACCEPT'
    traces = {t['id']: t for t in cert['traces']}
    nested = next(t for t in cert['traces'] if t.get('creator_trace') in traces and
                  traces[t['creator_trace']]['entry'] != 'main')
    checks = 1

    def rejected(mutated):
        nonlocal checks
        result = check(program, mutated)
        assert result['status'] == 'REJECT', result
        checks += 1

    def changed_trace(c, name):
        return next(t for t in c['traces'] if t['id'] == name)

    c = copy.deepcopy(cert)
    duplicate = copy.deepcopy(nested); duplicate['id'] = 'aux_987'
    c['traces'].append(duplicate); rejected(c)
    c = copy.deepcopy(cert); changed_trace(c, nested['id'])['creator_trace'] = nested['id']; rejected(c)
    c = copy.deepcopy(cert); changed_trace(c, nested['id'])['creator_step'] = -1; rejected(c)
    c = copy.deepcopy(cert); changed_trace(c, nested['id'])['creator_step'] += 1; rejected(c)
    c = copy.deepcopy(cert); changed_trace(c, nested['id'])['entry'] = 'main'; rejected(c)
    c = copy.deepcopy(cert)
    child = changed_trace(c, nested['id'])
    parent = changed_trace(c, child['creator_trace'])
    child['steps'][0]['clock'] = str(int(parent['steps'][child['creator_step']]['clock']) - 1)
    rejected(c)
    for field, value in [('width', 1), ('alignment', 1), ('offset', '1')]:
        c = copy.deepcopy(cert)
        create = changed_trace(c, nested['creator_trace'])['steps'][nested['creator_step']]
        create['encoded_address'][field] = value; rejected(c)
    c = copy.deepcopy(cert)
    create = changed_trace(c, nested['creator_trace'])['steps'][nested['creator_step']]
    del create['encoded_address']; rejected(c)
    child_roots = [t for t in cert['traces'] if 'creator_trace' in t]
    equal_parents = [(a, b) for i, a in enumerate(child_roots) for b in child_roots[i + 1:]
                     if a['creator_trace'] != b['creator_trace'] and
                     traces[a['creator_trace']]['entry'] == traces[b['creator_trace']]['entry']]
    for first, second in equal_parents[:1]:
        c = copy.deepcopy(cert)
        child = changed_trace(c, first['id'])
        child['creator_trace'], child['creator_step'] = second['creator_trace'], second['creator_step']
        rejected(c)  # one dynamic occurrence cannot bind two same-static children
    for field, value in [('declaration', False), ('child_parameters', 0), ('special_parameter_abi', True),
                         ('ordinary_abi', False), ('child_ordinary_abi', False),
                         ('ordinary_abi', None), ('child_ordinary_abi', None)]:
        altered_program = copy.deepcopy(program)
        sid = traces[nested['creator_trace']]['steps'][nested['creator_step']]['statement']
        if value is None: altered_program['instructions'][sid]['native_create'].pop(field, None)
        else: altered_program['instructions'][sid]['native_create'][field] = value
        c = copy.deepcopy(cert); c['program_sha256'] = digest_json(altered_program)
        result = check(altered_program, c)
        assert result['status'] == 'REJECT', result
        checks += 1
    # Once's independent all-call inventory may refuse the injected adapter
    # before dynamic dispatch. Exercise the precise dispatch seam on graph-only
    # controls; once controls retain their own complete callback checks.
    if not any('once_role' in step for trace in cert['traces'] for step in trace['steps']):
        # These added indirect occurrences are absent from the static Create set.
        # Require the precise resolved boundary, not an earlier broken-CFG refusal.
        sid = traces[nested['creator_trace']]['steps'][nested['creator_step']]['statement']
        create_api = program['instructions'][sid]['operands'][0]['id']
        for api in (create_api, 'pthread_join', 'pthread_exit', 'malloc', 'pthread_mutexattr_settype'):
            altered_program, c = inject_resolved_call(program, cert, sid, api)
            for metadata in (True, False):
                tested = copy.deepcopy(c)
                if not metadata: tested.pop('creator_graph', None)
                result = check(altered_program, tested)
                expected_reason = ('creator graph direct actual Create' if api == create_api else
                                   'creator graph lifecycle/synchronization scope')
                assert result['status'] in ('REJECT', 'UNSUPPORTED') and result.get('reason') == expected_reason, result
                checks += 1
    c = copy.deepcopy(cert); c['targets'][1] = copy.deepcopy(c['targets'][0]); rejected(c)
    c = copy.deepcopy(cert); c['traces'] = [t for t in c['traces'] if t['id'] != nested['id']]; rejected(c)
    main_statements = [sid for block in program['functions']['main']['blocks'].values() for sid in block['statements']]
    main_return = next(sid for sid in main_statements if program['instructions'][sid]['kind'] == 'return')
    c = copy.deepcopy(cert)
    root = changed_trace(c, 'main')
    root['steps'].append(dict(statement=main_return,
        clock=str(max(int(s['clock']) for t in c['traces'] for s in t['steps']) + 1)))
    rejected(c)
    c = copy.deepcopy(cert); c.pop('creator_graph', None)
    assert check(program, c)['status'] == 'ACCEPT'  # structural detection cannot be bypassed
    checks += 1
    for trace in cert['traces']:
        for index, step in enumerate(trace['steps']):
            if step.get('once_callback_return'):
                c = copy.deepcopy(cert); del changed_trace(c, trace['id'])['steps'][index]; rejected(c)
            if step.get('synthetic_event') == 'once_return':
                c = copy.deepcopy(cert); del changed_trace(c, trace['id'])['steps'][index]; rejected(c)
            if step.get('once_role') == 'active':
                c = copy.deepcopy(cert); changed_trace(c, trace['id'])['steps'][index]['once_role'] = 'passive'; rejected(c)
    # A callback-local handle dies at the real callback Return. Its child is
    # still a live independent physical thread and gets no completion clock.
    if any(s.get('once_callback_return') for t in cert['traces'] for s in t['steps']):
        c = copy.deepcopy(cert)
        last = max(int(s['clock']) for t in c['traces'] if t['id'] != nested['id'] for s in t['steps'])
        for i, step in enumerate(changed_trace(c, nested['id'])['steps']): step['clock'] = str(last + i + 1)
        assert check(program, c)['status'] == 'ACCEPT'
        checks += 1
    return checks


def run_owned_command(command, cwd, env, timeout):
    """A timed-out test owns and terminates its complete child process group."""
    options = dict(cwd=cwd, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if os.name == 'nt': options['creationflags'] = subprocess.CREATE_NEW_PROCESS_GROUP
    else: options['start_new_session'] = True
    process = subprocess.Popen(command, **options)
    timed_out = False
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        if os.name == 'nt':
            subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        else:
            try: os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError: pass
        try:
            stdout, stderr = process.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            if os.name == 'nt': process.kill()
            else:
                try: os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError: pass
            stdout, stderr = process.communicate()
        # A child may close inherited pipes before ignoring SIGTERM. Kill any
        # remaining members even when communicate already reaped the leader.
        if os.name != 'nt':
            try: os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError: pass
    return subprocess.CompletedProcess(command, process.returncode, stdout, stderr), timed_out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--bits', choices=('32', '64', 'both'), default='both')
    parser.add_argument('--source-1035', type=Path, help='Original 1035, always its manifest LP64 model')
    parser.add_argument('--source-1071', type=Path, help='Original 1071, always its manifest LP64 model')
    parser.add_argument('--evidence-json', type=Path, help='Persist both arm verdicts and actual report diagnostics')
    parser.add_argument('--fixture', action='append', choices=('creator-graph', 'once-creator-graph',
        'same-static', 'frame-controls', 'original1035', 'original1071'))
    parser.add_argument('--case', type=int, action='append', help='Run only these handcrafted CASE values')
    parser.add_argument('--artifacts', type=Path, help='Retain exact inputs, reports and stdout/stderr; defaults to a persistent temp directory')
    args = parser.parse_args()
    checks = 0
    observations = []
    fixtures = ROOT / 'analyzer/test/regression/bmc-creator-graph'
    cases = [(fixtures/'creator-graph.c', i, 'race' if i in (0, 3) else 'safe') for i in range(5)]
    cases += [(fixtures/'once-creator-graph.c', i, 'race' if i == 0 else 'safe') for i in range(5)]
    cases += [(fixtures/'same-static.c', 0, 'race'), (fixtures/'same-static.c', 1, 'safe'),
              (fixtures/'same-static.c', 2, 'unsupported')]
    cases += [(fixtures/'frame-controls.c', 0, 'covered')]
    cases += [(fixtures/'frame-controls.c', i, 'unsupported') for i in range(1, 6)]
    selected = set(args.fixture or ())
    cases = [(path, case, expected) for path, case, expected in cases
             if (not selected or path.stem in selected) and (not args.case or case in args.case)]
    plan = [(bits, path, case, expected) for bits in (('32', '64') if args.bits == 'both' else (args.bits,))
            for path, case, expected in cases]
    for identifier, path in (('original1035', args.source_1035), ('original1071', args.source_1071)):
        if identifier in selected and not path: parser.error(identifier + ' requires its --source path')
        if path and (not selected or identifier in selected):
            plan.append(('64', path.resolve(), 0, 'race'))
    if not plan: parser.error('no selected cases')
    artifacts = args.artifacts.resolve() if args.artifacts else Path(tempfile.mkdtemp(prefix='ikos-creator-graph-evidence-'))
    artifacts.mkdir(parents=True, exist_ok=True)
    (artifacts/'inputs').mkdir(exist_ok=True)
    evidence_path = args.evidence_json or artifacts/'evidence.json'
    print('creator graph evidence: ' + str(artifacts), file=sys.stderr)
    current = {}
    try:
        with tempfile.TemporaryDirectory(prefix='ikos-creator-graph-work-') as temp:
            work = Path(temp)
            for bits, original_source, case, expected in plan:
                # Bind source mapping to the same preprocessor selection. The
                # mapper invokes Clang on this exact source without -DCASE.
                source = original_source
                if original_source.parent == fixtures:
                    source = artifacts/'inputs'/(original_source.stem + '-' + str(case) + '.c')
                    source.write_text(original_source.read_text().replace('#define CASE 0', '#define CASE '+str(case)))
                reports = {}
                # Change one diagnostic switch only; all budgets stay fixed.
                for disabled in (False, True):
                    label = str(bits) + '-' + original_source.stem + '-case' + str(case) + ('-off' if disabled else '-on')
                    arm_dir = artifacts/label; arm_dir.mkdir(exist_ok=True)
                    report = arm_dir/'report.json'; report.unlink(missing_ok=True)
                    current = dict(bits=bits, source=str(original_source), case=case, disabled=disabled, artifacts=str(arm_dir))
                    env = dict(os.environ, IKOS_BMC_RECOVER='1')
                    env.pop('IKOS_BMC_DISABLE_CREATOR_GRAPH', None)
                    if disabled: env['IKOS_BMC_DISABLE_CREATOR_GRAPH'] = '1'
                    command = runtime_command(args.ikos.resolve()) + [
                        '--analyses=race', '--concurrency=auto', '--demote-race-to-unknown',
                        '-m', bits, '--format=json',
                        '--report-file='+str(report), '-o', str(work/'output.db'), str(source)]
                    (arm_dir/'command.json').write_text(json.dumps(dict(command=command, disabled=disabled,
                        wall_timeout=30, model='ILP32' if bits == '32' else 'LP64'), indent=2) + '\n')
                    proc, timed_out = run_owned_command(command, work, env, 30)
                    (arm_dir/'stdout.log').write_text(proc.stdout)
                    (arm_dir/'stderr.log').write_text(proc.stderr)
                    assert not timed_out, (current, 'wall timeout; owned process group terminated')
                    assert proc.returncode == 0, (current, proc.returncode, proc.stdout, proc.stderr)
                    data = json.loads(report.read_text())
                    verdict = validate_race_result(data)['verdict']
                    executions = [r['info']['counterexample'] for r in data['reports']
                                  if 'counterexample' in r.get('info', {})]
                    graphs = [e for e in executions if e.get('creator_graph') is True]
                    if verdict == 'FALSE':
                        assert executions, (bits, source, case, 'FALSE without independent execution')
                    # An old path may legitimately remain FALSE in the off arm.
                    # Every retained execution, graph or legacy, must replay.
                    for execution in executions:
                        c = copy.deepcopy(execution); p = c.pop('program'); c['program_sha256'] = digest_json(p)
                        replay = check(p, c)
                        assert replay['status'] == 'ACCEPT', (current, replay)
                        checks += 1
                    observations.append(dict(bits=bits, source=str(original_source), case=case,
                        disabled=disabled, verdict=verdict, executions=len(executions), graphs=len(graphs),
                        diagnostics=[{k: v for k, v in row.get('info', {}).items()
                                      if k != 'counterexample'} for row in data.get('reports', [])],
                        stdout=proc.stdout, stderr=proc.stderr))
                    if disabled: assert not graphs
                    elif expected == 'race':
                        assert verdict == 'FALSE' and graphs, (bits, source, case, verdict)
                        for execution in graphs:
                            roots = {t['id']: t['entry'] for t in execution['traces']}
                            edges = {(roots[t['creator_trace']], t['entry']) for t in execution['traces']
                                     if 'creator_trace' in t}
                            if args.source_1035 and original_source == args.source_1035.resolve():
                                assert set(roots.values()) == {'main', 't1', 't2', 't4'} and edges == {
                                    ('main', 't1'), ('main', 't2'), ('t2', 't4')}, (current, roots, edges)
                            if args.source_1071 and original_source == args.source_1071.resolve():
                                assert set(roots.values()) == {'main', 't_fun', 't_other'} and edges == {
                                    ('main', 't_fun'), ('t_fun', 't_other')}, (current, roots, edges)
                            checks += corruptions(execution)
                            witness = build_witness(str(source), {'counterexample': execution},
                                'ILP32' if bits == '32' else 'LP64', str(args.ikos.resolve()))
                            traces = {t['id']: t for t in execution['traces']}
                            children = {(t['creator_trace'], t['creator_step']): t['id']
                                        for t in execution['traces'] if 'creator_trace' in t}
                            ordered = sorted((int(traces[parent]['steps'][step]['clock']), parent, step, child)
                                             for (parent, step), child in children.items())
                            ids, expected_parents = {'main': 0}, []
                            for _, parent, step, child in ordered:
                                expected_parents.append(ids[parent]); ids[child] = len(ids)
                            content = witness[0]['content']
                            assert [segment['segment'][0]['waypoint']['thread_id'] for segment in content[:-1]] == expected_parents
                            assert [point['waypoint']['thread_id'] for point in content[-1]['segment']] == [
                                ids[t['trace']] for t in execution['targets']]
                            checks += 1
                    elif expected == 'covered':
                        assert verdict == 'FALSE' and executions, (bits, source, case, verdict)
                        for execution in executions:
                            c = copy.deepcopy(execution); p = c.pop('program'); c['program_sha256'] = digest_json(p)
                            assert check(p, c)['status'] == 'ACCEPT'
                    elif expected == 'safe': assert verdict != 'FALSE', (bits, source, case, verdict)
                    else: assert not graphs, (bits, source, case, verdict)
                    if expected == 'safe': assert verdict != 'FALSE'
                    reports[disabled] = verdict
                    checks += 1
                # No verdict-drop requirement: the switch isolates graph evidence.
    except Exception as error:
        (artifacts/'failure.json').write_text(json.dumps(dict(current=current,
            exception=type(error).__name__, reason=str(error)), indent=2) + '\n')
        raise
    finally:
        evidence_path.parent.mkdir(parents=True, exist_ok=True)
        evidence_path.write_text(json.dumps(dict(checks=checks, bits=args.bits, observations=observations,
            artifacts=str(artifacts)), indent=2) + '\n')
    print(json.dumps(dict(checks=checks, bits=args.bits, artifacts=str(artifacts), evidence=str(evidence_path))))


if __name__ == '__main__':
    main()
