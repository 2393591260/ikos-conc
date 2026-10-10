#!/usr/bin/env python3
"""Check every saved selected execution; retain small results, not raw reports."""
import argparse
from collections import Counter
import copy
import hashlib
import json
from pathlib import Path
from validate_bmc_execution import check, digest_json


def validate_report(report):
    results = []
    for index, row in enumerate(report.get('reports', [])):
        info = row.get('info', {})
        if 'confirmed_pair_id' not in info:
            continue
        replay = info.get('counterexample')
        if replay is None:
            replay = next((c['replay'] for c in info.get('bmc', {}).get('candidates', [])
                           if c.get('pair_id') == info['confirmed_pair_id'] and 'replay' in c), None)
        if replay is None:
            results.append(dict(group=index, status='UNSUPPORTED', reason='missing selected evidence'))
            continue
        certificate = copy.deepcopy(replay)
        program = certificate.pop('program')
        certificate['program_sha256'] = digest_json(program)
        results.append(dict(group=index, pair=info['confirmed_pair_id'],
                            program_sha256=certificate['program_sha256'],
                            certificate_sha256=digest_json(certificate), **check(program, certificate)))
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reports', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    rows = []
    for file in sorted(args.reports.rglob('report.json')):
        report = json.loads(file.read_text())
        result_file = file.parent / 'result.json'
        task = json.loads(result_file.read_text()) if result_file.exists() else {}
        source = Path(task.get('input', ''))
        rows.append(dict(task=task.get('task', str(file.relative_to(args.reports))),
                         input_sha256=hashlib.sha256(source.read_bytes()).hexdigest() if source.is_file() else None,
                         verdict=task.get('verdict'), executions=validate_report(report)))
    selected = [r for row in rows for r in row['executions']]
    task_statuses = Counter()
    for row in rows:
        statuses = {r['status'] for r in row['executions']}
        task_statuses['rejected' if 'REJECT' in statuses else 'all_accepted' if statuses == {'ACCEPT'} else
                      'partially_accepted' if 'ACCEPT' in statuses else 'unsupported'] += 1
    summary = dict(task_status_counts=dict(task_statuses), tasks=len(rows), tasks_with_selected_evidence=sum(bool(r['executions']) for r in rows),
                   executions=len(selected), status_counts=dict(Counter(r['status'] for r in selected)),
                   reason_counts=dict(Counter(r.get('reason', 'accepted') for r in selected)),
                   scope='Concrete AR execution with explicit API contracts; not official C/witness validation',
                   validator_sha256=hashlib.sha256(Path(__file__).with_name('validate_bmc_execution.py').read_bytes()).hexdigest())
    (args.output / 'results.json').write_text(json.dumps(rows, indent=2) + '\n')
    (args.output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    return int(any(r['status'] == 'REJECT' for r in selected))


if __name__ == '__main__':
    raise SystemExit(main())
