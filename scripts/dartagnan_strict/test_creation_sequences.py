#!/usr/bin/env python3
"""Check dynamic creation order across mutually exclusive loop branches."""
import argparse
import copy
import json
from pathlib import Path

from calibrate import events
from map_ikos import translate


def main(args):
    report = json.loads(args.report.read_text())
    replay = next(c['replay'] for g in report['reports']
                  for c in g.get('info', {}).get('bmc', {}).get('candidates', []) if 'replay' in c)
    h, es = events(args.events)
    cert, coverage = translate(replay, h, es)
    bindings = sorted(coverage['creation_bindings'], key=lambda b: b['creator_step'])
    assert len(bindings) == 2 and bindings[0]['parent_trace'] == bindings[1]['parent_trace']
    assert bindings[0]['reference_event'] < bindings[1]['reference_event']
    assert [b['occurrence'] for b in bindings] == [1, 1]
    assert [b['prefix_count'] for b in bindings] == [1, 2]

    # Traversal order of the child traces must not change their creators.
    changed = copy.deepcopy(replay)
    changed['traces'].reverse()
    other, other_coverage = translate(changed, h, es)
    assert sorted(other_coverage['creation_bindings'], key=lambda b: b['creator_step']) == bindings
    assert {k: v for k, v in cert.items() if k.startswith('target_')} == {
        k: v for k, v in other.items() if k.startswith('target_')}

    def rejected(r, ev):
        try:
            translate(r, h, ev)
        except ValueError:
            return
        raise AssertionError('ambiguous/corrupt creation mapping accepted')

    changed = copy.deepcopy(replay)
    child = next(t for t in changed['traces'] if 'creator_trace' in t)
    child['creator_step'] = 0
    rejected(changed, es)

    changed_events = copy.deepcopy(es)
    first = next(e for e in changed_events if e['id'] == bindings[0]['reference_event'])
    first['unrolling_id'] = -1
    rejected(replay, changed_events)

    # Removing all later creation clones must fail, not reuse an earlier branch.
    changed_events = [e for e in es if not ('ThreadCreate(' in e['code']
                                          and e['id'] > bindings[0]['reference_event'])]
    rejected(replay, changed_events)
    assert coverage['full_counterexample_validated'] is False
    print('10 creation-sequence assertions passed')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--report', type=Path, required=True)
    p.add_argument('--events', type=Path, required=True)
    main(p.parse_args())
