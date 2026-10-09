#!/usr/bin/env python3
"""Check that ambiguous/corrupt IKOS-to-reference mappings fail closed."""
import argparse
import copy
import json
from pathlib import Path
from calibrate import events
from map_ikos import translate


def main(args):
    report = json.loads(args.report.read_text())
    replay = next(c['replay'] for r in report['reports']
                  for c in r.get('info', {}).get('bmc', {}).get('candidates', []) if 'replay' in c)
    h, es = events(args.events)
    cert, coverage = translate(replay, h, es)
    assert coverage['full_counterexample_validated'] is False
    assert coverage['mapped_memory_events'] == 3
    assert cert['count'] == 3 + len(coverage['creation_bindings'])
    traces = {t['id']: t for t in replay['traces']}
    child_name = next(t['id'] for t in replay['traces'] if 'creator_trace' in t)

    def rejected(r, e):
        try:
            translate(r, h, e)
        except ValueError:
            return
        raise AssertionError('corrupt/ambiguous mapping accepted')

    r = copy.deepcopy(replay)
    next(t for t in r['traces'] if t['id'] == child_name)['creator_step'] = 0
    rejected(r, es)
    r = copy.deepcopy(replay); r['targets'][0]['step'] = 0
    rejected(r, es)
    r = copy.deepcopy(replay)
    target = r['targets'][0]
    stmt = next(t for t in r['traces'] if t['id'] == target['trace'])['steps'][target['step']]['statement']
    r['program']['instructions'][stmt]['line'] = 99999
    rejected(r, es)
    e = copy.deepcopy(es)
    original = next(x for x in e if x['id'] == cert['target_a'])
    duplicate = copy.deepcopy(original); duplicate['id'] = max(x['id'] for x in e)+1
    # A clone with matching provenance is supported; this extra event has none.
    duplicate['unrolling_id'] = -1
    e.append(duplicate)
    rejected(replay, e)
    r = copy.deepcopy(replay)
    t = next(t for t in r['traces'] if t['id'] == r['targets'][0]['trace'])
    t['steps'].append(copy.deepcopy(t['steps'][r['targets'][0]['step']]))
    rejected(r, es)
    r = copy.deepcopy(replay)
    r['traces'].append(copy.deepcopy(r['traces'][0]))
    rejected(r, es)
    r = copy.deepcopy(replay)
    next(t for t in r['traces'] if t['id'] == child_name)['creator_step'] = -1
    rejected(r, es)
    print('8 mapping assertions passed')

    if args.repeated_report is None:
        return
    if args.repeated_events is None:
        raise ValueError('--repeated-events is required with --repeated-report')
    report = json.loads(args.repeated_report.read_text())
    replay = next(c['replay'] for r in report['reports']
                  for c in r.get('info', {}).get('bmc', {}).get('candidates', []) if 'replay' in c)
    h, es = events(args.repeated_events)
    cert, coverage = translate(replay, h, es)
    assert coverage['threads'] == {'startup': 0, 'a': 1, 'b': 2}
    assert [b['occurrence'] for b in coverage['creation_bindings']] == [1, 2]
    assert [cert[f'event.{i}.creation_occurrence'] for i in range(cert['count'])
            if f'event.{i}.creation_occurrence' in cert] == [1, 2]
    e = copy.deepcopy(es)
    for event in e:
        event.pop('unrolling_id', None)
    rejected(replay, e)
    e = copy.deepcopy(es)
    creates = [x for x in e if x['code'].startswith('ThreadCreate(') and x['thread'] == 0]
    creates[1]['unrolling_id'] += 1
    rejected(replay, e)
    r = copy.deepcopy(replay)
    a, b = (next(t for t in r['traces'] if t['id'] == n) for n in ('a', 'b'))
    b['creator_step'] = a['creator_step']
    rejected(r, es)
    r = copy.deepcopy(replay)
    t = next(t for t in r['traces'] if t['id'] == 'startup')
    stmt = t['steps'][-1]['statement']
    r['program']['instructions']['duplicate-site'] = copy.deepcopy(r['program']['instructions'][stmt])
    rejected(r, es)
    e = copy.deepcopy(es)
    next(x for x in e if x['id'] == coverage['creation_bindings'][1]['reference_event'])['code'] = 'ThreadCreate(T2:wrong_entry, bv64(0))'
    rejected(replay, e)
    r = copy.deepcopy(replay)
    t = next(t for t in r['traces'] if t['id'] == 'startup')
    t['steps'].append(copy.deepcopy(t['steps'][-1]))
    next(t for t in r['traces'] if t['id'] == 'b')['creator_step'] = len(t['steps']) - 1
    rejected(r, es)
    print('7 repeated-creation mapping assertions passed')

    if args.memory_report is None:
        return
    if args.memory_events is None:
        raise ValueError('--memory-events is required with --memory-report')
    report = json.loads(args.memory_report.read_text())
    replay = next(c['replay'] for r in report['reports']
                  for c in r.get('info', {}).get('bmc', {}).get('candidates', []) if 'replay' in c)
    h, es = events(args.memory_events)
    cert, coverage = translate(replay, h, es)
    init = [m for m in coverage['memory_bindings'] if m['trace'] == 'startup']
    assert [m['occurrence'] for m in init] == list(range(1, 7))
    assert coverage['full_counterexample_validated'] is False
    e = copy.deepcopy(es)
    next(x for x in e if x['id'] == init[1]['reference_event'])['unrolling_id'] += 1
    _, reduced = translate(replay, h, e)
    assert not any(m['trace'] == 'startup' for m in reduced['memory_bindings'])
    assert any(s['reason'] == 'distinct/unknown static memory origins' for s in reduced['omitted_steps'])
    r = copy.deepcopy(replay)
    trace = next(t for t in r['traces'] if t['id'] == 'startup')
    stmt = trace['steps'][init[0]['step']]['statement']
    r['program']['instructions']['duplicate-memory-site'] = copy.deepcopy(r['program']['instructions'][stmt])
    _, reduced = translate(r, h, es)
    assert not any(m['trace'] == 'startup' for m in reduced['memory_bindings'])
    r = copy.deepcopy(replay)
    trace = next(t for t in r['traces'] if t['id'] == 'startup')
    # Removing the current invocation's result must not reuse the previous one.
    previous = next(s for s in reversed(trace['steps'][:init[1]['step']])
                    if 'encoded_result' in s and r['program']['instructions'][s['statement']]['kind'] == 'call')
    del previous['encoded_result']
    cert, _ = translate(r, h, es)
    slot = next(i for i in range(cert['count']) if cert[f'event.{i}.id'] == init[1]['reference_event'])
    assert f'event.{slot}.value' not in cert
    print('4 repeated-memory mapping assertions passed')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--report', type=Path, required=True)
    p.add_argument('--events', type=Path, required=True)
    p.add_argument('--repeated-report', type=Path)
    p.add_argument('--repeated-events', type=Path)
    p.add_argument('--memory-report', type=Path)
    p.add_argument('--memory-events', type=Path)
    main(p.parse_args())
