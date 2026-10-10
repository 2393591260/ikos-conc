#!/usr/bin/env python3
"""Check source-site mapping and SMT object binding for indirect memory events."""
import argparse
import copy
import json
from pathlib import Path
from calibrate import PIN, build, events, run
from map_ikos import translate


def main(args):
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    cp = build(args, out)
    report = json.loads(args.report.read_text())
    replay = next(c['replay'] for g in report['reports']
                  for c in g.get('info', {}).get('bmc', {}).get('candidates', []) if 'replay' in c)
    bits = replay['program']['pointer_bits']
    source = args.input.resolve()
    base = run(args, cp, source, bits, out/'source')
    assert base['status'] == 'SOURCE_RACE', base
    h, es = events(out/'source/events.tsv')
    cert, coverage = translate(replay, h, es)
    assert not coverage['full_counterexample_validated']
    indirect = []
    for binding in coverage['memory_bindings']:
        event = next(e for e in es if e['id'] == binding['reference_event'])
        slot = next(i for i in range(cert['count']) if cert[f'event.{i}.id'] == event['id'])
        if cert[f'event.{slot}.object'] not in event['code']:
            indirect.append((binding, slot))
    assert indirect, 'fixture must exercise an address without a printed object name'
    binding, slot = indirect[0]
    trace = next(t for t in replay['traces'] if t['id'] == binding['trace'])
    statement = trace['steps'][binding['step']]['statement']
    changed = copy.deepcopy(replay)
    changed['program']['instructions']['duplicate-source-site'] = copy.deepcopy(
        changed['program']['instructions'][statement])
    try:
        _, reduced = translate(changed, h, es)
    except ValueError:
        pass
    else:
        assert not any(b['trace'] == binding['trace'] and b['step'] == binding['step']
                       for b in reduced['memory_bindings'])
    changed = copy.deepcopy(es)
    event = next(e for e in changed if e['id'] == binding['reference_event'])
    duplicate = copy.deepcopy(event); duplicate['id'] = max(e['id'] for e in changed)+1
    duplicate['unrolling_id'] = -1; changed.append(duplicate)
    try:
        translate(replay, h, changed)
    except ValueError:
        pass
    else:
        raise AssertionError('unknown duplicate memory origin accepted')
    rows = []
    object_key = f'event.{slot}.object'
    alternatives = [n for n in replay['program']['globals'] if n != cert[object_key]]
    assert alternatives
    cases = [('valid-indirect-object', {}, 'SAT'),
             ('unknown-object', {object_key: 'missing_object'}, 'UNSUPPORTED'),
             ('wrong-offset', {f'event.{slot}.offset': int(cert[f'event.{slot}.offset'])+1}, 'UNSAT_AT_BOUND'),
             ('wrong-width', {f'event.{slot}.width': 1}, 'UNSUPPORTED')]
    # A real, different object cannot be substituted for one racing target.
    cases.append(('wrong-object', {object_key: alternatives[0]}, 'UNSAT_AT_BOUND'))
    for name, updates, expected in cases:
        changed = copy.deepcopy(cert); changed.update(updates)
        row = run(args, cp, source, bits, out/name, changed)
        row.update(case=name, expected=expected)
        rows.append(row); print(bits, name, row['status'], flush=True)
        assert row['status'] == expected, row
    (out/'summary.json').write_text(json.dumps(dict(reference_commit=PIN,
        mapping_checks=2, coverage=coverage, results=rows,
        full_counterexample_validated=False), indent=2)+'\n')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--report', type=Path, required=True)
    p.add_argument('--input', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--reference', type=Path, default=Path('/home/ruan/dartagnan-svcomp2026/dartagnan'))
    p.add_argument('--source', type=Path, default=Path('/tmp/ikos-dartagnan-source/Dat3M-'+PIN))
    p.add_argument('--javac', type=Path, default=Path('/tmp/ikos-reference-java/jdk/usr/lib/jvm/java-17-openjdk-amd64/bin/javac'))
    p.add_argument('--timeout', type=int, default=60)
    p.add_argument('--bound', type=int, default=2)
    main(p.parse_args())
