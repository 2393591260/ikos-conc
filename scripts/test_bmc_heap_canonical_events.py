#!/usr/bin/env python3
"""Require canonical heap indices after real condition markers with main as b."""
import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project-root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--ikos', type=Path)
    parser.add_argument('--fixture', type=Path)
    parser.add_argument('--bits', type=int, choices=(32, 64))
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    root = args.project_root.resolve()
    sys.path.insert(0, str(root / 'scripts'))
    sys.path.insert(0, str(root))
    from ikos_runtime import runtime_command, runtime_identity
    from analyzer.python.ikos.race_result import validate_race_result
    from sweep_bmc_execution import validate_report
    from validate_bmc_execution import Replay, check, digest_json
    ikos = args.ikos or root / 'install/bin/ikos'
    fixture_path = args.fixture or (Path(__file__).resolve().parents[1] /
        'analyzer/test/regression/bmc-values/heap-condition-canonical.c')
    fixture = fixture_path.read_text()
    env = {k: v for k, v in os.environ.items()
           if not k.startswith('IKOS_') and k not in ('PYTHONPATH', 'PYTHONHOME')}
    env['IKOS_BMC_RECOVER'] = '1'
    os.environ.clear()
    os.environ.update(env)
    before = runtime_identity(ikos)
    counts = dict(controls=0, executions=0, mutations=0, main_as_b=0,
                  heap_targets=0, global_targets_with_heap_prefix=0,
                  heap_read_from=0, mutex_attributes=0, closure_refusals=0)
    rows = []
    with tempfile.TemporaryDirectory(prefix='ikos-heap-canonical-') as temp:
        work = Path(temp)
        for bits in ([args.bits] if args.bits else (32, 64)):
            for wide in (False, True):
                for mode in (0, 1, 2):
                    source, report = work / 'renamed.c', work / 'report.json'
                    source.write_text(f'#define MODE {mode}\n' +
                                      ('#define WIDE\n' if wide else '') + fixture)
                    report.unlink(missing_ok=True)
                    result = subprocess.run(runtime_command(ikos) + [
                        '--analyses=race', '--concurrency=auto', '--partitioning=guards',
                        '--demote-race-to-unknown', f'-m{bits}',
                        '--bmc-deadline-ms=20000', '--bmc-solver-timeout-ms=1000',
                        '--format=json', '--report-file=' + str(report),
                        '-o', str(work / 'output.db'), str(source)],
                        env=env, capture_output=True, text=True, timeout=45)
                    assert result.returncode == 0, (bits, wide, mode, result.stdout, result.stderr)
                    data = json.loads(report.read_text())
                    metadata = validate_race_result(data)
                    executions = validate_report(data)
                    certificates = [row['info']['counterexample'] for row in data['reports']
                                    if row.get('info', {}).get('counterexample')]
                    assert metadata['verdict'] == 'FALSE' and metadata['coverage'] == 'Checked', (
                        bits, wide, mode, metadata)
                    assert executions and len(executions) == len(certificates), executions
                    assert all(row['status'] == 'ACCEPT' and row['strict_false_publishable']
                               for row in executions), executions
                    for original in certificates:
                        certificate = copy.deepcopy(original)
                        program = certificate.pop('program')
                        certificate['program_sha256'] = digest_json(program)
                        assert program['pointer_bits'] == bits, program['pointer_bits']
                        replay = Replay(program, certificate)
                        checked = replay.run()
                        assert checked['status'] == 'ACCEPT', checked
                        traces = {trace['id']: trace for trace in certificate['traces']}
                        assert traces['a']['entry'] == 'aaa_worker' and traces['b']['entry'] == 'main', traces
                        assert not traces.get('startup', {}).get('steps'), traces.get('startup')
                        counts['main_as_b'] += 1
                        instructions = program['instructions']

                        def callee(step):
                            inst = instructions[step['statement']]
                            return inst['operands'][0].get('id') if inst['kind'] == 'call' else None

                        main_steps = traces['b']['steps']
                        waits = [i for i, step in enumerate(main_steps)
                                 if not step.get('synthetic_event') and callee(step) == 'pthread_cond_wait']
                        allocations = [i for i, step in enumerate(main_steps)
                                       if callee(step) in ('malloc', 'ar.libc.malloc')]
                        assert len(waits) == len(allocations) == 1, (waits, allocations)
                        wait_index, allocation_index = waits[0], allocations[0]
                        assert wait_index + 2 < allocation_index, (wait_index, allocation_index)
                        for delta, kind in ((1, 'condition_release'), (2, 'condition_reacquire')):
                            marker = main_steps[wait_index + delta]
                            assert marker.get('synthetic_event') == kind and marker['statement'] == (
                                main_steps[wait_index]['statement']), marker
                        binding = dict(trace='b', step=allocation_index)
                        heap_objects = [value for value in replay.objects.values()
                                        if value.get('allocation') == main_steps[allocation_index]['statement']]
                        assert len(heap_objects) == 1 and heap_objects[0]['live'], heap_objects
                        assert heap_objects[0]['occurrence'] == 1 and heap_objects[0]['allocation_event'] == binding
                        heap_steps = []
                        for name, trace in traces.items():
                            for i, step in enumerate(trace['steps']):
                                for key in ('encoded_address', 'encoded_object_pointer'):
                                    obj = step.get(key, {}).get('object', {})
                                    if obj.get('kind') != 'heap':
                                        continue
                                    assert obj['occurrence'] == 1 and obj['allocation_event'] == binding, obj
                                    assert obj['allocation'] == main_steps[allocation_index]['statement'], obj
                                    if key == 'encoded_address':
                                        heap_steps.append((name, i))
                        worker_heap = [(name, i) for name, i in heap_steps if name == 'a']
                        assert worker_heap, 'worker must consume the actual main allocation'
                        selected = [(t['trace'], t['step']) for t in certificate['targets']]
                        if mode == 0:
                            assert len(selected) == 2 and all(t in heap_steps for t in selected), selected
                            counts['heap_targets'] += 1
                        else:
                            assert len(selected) == 2 and all(
                                traces[name]['steps'][i]['encoded_address']['object']['kind'] == 'global'
                                for name, i in selected), selected
                            worker_target = next(i for name, i in selected if name == 'a')
                            assert any(i < worker_target for _, i in worker_heap), worker_heap
                            counts['global_targets_with_heap_prefix'] += 1

                        def damage(mutator, label):
                            changed = copy.deepcopy(certificate)
                            mutator(changed)
                            verdict = check(program, changed)
                            assert verdict['status'] == 'REJECT', (label, verdict)
                            counts['mutations'] += 1

                        name, index = worker_heap[0]

                        def heap_step(changed):
                            ts = {trace['id']: trace for trace in changed['traces']}
                            return ts[name]['steps'][index]

                        damage(lambda c: heap_step(c)['encoded_address']['object']['allocation_event'].__setitem__(
                            'step', allocation_index - 2), 'pre-expansion malloc step')
                        damage(lambda c: heap_step(c)['encoded_address']['object']['allocation_event'].__setitem__(
                            'trace', 'a'), 'wrong allocator trace')
                        damage(lambda c: heap_step(c)['encoded_address']['object'].__setitem__(
                            'occurrence', 2), 'nonexistent second generation')
                        damage(lambda c: heap_step(c)['encoded_address']['object'].__setitem__(
                            'allocation', 'wrong-allocation'), 'wrong actual malloc instruction')

                        def wrong_marker(changed):
                            ts = {trace['id']: trace for trace in changed['traces']}
                            ts['b']['steps'][wait_index + 1]['synthetic_event'] = 'condition_reacquire'
                        damage(wrong_marker, 'condition reacquire without release')
                        if mode == 2:
                            assert any(callee(s) == 'pthread_mutexattr_settype' for s in main_steps), main_steps
                            counts['mutex_attributes'] += 1
                            assert instructions[traces[name]['steps'][index]['statement']]['kind'] == 'load'
                            read_from = traces[name]['steps'][index].get('read_from')
                            assert read_from and all(isinstance(r, dict) and r['trace'] == 'b' and
                                instructions[main_steps[r['step']]['statement']]['kind'] == 'store'
                                for r in read_from), read_from

                            def wrong_rf(changed):
                                heap_step(changed)['read_from'] = [dict(trace='b', step=wait_index + 1)] * len(read_from)
                            damage(wrong_rf, 'heap bytes from condition-release event')
                            counts['heap_read_from'] += 1
                        counts['executions'] += 1
                    counts['controls'] += 1
                    rows.append(dict(bits=bits, wide=wide, mode=mode,
                                     verdict=metadata['verdict'], executions=len(executions)))
                    print(f'heap-canonical bits={bits}/wide={wide}/mode={mode}: '
                          f'{metadata["verdict"]} executions={len(executions)}', flush=True)
        # These are producer-closure refusals, separate from the three mandatory
        # positive modes above. A selected good spawn may not hide an unsafe
        # alternate callback source or opaque-storage use in another CFG arm.
        for bits in ([args.bits] if args.bits else (32, 64)):
            for wide in (False, True):
                prefix = '#define MODE 2\n' + ('#define WIDE\n' if wide else '')
                def variant(old, new, preamble=''):
                    assert fixture.count(old) == 1, old
                    return prefix + preamble + fixture.replace(old, new)

                create = 'pthread_create(&child, 0, aaa_worker, p);'
                nondet = 'extern int __VERIFIER_nondet_int(void);\n'
                controls = {
                    'incoming_attribute_pointer': variant(create,
                        'pthread_create(&child, 0, aaa_worker, &attributes);'),
                    'mixed_heap_attribute_pointer': variant(create,
                        'pthread_create(&child, 0, aaa_worker, '
                        '__VERIFIER_nondet_int() ? (void *)p : (void *)&attributes);', nondet),
                    'second_bad_callback_activation': variant(create,
                        'if (__VERIFIER_nondet_int()) { pthread_t other; '
                        'pthread_create(&other, 0, aaa_worker, &attributes); }\n' + create, nondet),
                    'direct_callback_activation': variant(create,
                        'aaa_worker(&attributes);\n' + create),
                    'unknown_pointer_source': variant('element *p = malloc(sizeof(element));',
                        'element *p = unknown_pointer();',
                        'extern void *unknown_pointer(void);\n'),
                    'out_of_extent': variant('malloc(sizeof(element))',
                        'malloc(sizeof(element) - 1)'),
                    'nonzero_heap_offset': variant(create,
                        'pthread_create(&child, 0, aaa_worker, p + 1);'),
                    'opaque_attribute_scalar_read': variant('element value = *p;',
                        'element value = *p + *(int *)&attributes;'),
                    'freed_allocation': variant(create, 'free(p);\n' + create),
                    'external_opaque_storage': variant('static pthread_mutexattr_t attributes;',
                        'pthread_mutexattr_t attributes;'),
                    'underaligned_condition_storage': variant(
                        'static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;',
                        'typedef pthread_cond_t underaligned_condition '
                        '__attribute__((aligned(1)));\n'
                        'static underaligned_condition condition = PTHREAD_COND_INITIALIZER;'),
                    'nonzero_condition_initializer': variant(
                        'static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;',
                        'static pthread_cond_t condition = { .__size = { 1 } };'),
                    'condition_destroyed': variant(
                        'pthread_cond_wait(&condition, &mutex);',
                        'pthread_cond_destroy(&condition);\n'
                        'pthread_cond_wait(&condition, &mutex);'),
                    'mismatched_condition_object': variant(
                        'pthread_cond_wait(&condition, &mutex);',
                        'pthread_cond_wait((pthread_cond_t *)&mutex, &mutex);'),
                }
                for label, text in controls.items():
                    source, report = work / 'refused.c', work / 'refused.json'
                    source.write_text(text)
                    report.unlink(missing_ok=True)
                    result = subprocess.run(runtime_command(ikos) + [
                        '--analyses=race', '--concurrency=auto', '--partitioning=guards',
                        '--demote-race-to-unknown', f'-m{bits}',
                        '--bmc-deadline-ms=20000', '--bmc-solver-timeout-ms=1000',
                        '--format=json', '--report-file=' + str(report),
                        '-o', str(work / 'refused.db'), str(source)],
                        env=env, capture_output=True, text=True, timeout=45)
                    assert result.returncode == 0, (bits, wide, label, result.stdout, result.stderr)
                    data = json.loads(report.read_text())
                    metadata = validate_race_result(data)
                    assert metadata['verdict'] == 'UNKNOWN' and metadata['coverage'] == 'Blocked', (
                        bits, wide, label, metadata)
                    assert not validate_report(data), (bits, wide, label, data)
                    assert not any(row.get('info', {}).get('counterexample') for row in data['reports']), (
                        bits, wide, label, data)
                    counts['closure_refusals'] += 1
                    rows.append(dict(bits=bits, wide=wide, closure_control=label,
                                     verdict=metadata['verdict'], executions=0))
                    print(f'heap-canonical closure bits={bits}/wide={wide}/{label}: '
                          f'{metadata["verdict"]}', flush=True)

    assert counts['main_as_b'] == counts['executions'] > 0, counts
    assert all(counts[key] for key in ('heap_targets', 'global_targets_with_heap_prefix',
                                     'heap_read_from', 'mutex_attributes', 'mutations')), counts
    assert counts['closure_refusals'] == 28 * (1 if args.bits else 2), counts
    after = runtime_identity(ikos)
    assert before == after, 'selected runtime changed during canonical-index controls'
    evidence = dict(counts=counts, rows=rows, runtime_before=before, runtime_after=after)
    if args.output:
        args.output.write_text(json.dumps(evidence, indent=2) + '\n')
    print(json.dumps(counts), flush=True)


if __name__ == '__main__':
    main()
