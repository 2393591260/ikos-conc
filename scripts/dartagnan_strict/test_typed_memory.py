#!/usr/bin/env python3
"""Check that width separates co-located loads without accepting ambiguous origins."""
import argparse
import copy
import json
from pathlib import Path

from calibrate import events
from map_ikos import memory_type, same_source, translate


def main(args):
    report = json.loads(args.report.read_text())
    replay = next(c['replay'] for group in report['reports']
                  for c in group.get('info', {}).get('bmc', {}).get('candidates', []) if 'replay' in c)
    h, es = events(args.events)
    cert, coverage = translate(replay, h, es)
    assert not coverage['full_counterexample_validated']
    instructions = replay['program']['instructions']
    traces = {t['id']: t for t in replay['traces']}
    targets = {(t['trace'], t['step']) for t in replay['targets']}
    binding = next(b for b in coverage['memory_bindings'] if (b['trace'], b['step']) in targets)
    sid = traces[binding['trace']]['steps'][binding['step']]['statement']
    ar = instructions[sid]
    event = next(e for e in es if e['id'] == binding['reference_event'])
    assert event['memory_bits'] == memory_type(ar)['bits']
    loads = [instructions[t['steps'][b['step']]['statement']] for b in coverage['memory_bindings']
             for t in (traces[b['trace']],) if instructions[t['steps'][b['step']]['statement']]['kind'] == 'load']
    assert any(e.get('memory_bits') == 64 and any(same_source(a, e) for a in loads)
               for e in es), 'fixture needs a co-located pointer load'

    def absent_or_rejected(r, ir):
        try:
            _, reduced = translate(r, h, ir)
        except ValueError:
            return
        assert not any(b['trace'] == binding['trace'] and b['step'] == binding['step']
                       for b in reduced['memory_bindings']), 'ambiguous target accepted'

    changed = copy.deepcopy(replay)
    changed['program']['instructions']['duplicate-same-type-site'] = copy.deepcopy(ar)
    absent_or_rejected(changed, es)
    duplicate = copy.deepcopy(event)
    duplicate.update(id=max(e['id'] for e in es)+1, unrolling_id=-1)
    absent_or_rejected(replay, es + [duplicate])
    changed = copy.deepcopy(es)
    next(e for e in changed if e['id'] == event['id'])['memory_bits'] = 64
    absent_or_rejected(replay, changed)
    duplicate['memory_bits'] = 64
    cert2, _ = translate(replay, h, es + [duplicate])
    assert (cert2['target_a'], cert2['target_b']) == (cert['target_a'], cert['target_b'])
    changed = copy.deepcopy(replay)
    stmt = changed['program']['instructions'][sid]
    if stmt['kind'] == 'load':
        stmt['result']['type']['bits'] = 64
    else:
        stmt['operands'][1]['type']['bits'] = 64
    absent_or_rejected(changed, es)
    print('8 typed-memory assertions passed')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--report', type=Path, required=True)
    p.add_argument('--events', type=Path, required=True)
    main(p.parse_args())
