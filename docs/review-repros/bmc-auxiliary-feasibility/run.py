#!/usr/bin/env python3
"""Construct finite auxiliary schedules on genuine frontend AR snapshots.

This is an execution/replay feasibility experiment, not production SMT search.
The temporary Replay differs only in trace-name admission and a creation-origin
uniqueness check. Production analyzer/validator files are never edited.
"""
import argparse
import copy
import difflib
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def dump(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def temporary_replay(root, output):
    source = root / 'scripts/validate_bmc_execution.py'
    original = source.read_text()
    old = "        require(set(self.traces) <= {'startup', 'a', 'b'} and {'a', 'b'} <= set(self.traces), 'trace identities')"
    new = """        require({'a', 'b'} <= set(self.traces) and all(
            name in {'startup', 'a', 'b'} or (name.startswith('aux_') and name[4:].isdigit())
            for name in self.traces), 'trace identities')
        bindings = [(t.get('creator_trace'), t.get('creator_step'))
                    for t in self.traces.values() if t['entry'] != 'main']
        require(all(parent in self.traces and type(step) is int and step >= 0
                    for parent, step in bindings), 'creation occurrence identity')
        require(len(bindings) == len(set(bindings)), 'duplicate dynamic creation occurrence')"""
    assert original.count(old) == 1
    modified = original.replace(old, new)
    target = output / 'validate_auxiliary.py'
    target.write_text(modified)
    (output / 'validator.patch').write_text(''.join(difflib.unified_diff(
        original.splitlines(True), modified.splitlines(True),
        fromfile=str(source), tofile=str(target))))
    spec = importlib.util.spec_from_file_location('validate_auxiliary', target)
    module = importlib.util.module_from_spec(spec)
    import sys
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module, source, target


def production_replay(source):
    spec = importlib.util.spec_from_file_location('validate_production', source)
    module = importlib.util.module_from_spec(spec)
    import sys
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def export(root, output, bits, joins):
    folder = output / f'{bits}-join{joins}'
    folder.mkdir()
    report = folder / 'report.json'
    command = [str(root / 'install/bin/ikos'), '--analyses=race', '--concurrency=auto',
        '--demote-race-to-unknown', '-m', str(bits), '-D_GNU_SOURCE', f'-DJOINS={joins}',
        '--format=json', '--report-file=' + str(report), '-o', str(folder / 'output.db'),
        str(Path(__file__).with_name('seed.c').resolve())]
    env = dict(os.environ, IKOS_BMC_RECOVER='1')
    process = subprocess.run(command, cwd=folder, env=env,
        text=True, capture_output=True, timeout=30)
    (folder / 'run.log').write_text(process.stdout + process.stderr)
    dump(folder / 'command.json', dict(argv=command, env=dict(IKOS_BMC_RECOVER='1'),
                                      returncode=process.returncode))
    assert process.returncode == 0, process.stdout + process.stderr
    certificates = [row['info']['counterexample'] for row in json.loads(report.read_text())['reports']
                    if 'counterexample' in row.get('info', {})]
    assert certificates, process.stdout + process.stderr
    dump(folder / 'exported-certificate.json', certificates[0])
    return folder, certificates[0], command


def construct(module, program, joins):
    names = ['aux_0', 'aux_1', 'aux_2', 'b']
    traces = [dict(id='a', entry='main', steps=[])] + [
        dict(id=name, entry='worker', creator_trace='a', creator_step=i, steps=[])
        for i, name in enumerate(names)]
    certificate = dict(schema=1, program_sha256=module.digest_json(program),
                       traces=traces, targets=[])
    replay = module.Replay(program, certificate)
    replay.threads['a'] = module.Thread(0, [replay.frame('main', [])], {0: 0})
    ticks, creates = 0, 0

    def truth(inst, frame):
        a, b = [replay.value(v, frame) for v in inst['operands']]
        return dict(eq=a == b, ne=a != b, gt=a > b, ge=a >= b,
                    lt=a < b, le=a <= b)[module.CMP[inst['predicate']]]

    def next_statement(name):
        frame = replay.threads[name].frames[-1]
        blocks = program['functions'][frame.function]['blocks']
        block = blocks[frame.block]
        if frame.pc < len(block['statements']):
            return block['statements'][frame.pc]
        pending, visited, eligible = list(block['successors']), set(), []
        while pending:
            bid = pending.pop()
            assert bid not in visited, 'empty-block cycle'
            visited.add(bid)
            candidate = blocks[bid]
            if not candidate['statements']:
                pending.extend(candidate['successors'])
                continue
            guards = []
            for sid in candidate['statements']:
                inst = program['instructions'][sid]
                if inst['kind'] != 'compare':
                    break
                guards.append(inst)
            if all(truth(inst, frame) for inst in guards):
                eligible.append(candidate['statements'][0])
        assert len(eligible) == 1, eligible
        return eligible[0]

    def emit(name):
        nonlocal ticks, creates
        sid = next_statement(name)
        inst = program['instructions'][sid]
        frame = replay.threads[name].frames[-1]
        values = [replay.value(v, frame) for v in inst['operands']]
        step = dict(statement=sid, clock=str(ticks))
        ticks += 1
        index = len(replay.traces[name]['steps'])
        if inst['kind'] == 'call':
            callee = values[0][1]
            if callee == 'pthread_create':
                replay.traces[names[creates]]['creator_step'] = index
                creates += 1
                pointer = values[1]
                step['encoded_address'] = dict(object=dict(kind='local', id=replay.objects[pointer[0]]['variable']),
                    offset=str(pointer[1]), width=program['pointer_bits']//8, alignment=program['pointer_bits']//8)
            if not inst.get('abstract_initializer') and (program['functions'][callee].get('intrinsic_nondet') or callee == '__VERIFIER_nondet_int'):
                # A worker has two source nondet calls: data value, then probe guard.
                ordinal = sum(program['instructions'][s['statement']]['kind'] == 'call' and
                              program['instructions'][s['statement']]['operands'][0].get('id') == callee
                              for s in replay.traces[name]['steps'])
                step['encoded_result'] = str(10 + names.index(name) if name != 'a' and ordinal == 0 else 0)
        if inst['kind'] in ('load', 'store'):
            pointer = values[0]
            width = int((inst['result']['type'] if inst['kind'] == 'load' else inst['operands'][1]['type'])['size'])
            meta = replay.objects[pointer[0]]
            obj = dict(kind='global' if pointer[0].startswith('global:') else 'local', id=meta['variable'])
            step['encoded_address'] = dict(object=obj, offset=str(pointer[1]), width=width)
            if inst['kind'] == 'load':
                step['read_from'] = [replay.writers.get((pointer[0], pointer[1]+j), 'initial') for j in range(width)]
        replay.execute(name, index, step)
        if 'result' in inst and inst['result']['type']['kind'] == 'integer':
            result = frame.env.get(inst['result']['id'])
            if isinstance(result, int):
                step['encoded_result'] = str(result)
        replay.traces[name]['steps'].append(step)
        return sid, inst

    # A single main frame creates all four instances. Each actual join executes
    # only after the matching child has really returned.
    while True:
        sid = next_statement('a')
        inst = program['instructions'][sid]
        if inst['kind'] == 'call' and inst['operands'][0].get('id') in ('pthread_join', 'ar.pthread.join'):
            frame = replay.threads['a'].frames[-1]
            instance = replay.value(inst['operands'][1], frame)
            child = next(name for name in names if name in replay.threads and replay.threads[name].instance == instance)
            while not replay.threads[child].finished:
                child_sid, child_inst = emit(child)
                if child == 'b' and child_inst['kind'] == 'store' and child_inst['operands'][0].get('id') == 'data':
                    certificate['targets'].append(dict(trace='b', step=len(replay.traces['b']['steps'])-1))
        emitted_sid, emitted = emit('a')
        if emitted['kind'] == 'load' and emitted['operands'][0].get('id') == 'data':
            certificate['targets'].insert(0, dict(trace='a', step=len(replay.traces['a']['steps'])-1))
            break
    if joins == 3:
        while True:
            sid, inst = emit('b')
            if inst['kind'] == 'store' and inst['operands'][0].get('id') == 'data':
                certificate['targets'].append(dict(trace='b', step=len(replay.traces['b']['steps'])-1))
                break
    assert creates == 4 and len(certificate['targets']) == 2
    return certificate


def controls(module, program, certificate, joins):
    cases = [('joined_effectful_prefix' if joins == 3 else 'all_join_hb', certificate,
              'ACCEPT' if joins == 3 else 'REJECT')]
    if joins == 3:
        def variant(name, mutate, expected='REJECT'):
            changed = copy.deepcopy(certificate)
            mutate(changed)
            cases.append((name, changed, expected))
        def trace(c, name='aux_0'):
            return next(t for t in c['traces'] if t['id'] == name)
        variant('missing_return', lambda c: trace(c)['steps'].pop(), 'UNSUPPORTED')
        variant('missing_store', lambda c: trace(c)['steps'].__delitem__(next(i for i,s in enumerate(trace(c)['steps'])
            if program['instructions'][s['statement']]['kind'] == 'store')))
        variant('fake_return', lambda c: trace(c)['steps'].__setitem__(0, dict(
            statement=trace(c)['steps'][-1]['statement'], clock=trace(c)['steps'][0]['clock'])))
        variant('wrong_creator', lambda c: trace(c).__setitem__('creator_step', 0))
        variant('duplicate_creator', lambda c: trace(c).__setitem__('creator_step', trace(c,'aux_1')['creator_step']))
        def false_guard(c):
            calls = [s for s in trace(c)['steps'] if program['instructions'][s['statement']]['kind'] == 'call' and
                     program['instructions'][s['statement']]['operands'][0].get('id') == '__VERIFIER_nondet_int']
            calls[1]['encoded_result'] = '1'
        variant('false_guard', false_guard)
        def wrong_read_from(c):
            target = c['targets'][0]
            step = trace(c,'a')['steps'][target['step']]
            step['read_from'][0] = 'initial'
        variant('wrong_read_from', wrong_read_from)
        def wrong_value(c):
            target = c['targets'][0]
            trace(c,'a')['steps'][target['step']]['encoded_result'] = '999'
        variant('wrong_value', wrong_value)
    rows = []
    for name, changed, expected in cases:
        result = module.check(program, changed)
        assert result['status'] == expected, (name, expected, result)
        rows.append(dict(case=name, expected=expected, result=result,
                         certificate_sha256=module.digest_json(changed)))
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    root = args.root.resolve()
    module, original, patched = temporary_replay(root, args.output)
    production = production_replay(original)
    rows = []
    for bits in (32, 64):
        for joins in (3, 4):
            folder, exported, command = export(root, args.output, bits, joins)
            exported = copy.deepcopy(exported)
            program = exported.pop('program')
            exported['program_sha256'] = module.digest_json(program)
            bootstrap = production.check(program, exported)
            assert bootstrap['status'] == 'ACCEPT', bootstrap
            certificate = construct(module, program, joins)
            production_aux = production.check(program, certificate)
            assert production_aux == dict(status='REJECT',reason='trace identities'), production_aux
            dump(folder / 'program.json', program)
            dump(folder / 'constructed-certificate.json', certificate)
            results = controls(module, program, certificate, joins)
            dump(folder / 'results.json', results)
            rows.append(dict(bits=bits, joins=joins, command=command,
                             program_sha256=module.digest_json(program),
                             production_bootstrap=bootstrap, production_auxiliary=production_aux,
                             trace_steps={t['id']:len(t['steps']) for t in certificate['traces']},
                             results=results))
            print(bits, joins, [(r['case'],r['result']['status']) for r in results], flush=True)
    summary = dict(scope='constructed finite AR schedules; not production SMT auxiliary support',
        checks=dict(target_and_corruption_replays=sum(len(r['results']) for r in rows),
                    production_bootstrap_accepts=len(rows), production_auxiliary_rejects=len(rows)),
        root_revision=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),
        analyzer_sha256=sha(root/'install/bin/ikos-analyzer'),
        fixture_sha256=sha(Path(__file__).with_name('seed.c')),
        script_sha256=sha(Path(__file__)), original_validator_sha256=sha(original),
        temporary_validator_sha256=sha(patched), rows=rows,
        limitations=['Successful opaque pthread API and frontend snapshot contracts remain trusted.',
            'Snapshot was exported using an additional worker/worker probe race; constructed child probe guards are false.',
            'The all-join source still permits an export_probe race; its negative control concerns only forced data store/load HB.',
            'Schedules and concrete nondet values are constructed using Replay, not found by an extended SMT encoder.',
            'Missing Return remains UNSUPPORTED under the unchanged existing join-completion gate.',
            'No production search, budget, evidence exporter, or source witness support is established.'])
    dump(args.output/'summary.json', summary)
    compact = {k:v for k,v in summary.items() if k!='rows'}
    compact['rows'] = [dict(bits=r['bits'],joins=r['joins'],program_sha256=r['program_sha256'],
        trace_steps=r['trace_steps'], production_bootstrap=r['production_bootstrap']['status'],
        production_auxiliary=r['production_auxiliary'], results=[dict(case=q['case'],
        status=q['result']['status'],reason=q['result'].get('reason'),
        certificate_sha256=q['certificate_sha256']) for q in r['results']]) for r in rows]
    dump(args.output/'compact.json', compact)


if __name__ == '__main__':
    main()
