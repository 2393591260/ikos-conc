#!/usr/bin/env python3
"""Dual-ABI once contract and damaged-execution controls.

Run only after integrating/reviewing/building the patch. This file imports the
candidate's checker/wrapper, never the outside draft's unbuilt implementation.
The default matrix is both ABIs; --with-ablation repeats exactly the same input
and budget with the new pass disabled. Export checks are not official validation.
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

POSITIVE = {0, 1, 2, 5, 7, 14, 20}
NEGATIVE = {3, 4, 6, 8, 9, 11, 12, 13, 15, 16, 17}
FINITE_POSITIVE = {0, 1, 11, 12, 18, 21, 22}
FINITE_NEGATIVE = {2, 3, 4, 13, 23}
FINITE_REFUSE = {5, 6, 7, 8, 9, 10, 14, 15, 16, 17, 19, 20}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def verify(execution, case, source, check, digest_json, source_binding):
    certificate = copy.deepcopy(execution)
    program = certificate.pop('program')
    certificate['program_sha256'] = digest_json(program)
    accepted = check(program, certificate)
    assert accepted['status'] == 'ACCEPT', (case, accepted)
    traces = {t['id']: t for t in certificate['traces']}
    instructions = program['instructions']
    roots = [t for t in traces.values() if t['entry'] == 'main']
    assert len(traces) == 2 and set(traces) == {'a', 'b'} and len(roots) == 1
    main = roots[0]
    child = next(t for t in traces.values() if t is not main)
    assert child['creator_trace'] == main['id']
    assert child['entry'] != 'main' and all(t['entry'] != 'init' for t in traces.values())
    main_statements = {sid for block in program['functions']['main']['blocks'].values()
                       for sid in block['statements']}
    assert not any(s['statement'] in main_statements and instructions[s['statement']]['kind'] == 'return'
                   for s in main['steps'])
    creates = [(i, s) for i, s in enumerate(main['steps'])
               if instructions[s['statement']].get('kind') == 'call' and
               instructions[s['statement']].get('operands', [{}])[0].get('id') in
               ('pthread_create', 'ar.pthread.create')]
    assert len(creates) == 1 and creates[0][0] == child['creator_step']
    calls = [(t['id'], i) for t in traces.values() for i, s in enumerate(t['steps'])
             if 'once_role' in s]
    assert calls
    count = 1 + source_binding(program, certificate, source, case)
    damage = []

    def trace(cert, name):
        return next(t for t in cert['traces'] if t['id'] == name)

    def damaged(label, mutate, mutate_program=False):
        nonlocal count
        cp, cc = copy.deepcopy(program), copy.deepcopy(certificate)
        mutate(cp, cc)
        if mutate_program:
            cc['program_sha256'] = digest_json(cp)
        result = check(cp, cc)
        assert result['status'] != 'ACCEPT', (case, label, result)
        damage.append(dict(label=label, status=result['status'], reason=result.get('reason')))
        count += 1

    def step(cert, name, i):
        return trace(cert, name)['steps'][i]

    for name, index in calls:
        call = traces[name]['steps'][index]
        sid = call['statement']
        related = [(i, s) for i, s in enumerate(traces[name]['steps'])
                   if s.get('once_call_step') == index]
        damaged('once callback forged as API callee',
                lambda p, c, n=name, i=index: step(c, n, i).update(expanded_callee='init'))
        damaged('wrong once origin',
                lambda p, c, n=name, i=index: step(c, n, i).update(once_call_step=i+1))
        damaged('unknown once role',
                lambda p, c, n=name, i=index: step(c, n, i).update(once_role='forged_owner'))
        for j, marker in related:
            if marker.get('synthetic_event') not in ('once_enter', 'once_return') and not marker.get('once_callback_return'):
                continue
            label = marker.get('synthetic_event', 'actual callback Return')
            damaged('omit '+label, lambda p, c, n=name, i=j: trace(c, n)['steps'].pop(i))
            damaged('repeat '+label, lambda p, c, n=name, i=j: trace(c, n)['steps'].insert(i, copy.deepcopy(step(c, n, i))))
            damaged('wrong origin '+label, lambda p, c, n=name, i=j: step(c, n, i).update(once_call_step=999999))
            if marker.get('once_callback_return'):
                damaged('unsealed actual callback Return',
                        lambda p, c, n=name, i=j: step(c, n, i).pop('once_callback_return'))
            if marker.get('synthetic_event') == 'once_return':
                damaged('nonzero successful once API result',
                        lambda p, c, n=name, i=j: step(c, n, i).update(encoded_result='1'))
        # Rehash all program changes: these must fail semantic checks, not hashing.
        damaged('rehash AR call convention',
                lambda p, c, s=sid: p['instructions'][s].update(calling_convention=8), True)
        damaged('rehash API return type',
                lambda p, c: p['functions']['pthread_once']['return_type'].update(bits=64), True)
        damaged('rehash API parameter count',
                lambda p, c: p['functions']['pthread_once']['parameter_types'].pop(), True)
        damaged('rehash API control pointer width',
                lambda p, c: p['functions']['pthread_once']['parameter_types'][0].update(bits=16), True)
        damaged('rehash control operand type',
                lambda p, c, s=sid: p['instructions'][s]['operands'][1]['type'].update(bits=16), True)
        damaged('rehash callback operand type',
                lambda p, c, s=sid: p['instructions'][s]['operands'][2]['type'].update(bits=16), True)
        damaged('rehash native callback identity',
                lambda p, c, s=sid: p['instructions'][s]['native_once'].update(callback='empty'), True)
        damaged('rehash native API convention',
                lambda p, c, s=sid: p['instructions'][s]['native_once'].update(call_convention=8), True)
        for field in ('size', 'extent'):
            damaged('rehash single once control operand '+field,
                    lambda p, c, s=sid, f=field: p['instructions'][s]['operands'][1]['type'].update({f: '1'}), True)
            damaged('rehash single once control pointee '+field,
                    lambda p, c, s=sid, f=field: p['instructions'][s]['operands'][1]['type']['pointee'].update({f: '1'}), True)
            damaged('rehash once API control parameter '+field,
                    lambda p, c, f=field: p['functions']['pthread_once']['parameter_types'][0].update({f: '1'}), True)
        native = instructions[sid]['native_once']
        finite = native.get('finite_controls')
        control = sorted(finite['objects'])[0] if finite else native['control']
        damaged('rehash native nonzero control',
                lambda p, c, g=control: p['globals'][g]['native_once_control'].update(zero_initializer=False), True)
        if finite:
            damaged('rehash incomplete finite control uses',
                    lambda p, c, s=sid: p['instructions'][s]['native_once']['finite_controls'].update(uses=[]), True)
            damaged('rehash omitted MAY control object',
                    lambda p, c, s=sid: p['instructions'][s]['native_once']['finite_controls']['objects'].pop(), True)
        else:
            damaged('rehash incomplete raw control uses',
                    lambda p, c, g=control: p['globals'][g]['native_once_control'].update(uses=[]), True)
        damaged('rehash native module alias',
                lambda p, c, g=control: p['globals'][g]['native_once_control'].update(module_aliases=True), True)
        if call['once_role'] == 'passive':
            damaged('duplicate active initializer',
                    lambda p, c, n=name, i=index: step(c, n, i).update(once_role='active'))
            ret = next((j for j, s in related if s.get('synthetic_event') == 'once_return'), None)
            if ret is not None:
                damaged('passive return before initialization',
                        lambda p, c, n=name, i=ret: step(c, n, i).update(clock='0'))
    if any(i.get('native_once', {}).get('finite_controls') for i in instructions.values()):
        def damage_control_layout(program, field, value):
            def visit(node):
                if isinstance(node, dict):
                    if (node.get('kind') == 'pointer' and
                            node.get('pointee', {}).get('kind') == 'integer' and
                            node['pointee'].get('bits') == 32):
                        target = node['pointee'] if field.startswith('pointee_') else node
                        target[field.removeprefix('pointee_')] = value
                    for child in node.values():
                        visit(child)
                elif isinstance(node, list):
                    for child in node:
                        visit(child)
            visit(program)
        for field, value in (('size', '1'), ('extent', '16'),
                             ('pointee_size', '1'), ('pointee_extent', '16')):
            damaged('rehash all connected once pointer '+field,
                    lambda p, c, f=field, v=value: damage_control_layout(p, f, v), True)
        reordered = copy.deepcopy(program)
        reordered['globals'] = dict(reversed(list(reordered['globals'].items())))
        cc = copy.deepcopy(certificate); cc['program_sha256'] = digest_json(reordered)
        assert check(reordered, cc)['status'] == 'ACCEPT', (case, 'global initializer key order')
        count += 1
        for sid, inst in instructions.items():
            ops = inst.get('operands', [])
            if (inst.get('kind') == 'unary' and inst.get('operator') == 14 and ops and
                    ops[0].get('kind') == 'function' and
                    ops[0].get('id') in ('__VERIFIER_nondet_int', '__VERIFIER_nondet_uint')):
                damaged('rehash old-style nondet callee as effectful API',
                        lambda p, c, s=sid: p['instructions'][s]['operands'][0].update(id='pthread_once'), True)
            if inst.get('kind') == 'allocate' and inst.get('allocated_type', {}).get('kind') == 'pointer':
                for field in ('size', 'extent'):
                    damaged('rehash local pointer slot '+field,
                            lambda p, c, s=sid, f=field: p['instructions'][s]['allocated_type'].update({f: '1'}), True)
                    damaged('rehash local slot pointee '+field,
                            lambda p, c, s=sid, f=field: p['instructions'][s]['allocated_type']['pointee'].update({f: '1'}), True)
            incoming = inst.get('native_pointer_phi')
            if incoming and any(x.get('value', {}).get('kind') == 'global' for x in incoming):
                original = inst['operands'][0]
                alternative = next((x['value'] for x in incoming if x['value'].get('id') != original.get('id')), None)
                if alternative:
                    damaged('rehash pointer PHI arm from wrong source edge',
                            lambda p, c, s=sid, v=alternative: p['instructions'][s].update(operands=[copy.deepcopy(v)]), True)
        for name, glob in program['globals'].items():
            native = glob.get('native_pointer_initializer')
            if native:
                for field in ('size', 'extent'):
                    damaged('rehash global pointer slot '+field,
                            lambda p, c, n=name, f=field: p['globals'][n]['type'].update({f: '1'}), True)
                    damaged('rehash global slot pointee '+field,
                            lambda p, c, n=name, f=field: p['globals'][n]['type']['pointee'].update({f: '1'}), True)
                    damaged('rehash global initializer pointer '+field,
                            lambda p, c, n=name, f=field: p['globals'][n]['initializers'][0]['type'].update({f: '1'}), True)
                damaged('rehash pointer initializer missing target',
                        lambda p, c, n=name: p['globals'][n]['native_pointer_initializer'].update(target='missing_object'), True)
    damaged('fake child creation occurrence',
            lambda p, c: trace(c, child['id']).update(creator_step=child['creator_step']+1))
    damaged('callback forged as pthread root',
            lambda p, c: trace(c, child['id']).update(entry='init'))
    damaged('wrong physical root identity',
            lambda p, c: trace(c, main['id']).update(entry=child['entry']))
    returns = [sid for block in program['functions']['main'].get('blocks', {}).values()
               for sid in block['statements'] if instructions[sid]['kind'] == 'return']
    if returns:
        damaged('insert main Return (structural rejection; not isolated process-exit check)',
                lambda p, c: trace(c, main['id'])['steps'].insert(0,
                    dict(statement=returns[0], clock='0')))
    return count, damage


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--ikos', type=Path)
    parser.add_argument('--evidence-dir', type=Path)
    parser.add_argument('--bits', choices=('32', '64', 'both'), default='both')
    parser.add_argument('--case', type=int, action='append')
    parser.add_argument('--with-ablation', action='store_true')
    parser.add_argument('--export-source-witness', action='store_true')
    parser.add_argument('--broken-premises', action='store_true')
    parser.add_argument('--finite-controls', action='store_true')
    parser.add_argument('--original-dir', type=Path,
                        help='optional exact pthread-once original fixture directory')
    args = parser.parse_args()
    repo = args.repo.resolve()
    if args.ikos is None:
        args.ikos = repo / 'install/bin/ikos'
    sys.path[:0] = [str(repo), str(repo / 'scripts')]
    from analyzer.python.ikos.race_result import validate_race_result
    from validate_bmc_execution import check, digest_json
    from ikos_runtime import runtime_command, runtime_identity
    from test_bmc_startup_provider import verify_source_binding
    import svcomp_witness
    # The imported module's ROOT must point to this same candidate checkout.
    assert Path(sys.modules['test_bmc_startup_provider'].ROOT).resolve() == repo
    regression = repo / 'analyzer/test/regression/bmc-values'
    fixture = (regression / 'once-physical.c').read_text()
    if args.evidence_dir is None:
        args.evidence_dir = Path(tempfile.mkdtemp(prefix='ikos-once-physical-'))
    else:
        args.evidence_dir.mkdir(parents=True, exist_ok=False)
    before = runtime_identity(args.ikos.resolve())
    identity_path = args.evidence_dir / 'runtime-before.json'
    identity_path.write_text(json.dumps(before, indent=2)+'\n')
    env = {k: v for k, v in os.environ.items()
           if not k.startswith('IKOS_') and k not in ('PYTHONPATH', 'PYTHONHOME')}
    env.update(IKOS_BMC_RECOVER='1', IKOS_BMC_DIAGNOSTICS='1')
    rows, count = [], 0
    model = {'32': 'ILP32', '64': 'LP64'}
    inputs = []
    for case in range(21):
        if args.case and case not in args.case:
            continue
        for argc in ((1, 2) if case in (18, 19) else (None,)):
            inputs.append((str(case), case, argc, f'#define CASE {case}\n'+fixture))
    if args.finite_controls:
        finite = (regression / 'once-finite-controls.c').read_text()
        for case in range(24):
            if args.case and 300 + case not in args.case:
                continue
            inputs.append((str(300 + case), 300 + case, None, f'#define CASE {case}\n'+finite))
        if not args.case or 300 in args.case:
            old_style = finite.replace('extern int __VERIFIER_nondet_int(void);',
                                       'extern int __VERIFIER_nondet_int();')
            assert old_style != finite
            inputs.append(('300-old-declaration', 300, None, '#define CASE 0\n'+old_style))
        if not args.case or 324 in args.case:
            inputs.append(('324', 324, None, (regression / 'once-finite-impostor.c').read_text()))
    if args.broken_premises:
        native = json.loads((regression / 'once-physical-broken.json').read_text())
        for i, row in enumerate(native['rows']):
            inputs.append((row['name'], 200+i, None, (repo / row['path']).read_text()))
    if args.original_dir:
        for number, name in ((1066, 'norace-race-child-once-vs-main-write.c'),
                             (1067, 'norace-race-distinct-once-objects.c'),
                             (1069, 'norace-race-once-init-vs-main-write.c'),
                             (1068, 'norace-race-distinct-once-state.c'),
                             (1070, 'norace-race-once-pointer-changes.c')):
            inputs.append((str(number), number, None, (args.original_dir / name).read_text()))
    for bits in (('32', '64') if args.bits == 'both' else (args.bits,)):
        for name, case, argc, text in inputs:
            for ablation in ((False, True) if args.with_ablation else (False,)):
                tag = f'case{name}-{bits}-argc{argc}-'+('off' if ablation else 'on')
                work = args.evidence_dir / tag
                work.mkdir()
                source, report = work / 'input.c', work / 'report.json'
                source.write_text(text)
                environment = dict(env)
                if ablation:
                    environment['IKOS_DISABLE_FINITE_ONCE_CONTROLS' if 300 <= case < 325 or case in (1068,1070)
                                else 'IKOS_BMC_DISABLE_ONCE_EXECUTION'] = '1'
                command = [*runtime_command(args.ikos.resolve()), '--analyses=race', '--concurrency=auto',
                           '--partitioning=guards', '--demote-race-to-unknown', '-m'+bits,
                           '--bmc-deadline-ms=20000', '--bmc-solver-timeout-ms=1000',
                           '--format=json', '--report-file='+str(report), '-o', str(work/'output.db')]
                if argc is not None:
                    command.append('--argc='+str(argc))
                command.append(str(source))
                process = subprocess.run(command, cwd=work, env=environment,
                                         capture_output=True, text=True, timeout=60)
                (work/'stdout.txt').write_text(process.stdout)
                (work/'stderr.txt').write_text(process.stderr)
                assert process.returncode == 0 and report.is_file(), (tag, process.returncode, process.stderr)
                data = json.loads(report.read_text())
                metadata = validate_race_result(data)
                infos = [row.get('info', {}) for row in data['reports']]
                confirmations = sum(info.get('bmc', {}).get('counts', {}).get('once_physical_confirmed', 0)
                                    for info in infos)
                executions = [info['counterexample'] for info in infos if 'counterexample' in info]
                positive = case in POSITIVE or case in (1066, 1067, 1068, 1069, 1070) or case - 300 in FINITE_POSITIVE or (case == 18 and argc == 1)
                infeasible = (case == 18 and argc == 2) or (case == 19 and argc == 1)
                no_new = 200 <= case < 300 or case == 10 or (case in (18, 19) and argc != 1) or ablation or case - 300 in FINITE_REFUSE or case == 324
                if no_new:
                    assert confirmations == 0, (tag, 'forbidden new once profile', confirmations)
                if positive and not ablation:
                    assert metadata['verdict'] == 'FALSE' and executions and confirmations > 0, (tag, metadata, confirmations)
                elif case in NEGATIVE or case - 300 in FINITE_NEGATIVE or infeasible:
                    assert metadata['verdict'] in ('SAFE', 'UNKNOWN') and not executions, (tag, metadata)
                    if case != 13:
                        assert metadata['analysis_complete'] and metadata['checks_complete'], (tag, metadata)
                # Defined same-name function and argc2 case19 may be proved by an
                # existing independent mechanism; any FALSE still needs AR ACCEPT.
                if metadata['verdict'] == 'FALSE':
                    assert executions, (tag, 'FALSE without selected execution')
                if case == 312 and not ablation:
                    ai_command = list(command)
                    ai_report = work / 'ai-only.json'
                    ai_command = [('--bmc-deadline-ms=0' if v == '--bmc-deadline-ms=20000' else
                                   '--report-file='+str(ai_report) if v == '--report-file='+str(report) else
                                   str(work/'ai-only.db') if v == str(work/'output.db') else v) for v in ai_command]
                    ai = subprocess.run(ai_command, cwd=work, env=environment, capture_output=True, text=True, timeout=60)
                    (work/'ai-only.stdout').write_text(ai.stdout); (work/'ai-only.stderr').write_text(ai.stderr)
                    assert ai.returncode == 0 and ai_report.is_file(), (tag, 'AI multiplicity control')
                    ai_data = json.loads(ai_report.read_text())
                    assert validate_race_result(ai_data)['verdict'] != 'SAFE', (tag, 'two controls collapsed into one instance')
                row_damage = []
                for execution in executions:
                    physical = any('once_role' in s for t in execution['traces'] for s in t['steps'])
                    if physical:
                        n, damage = verify(execution, case, source, check, digest_json, verify_source_binding)
                        count += n
                        row_damage.extend(damage)
                    else:
                        cert = copy.deepcopy(execution); program = cert.pop('program')
                        cert['program_sha256'] = digest_json(program)
                        assert check(program, cert)['status'] == 'ACCEPT', tag
                        count += 1
                exports = []
                if args.export_source_witness:
                    for i, info in enumerate(infos):
                        if 'counterexample' not in info:
                            continue
                        witness = svcomp_witness.build_witness(str(source), info, model[bits], str(args.ikos.resolve()))
                        path = work/f'source-witness-{i}.json'
                        path.write_text(json.dumps(witness, indent=2)+'\n')
                        segments = witness[0]['content']
                        registrations = [w['waypoint'] for seg in segments for w in seg['segment']
                                         if w['waypoint']['type'] == 'function_enter']
                        targets = [w['waypoint'] for seg in segments for w in seg['segment']
                                   if w['waypoint']['type'] == 'target']
                        if any('once_role' in s for t in info['counterexample']['traces'] for s in t['steps']):
                            assert len(registrations) == 1 and len(targets) == 2
                            assert {x['thread_id'] for x in targets} == {0, 1}
                        exports.append(dict(path=str(path), sha256=sha(path), official_validation='NOT_RUN'))
                rows.append(dict(case=case, bits=int(bits), argc=argc, ablation=ablation,
                                 verdict=metadata['verdict'], once_confirmed=confirmations,
                                 source=str(source), source_sha256=sha(source), report=str(report),
                                 report_sha256=sha(report), executions=len(executions),
                                 damage=row_damage, source_exports=exports))
                print(tag+': '+metadata['verdict'], flush=True)
    after = runtime_identity(args.ikos.resolve())
    assert before == after, 'runtime identity changed during evaluation'
    print('Evidence: '+str(args.evidence_dir), flush=True)
    (args.evidence_dir/'results.json').write_text(json.dumps(dict(
        status='EXECUTED_ONLY_IF_THIS_RUNNER_FINISHED', runtime_before=before, runtime_after=after,
        assertions=count, official_source_witness_validation='NOT_RUN', rows=rows), indent=2)+'\n')


if __name__ == '__main__':
    main()
