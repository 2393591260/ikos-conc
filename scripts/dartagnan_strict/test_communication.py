#!/usr/bin/env python3
"""Check SC source identity when equal-valued writes cannot identify rf."""
import copy
from map_ikos import communication, memory_orderings


def access(statement, clock, kind='global', name='data', offset=0, width=4):
    return dict(statement=statement, clock=str(clock), encoded_address=dict(
        object=dict(kind=kind, id=name), offset=str(offset), width=width))


def main():
    replay = dict(program=dict(instructions={
        'write-first': dict(kind='store'), 'write-last': dict(kind='store'), 'read': dict(kind='load')}),
        traces=[dict(id='a', steps=[access('write-first', 10), access('read', 30)]),
                dict(id='b', steps=[access('write-last', 20)])])
    bindings = [dict(trace='a', step=0, reference_event=1),
                dict(trace='a', step=1, reference_event=3),
                dict(trace='b', step=0, reference_event=2)]
    co = dict(relation='co', **{'from': 1, 'to': 2})
    first = dict(relation='rf', **{'from': 1, 'to': 3})
    last = dict(relation='rf', **{'from': 2, 'to': 3})
    assert communication(replay, bindings) == ([co, last], [])
    assert memory_orderings(replay, bindings) == ([dict(before=1, after=2), dict(before=2, after=3)], [])
    r = copy.deepcopy(replay); r['traces'][0]['steps'][1]['clock'] = '15'
    assert communication(r, bindings) == ([co, first], [])
    r['traces'][0]['steps'][1]['clock'] = '5'
    edges, omitted = communication(r, bindings)
    assert edges == [co] and omitted[0]['reason'] == 'implicit initial write not mapped'
    edges, omitted = communication(replay, bindings[:-1])
    assert not edges and omitted[0]['reason'] == 'unmapped/unknown write source'
    r = copy.deepcopy(replay); del r['traces'][1]['steps'][0]['encoded_address']
    edges, omitted = communication(r, bindings)
    assert not edges and omitted[0]['reason'] == 'unmapped/unknown write source'
    r = copy.deepcopy(replay); del r['traces'][1]['steps'][0]['clock']
    assert not communication(r, bindings)[0]
    orders, omitted = memory_orderings(r, bindings)
    assert orders == [dict(before=1, after=3)] and omitted[0]['reason'] == 'missing memory clock'
    r = copy.deepcopy(replay); r['traces'][1]['steps'][0]['clock'] = '30'
    assert not communication(r, bindings)[0]
    assert memory_orderings(r, bindings) == ([dict(before=1, after=3), dict(before=1, after=2)], [])
    r = copy.deepcopy(replay); r['traces'][1]['steps'][0]['encoded_address']['width'] = 1
    assert not communication(r, bindings)[0]
    r = copy.deepcopy(replay); r['traces'][1]['steps'][0]['encoded_address']['offset'] = '1'
    assert not communication(r, bindings)[0]
    r['traces'][1]['steps'][0]['encoded_address']['offset'] = '4'
    assert communication(r, bindings) == ([first], [])
    r = copy.deepcopy(replay); r['traces'][1]['steps'][0]['encoded_address']['object']['id'] = 'other'
    assert communication(r, bindings) == ([first], [])
    r = copy.deepcopy(replay)
    read = r['traces'][0]['steps'].pop()
    r['traces'][0]['id'] = 'startup'; del r['traces'][0]['steps'][0]['clock']
    r['traces'].append(dict(id='a', steps=[read]))
    b = copy.deepcopy(bindings); b[0]['trace'] = 'startup'; b[1]['step'] = 0
    assert communication(r, b) == ([co, last], [])
    assert memory_orderings(r, b) == ([dict(before=1, after=2), dict(before=2, after=3)], [])
    r['traces'][1]['steps'][0]['encoded_address']['object']['kind'] = 'local'
    assert communication(r, b) == ([first], [])
    print('13 communication mapping checks passed')
    print('4 memory-order mapping checks passed')


if __name__ == '__main__':
    main()
