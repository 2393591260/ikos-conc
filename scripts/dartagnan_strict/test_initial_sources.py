#!/usr/bin/env python3
"""Calibrate identical-valued initial writes as distinct SC read sources."""
import argparse
import copy
import json
from pathlib import Path
from calibrate import ROOT, PIN, build, certificate, events, run
from map_ikos import communication, initial_sources


def mapping_checks():
    replay = dict(program=dict(instructions={'read': dict(kind='load', result=dict(
        type=dict(kind='integer', bits=32)))}), traces=[dict(id='a', steps=[dict(
        statement='read', clock='1', encoded_address=dict(object=dict(kind='global', id='data'),
                                                        offset='0', width=4))])])
    bindings = [dict(trace='a', step=0, reference_event=10)]
    init = dict(id=5, initial=dict(object='data', offset=0, bits=32))
    sources = initial_sources(replay, bindings, [init])
    assert sources == {('data', 0, 4): 5}
    assert communication(replay, bindings, sources) == ([dict(relation='rf', **{'from': 5, 'to': 10})], [])
    for updates in (dict(object='other'), dict(offset=1), dict(bits=8)):
        changed = copy.deepcopy(init); changed['initial'].update(updates)
        assert initial_sources(replay, bindings, [changed]) == {}
    duplicate = copy.deepcopy(init); duplicate['id'] = 6
    assert initial_sources(replay, bindings, [init, duplicate]) == {}
    assert initial_sources(replay, bindings, [dict(id=5)]) == {}
    changed = copy.deepcopy(replay)
    changed['program']['instructions']['read']['result']['type']['kind'] = 'pointer'
    assert initial_sources(changed, bindings, [init]) == {}
    print('8 initial-source mapping checks passed')


def main(args):
    mapping_checks()
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    cp = build(args, out)
    source = ROOT / 'docs/review-repros/dartagnan-strict/initial-read.c'
    rows = []
    for bits in (32, 64):
        base = run(args, cp, source, bits, out/f'{bits}-source')
        assert base['status'] == 'SOURCE_RACE', base
        h, es = events(out/f'{bits}-source/events.tsv')
        write = next(e for e in es if 'store(&data,' in e['code'])
        read = next(e for e in es if 'load(&data)' in e['code'])
        init = next(e for e in es if e.get('initial') == dict(object='data', offset=0, bits=32))
        other = next(e for e in es if e.get('initial') == dict(object='other', offset=0, bits=32))
        cert = certificate(h, [write, read, init, other])
        cert.update({'event.0.value': 9, 'event.1.value': 7, 'edge_count': 1,
                     'edge.0.relation': 'rf', 'edge.0.from': init['id'], 'edge.0.to': read['id']})
        for name, updates, expected in (
                ('valid-initial-rf', {}, 'SAT'),
                ('equal-valued-wrong-object', {'edge.0.from': other['id']}, 'UNSAT_AT_BOUND'),
                ('wrong-initial-value', {'event.1.value': 8}, 'UNSAT_AT_BOUND'),
                ('overwritten-initial', {'order_count': 1, 'order.0.before': write['id'],
                                         'order.0.after': read['id']}, 'UNSAT_AT_BOUND'),
                ('initial-is-not-racing-access', {'target_a': init['id']}, 'UNSAT_AT_BOUND')):
            changed = copy.deepcopy(cert); changed.update(updates)
            row = run(args, cp, source, bits, out/f'{bits}-{name}', changed)
            row.update(case=name, expected=expected, matched=row['status'] == expected)
            rows.append(row); print(bits, name, row['status'], flush=True)
            assert row['matched'], row
    (out/'summary.json').write_text(json.dumps(dict(reference_commit=PIN, results=rows,
        passed=True, full_counterexample_validated=False), indent=2)+'\n')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--reference', type=Path, default=Path('/home/ruan/dartagnan-svcomp2026/dartagnan'))
    p.add_argument('--source', type=Path, default=Path('/tmp/ikos-dartagnan-source/Dat3M-'+PIN))
    p.add_argument('--javac', type=Path, default=Path('/tmp/ikos-reference-java/jdk/usr/lib/jvm/java-17-openjdk-amd64/bin/javac'))
    p.add_argument('--timeout', type=int, default=60)
    p.add_argument('--bound', type=int, default=2)
    main(p.parse_args())
