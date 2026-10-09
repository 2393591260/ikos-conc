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
    assert cert['count'] == 3
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
    e.append(duplicate)
    rejected(replay, e)
    r = copy.deepcopy(replay)
    t = next(t for t in r['traces'] if t['id'] == r['targets'][0]['trace'])
    t['steps'].append(copy.deepcopy(t['steps'][r['targets'][0]['step']]))
    rejected(r, es)
    print('6 mapping assertions passed')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--report', type=Path, required=True)
    p.add_argument('--events', type=Path, required=True)
    main(p.parse_args())
