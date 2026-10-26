#!/usr/bin/env python3
"""Require actual repeated-site heap generations and independently replay them."""
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
        'analyzer/test/regression/bmc-values/heap-generations.c')
    fixture = fixture_path.read_text()
    positives = (0, 1, 4, 5, 12, 13)
    negatives = (2, 3, 6, 7, 8, 9, 10)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith('IKOS_') and k not in ('PYTHONPATH', 'PYTHONHOME')}
    env['IKOS_BMC_RECOVER'] = '1'
    os.environ.clear()
    os.environ.update(env)
    before = runtime_identity(ikos)
    counts = dict(controls=0, executions=0, mutations=0, cross_generation_rf=0,
                  free_mutations=0, alignment_mutations=0, positive_runs=0)
    rows = []
    with tempfile.TemporaryDirectory(prefix='ikos-heap-generations-') as temp:
        work = Path(temp)
        for bits in ([args.bits] if args.bits else (32, 64)):
            for wide in (False, True):
                for mode in positives + negatives:
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
                    if mode in negatives:
                        assert metadata['verdict'] in ('SAFE', 'UNKNOWN') and not certificates and not executions, (
                            bits, wide, mode, metadata, executions)
                        assert metadata['analysis_complete'] and metadata['checks_complete'], (
                            bits, wide, mode, 'incomplete analysis cannot pass a negative', metadata)
                    else:
                        assert metadata['verdict'] == 'FALSE' and metadata['coverage'] == 'Checked', (
                            bits, wide, mode, metadata)
                        assert executions and all(row['status'] == 'ACCEPT' and
                            row['strict_false_publishable'] for row in executions), executions
                        assert len(executions) == len(certificates), (executions, certificates)
                        counts['positive_runs'] += 1
                        for original in certificates:
                            certificate = copy.deepcopy(original)
                            program = certificate.pop('program')
                            certificate['program_sha256'] = digest_json(program)
                            assert program['pointer_bits'] == bits, program['pointer_bits']
                            replay = Replay(program, certificate)
                            checked = replay.run()
                            assert checked['status'] == 'ACCEPT', checked
                            traces = {trace['id']: trace for trace in certificate['traces']}
                            target = certificate['targets'][0]
                            step = traces[target['trace']]['steps'][target['step']]
                            obj = step['encoded_address']['object']
                            ordinal = 3 if mode == 4 else 2
                            assert obj['kind'] == 'heap' and obj['occurrence'] == ordinal, obj
                            allocation = obj['allocation']
                            heap = [value for value in replay.objects.values()
                                    if value.get('allocation') == allocation]
                            assert len(heap) == ordinal and sorted(value['occurrence']
                                for value in heap) == list(range(1, ordinal + 1)), heap
                            for value in heap:
                                assert value['allocation_event'] in [
                                    dict(trace=name, step=i)
                                    for name, trace in traces.items()
                                    for i, item in enumerate(trace['steps'])
                                    if item['statement'] == allocation], value
                            expected_live = ordinal - (mode == 1)
                            assert sum(value['live'] for value in heap) == expected_live, heap
                            assert obj['allocation_event'] == next(value['allocation_event']
                                for value in heap if value['occurrence'] == ordinal), obj
                            counts['executions'] += 1

                            def damage(mutator, label, expected_reason=None):
                                changed = copy.deepcopy(certificate)
                                mutator(changed)
                                verdict = check(program, changed)
                                assert verdict['status'] == 'REJECT', (label, verdict)
                                if expected_reason is not None:
                                    assert verdict['reason'] == expected_reason, (label, verdict)
                                counts['mutations'] += 1

                            def target_step(changed):
                                ts = {trace['id']: trace for trace in changed['traces']}
                                return ts[target['trace']]['steps'][target['step']]

                            fields = [
                                ('occurrence', 1),
                                ('extent', str(int(obj['extent']) + 1)),
                                ('allocation', 'wrong-allocation'),
                            ]
                            for field, value in fields:
                                damage(lambda c, k=field, v=value:
                                    target_step(c)['encoded_address']['object'].__setitem__(k, v), field)
                            first = next(value for value in heap if value['occurrence'] == 1)
                            damage(lambda c: target_step(c)['encoded_address']['object'].__setitem__(
                                'allocation_event', copy.deepcopy(first['allocation_event'])), 'wrong malloc step')
                            damage(lambda c: target_step(c)['encoded_address']['object']['allocation_event'].__setitem__(
                                'trace', 'wrong-trace'), 'wrong malloc trace')
                            damage(lambda c: target_step(c)['encoded_address']['object'].pop(
                                'allocation_event'), 'missing repeated malloc step')
                            damage(lambda c: c['targets'][0].__setitem__(
                                'step', len(traces[target['trace']]['steps'])), 'wrong target step')
                            damage(lambda c: target_step(c)['encoded_address'].__setitem__(
                                'offset', str(int(step['encoded_address']['offset']) + 1)), 'wrong byte offset')

                            if mode == 13:
                                outputs = [(name, i, item) for name, trace in traces.items()
                                    for i, item in enumerate(trace['steps'])
                                    if program['instructions'][item['statement']]['kind'] == 'call'
                                    and program['instructions'][item['statement']]['operands'][0].get('id')
                                        in ('pthread_create', 'ar.pthread.create')]
                                assert len(outputs) == 2
                                handle_width = bits // 8
                                assert {int(item['encoded_address']['offset'])
                                        for _, _, item in outputs} == {0, handle_width}
                                for name, index, item in outputs:
                                    address = item['encoded_address']
                                    assert address['object'] == obj
                                    assert address['alignment'] == handle_width
                                    assert address['width'] == handle_width

                                    def wrong_alignment(changed, owner=name, i=index):
                                        ts = {trace['id']: trace for trace in changed['traces']}
                                        ts[owner]['steps'][i]['encoded_address']['alignment'] = handle_width * 2
                                    damage(wrong_alignment, 'second-generation create output alignment',
                                           'SMT thread output alignment mismatch')
                                    counts['alignment_mutations'] += 1

                            # In MODE12 both generations initialize the same byte offset.
                            # Selecting the older object's write must still be rejected.
                            if mode == 12:
                                read_target = next(t for t in certificate['targets']
                                    if program['instructions'][
                                        traces[t['trace']]['steps'][t['step']]['statement']]['kind'] == 'load')
                                old_writes = [(name, i) for name, trace in traces.items()
                                    for i, item in enumerate(trace['steps'])
                                    if item.get('encoded_address', {}).get('object', {}).get('kind') == 'heap'
                                    and item['encoded_address']['object']['occurrence'] == 1
                                    and int(item['encoded_address']['offset']) == 0
                                    and program['instructions'][item['statement']]['kind'] == 'store']
                                assert old_writes, 'missing actual first-generation same-offset write'
                                name, index = old_writes[-1]

                                def wrong_rf(changed):
                                    ts = {trace['id']: trace for trace in changed['traces']}
                                    rs = ts[read_target['trace']]['steps'][read_target['step']]
                                    assert rs.get('read_from'), 'missing actual byte rf evidence'
                                    rs['read_from'] = [dict(trace=name, step=index)] * len(rs['read_from'])
                                damage(wrong_rf, 'same-offset different-generation rf')
                                counts['cross_generation_rf'] += 1
                            if mode == 1:
                                free_steps = [(name, i) for name, trace in traces.items()
                                    for i, item in enumerate(trace['steps'])
                                    if program['instructions'][item['statement']]['kind'] == 'call'
                                    and program['instructions'][item['statement']]['operands'][0].get('id')
                                        in ('free', 'ar.libc.free')]
                                assert free_steps, 'missing actual free'
                                name, index = free_steps[0]

                                def wrong_free(changed):
                                    ts = {trace['id']: trace for trace in changed['traces']}
                                    fs = ts[name]['steps'][index]
                                    fs['encoded_address']['object']['occurrence'] = ordinal
                                    fs['encoded_address']['object']['allocation_event'] = copy.deepcopy(
                                        obj['allocation_event'])
                                damage(wrong_free, 'free later generation instead of actual old generation')
                                counts['free_mutations'] += 1
                    counts['controls'] += 1
                    rows.append(dict(bits=bits, wide=wide, mode=mode,
                                     verdict=metadata['verdict'], executions=len(executions)))
                    print(f'heap-generations bits={bits}/wide={wide}/mode={mode}: '
                          f'{metadata["verdict"]} executions={len(executions)}', flush=True)
    assert counts['executions'] > 0 and counts['mutations'] > 0, counts
    assert counts['cross_generation_rf'] and counts['free_mutations'] and counts['alignment_mutations'], counts
    after = runtime_identity(ikos)
    assert before == after, 'selected runtime changed during generation controls'
    evidence = dict(counts=counts, rows=rows, runtime_before=before, runtime_after=after)
    if args.output:
        args.output.write_text(json.dumps(evidence, indent=2) + '\n')
    print(json.dumps(counts), flush=True)


if __name__ == '__main__':
    main()
