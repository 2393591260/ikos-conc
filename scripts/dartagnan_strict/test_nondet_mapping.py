#!/usr/bin/env python3
"""Check direct/bitcast nondet mapping against the local-nondet fixture."""
import argparse
import copy
import json
from calibrate import events
from map_ikos import translate
from pathlib import Path


def main(args):
    report = json.loads(args.report.read_text())
    replay = next(c['replay'] for g in report['reports']
                  for c in g.get('info', {}).get('bmc', {}).get('candidates', []) if 'replay' in c)
    h, es = events(args.events)
    cert, coverage = translate(replay, h, es)
    bindings = coverage['nondet_bindings']
    assert len(bindings) == 2
    assert len({b['reference_event'] for b in bindings}) == 2
    assert {b['trace'] for b in bindings} == {'a', 'b'}
    assert coverage['full_counterexample_validated'] is False
    binding = next(b for b in bindings if b['trace'] == 'a')
    trace = next(t for t in replay['traces'] if t['id'] == 'a')
    sid = trace['steps'][binding['step']]['statement']
    fn = binding['callee']
    for key, value in (('declaration', False), ('parameter_types', [{}])):
        r = copy.deepcopy(replay); r['program']['functions'][fn][key] = value
        assert not translate(r, h, es)[1]['nondet_bindings']
    r = copy.deepcopy(replay)
    r['program']['functions'][fn]['return_type']['bits'] = 64
    assert not translate(r, h, es)[1]['nondet_bindings']
    r = copy.deepcopy(replay)
    r['program']['functions'][fn]['return_type']['signed'] = False
    assert not translate(r, h, es)[1]['nondet_bindings']
    r = copy.deepcopy(replay)
    r['program']['instructions'][sid]['abstract_initializer'] = True
    assert not translate(r, h, es)[1]['nondet_bindings']
    r = copy.deepcopy(replay)
    r['program']['instructions'][sid]['operands'][0]['id'] = '__VERIFIER_nondet_int_fake'
    assert not translate(r, h, es)[1]['nondet_bindings']
    r = copy.deepcopy(replay)
    r['program']['instructions']['ambiguous-call'] = copy.deepcopy(r['program']['instructions'][sid])
    assert not translate(r, h, es)[1]['nondet_bindings']
    e = copy.deepcopy(es)
    next(e for e in e if e['id'] == binding['reference_event'])['nondet_bits'] = 64
    assert len(translate(replay, h, e)[1]['nondet_bindings']) == 1
    r = copy.deepcopy(replay)
    t = next(t for t in r['traces'] if t['id'] == 'a')
    t['steps'][binding['step']]['encoded_result'] = str(2**32 + 7)
    try:
        translate(r, h, es)
    except ValueError as exc:
        assert 'integer range' in str(exc)
    else:
        raise AssertionError('out-of-range result silently wrapped')
    r = copy.deepcopy(replay)
    # Preserve dynamic identity: a second call must not reuse the first reference result.
    t = next(t for t in r['traces'] if t['id'] == 'a')
    t['steps'].append(copy.deepcopy(t['steps'][binding['step']]))
    assert len(translate(r, h, es)[1]['nondet_bindings']) == 2
    r = copy.deepcopy(replay)
    ar = r['program']['instructions'][sid]
    ar['operands'][0] = dict(kind='variable', id='test-callee')
    cast = dict(kind='unary', operator=14, block=ar['block'],
                result=dict(kind='variable', id='test-callee'), operands=[dict(kind='function', id=fn)])
    r['program']['instructions']['test-cast'] = cast
    for t in r['traces']:
        if t['id'] in ('a', 'b'):
            t['steps'].insert(binding['step'], dict(statement='test-cast'))
    for target in r['targets']:
        if target['trace'] in ('a', 'b') and target['step'] >= binding['step']:
            target['step'] += 1
    assert len(translate(r, h, es)[1]['nondet_bindings']) == 2
    r['program']['instructions']['test-cast']['operator'] = 0
    assert not translate(r, h, es)[1]['nondet_bindings']
    print('13 nondet mapping checks passed')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--report', type=Path, required=True)
    p.add_argument('--events', type=Path, required=True)
    main(p.parse_args())
