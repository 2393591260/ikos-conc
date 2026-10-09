#!/usr/bin/env python3
"""Constrain reference execution using unambiguous IKOS memory-event mappings.

PARTIAL evidence only: local/synchronization/CFG/schedule claims are reported as
unbound. SAT here must not be counted as full IKOS counterexample validation.
"""
import argparse
import copy
import json
from pathlib import Path
import re
from calibrate import build, certificate, digest, events, run, PIN


def same_source(ar, event):
    return event['line'] == ar.get('line') and Path(event['file']).resolve() == Path(ar.get('file', '')).resolve()


def is_creation(ar):
    operands = ar.get('operands', [])
    return (ar.get('kind') == 'call' and len(operands) == 5
            and operands[0].get('kind') == 'function'
            and operands[0].get('id') in ('pthread_create', 'ar.pthread.create'))


def translate(replay, ir_hash, ir_events):
    instructions = replay['program']['instructions']
    traces = {t['id']: t for t in replay['traces']}
    if len(traces) != len(replay['traces']):
        raise ValueError('duplicate trace identity')
    threads = {}
    creators, creation_bindings = {}, []
    remaining = dict(traces)
    for _ in range(len(traces)):
        progress = False
        for name, trace in list(remaining.items()):
            if trace['entry'] == 'main' and 'creator_trace' not in trace:
                threads[name] = 0
            elif trace.get('creator_trace') in threads:
                parent = traces[trace['creator_trace']]
                index = trace.get('creator_step')
                if type(index) is not int or not 0 <= index < len(parent['steps']):
                    raise ValueError('invalid creator step')
                ar = instructions[parent['steps'][trace['creator_step']]['statement']]
                operands = ar.get('operands', [])
                if (not is_creation(ar)
                        or operands[3].get('kind') != 'function'
                        or operands[3].get('id') != trace['entry']):
                    raise ValueError('creator step is not a matching direct thread creation')
                matches = [e for e in ir_events if e['thread'] == threads[parent['id']]
                           and same_source(ar, e) and 'ThreadCreate(' in e['code']
                           and not e['code'].startswith('ThreadStart')]
                if not matches:
                    raise ValueError('ambiguous/unmapped dynamic creator: ' + name)
                matches.sort(key=lambda e: e['id'])
                sites = [sid for sid, stmt in instructions.items()
                         if is_creation(stmt) and same_source(stmt, matches[0])]
                statement = parent['steps'][trace['creator_step']]['statement']
                if sites != [statement]:
                    raise ValueError('ambiguous AR creation site: ' + name)
                ordinal = sum(step['statement'] == statement and 'synthetic_event' not in step
                              for step in parent['steps'][:trace['creator_step'] + 1])
                if not 1 <= ordinal <= len(matches):
                    raise ValueError('creation occurrence exceeds reference bound: ' + name)
                occurrence = None
                if len(matches) > 1:
                    origins = {e.get('unrolling_id', -1) for e in matches}
                    if len(origins) != 1 or -1 in origins:
                        raise ValueError('ambiguous/unmapped dynamic creator: ' + name)
                    occurrence = ordinal
                creator = matches[(occurrence or 1) - 1]
                if not re.match(r'ThreadCreate\(T\d+:' + re.escape(trace['entry']) + r',', creator['code']):
                    raise ValueError('reference creation entry mismatch: ' + name)
                children = {e['thread'] for e in ir_events if e['creator'] == creator['id']}
                if len(children) != 1:
                    raise ValueError('ambiguous/unmapped created thread: ' + name)
                threads[name] = children.pop()
                creators[creator['id']] = creator, occurrence
                creation_bindings.append(dict(trace=name, parent_trace=parent['id'],
                                              creator_step=trace['creator_step'], reference_event=creator['id'],
                                              occurrence=occurrence, pre_unroll_id=creator.get('unrolling_id', -1)))
            else:
                continue
            if threads[name] in (t for n, t in threads.items() if n != name):
                raise ValueError('distinct traces collapse to one reference thread')
            del remaining[name]; progress = True
        if not remaining or not progress:
            break
    if remaining:
        raise ValueError('unmapped thread traces: ' + ','.join(remaining))
    bound, omitted, mapped_steps, claims = {}, [], {}, {}
    memory_bindings = []
    for name, trace in traces.items():
        values = {}
        for index, step in enumerate(trace['steps']):
            ar = instructions[step['statement']]
            if ar.get('result', {}).get('kind') == 'variable':
                values.pop(ar['result']['id'], None)
                if 'encoded_result' in step:
                    values[ar['result']['id']] = step['encoded_result']
            address = step.get('encoded_address', {})
            obj = address.get('object', {})
            kind = ar['kind']
            if kind not in ('load', 'store') or obj.get('kind') != 'global':
                omitted.append(dict(trace=name, step=index, kind=kind, reason='not a mapped global memory access'))
                continue
            token = 'load(' if kind == 'load' else 'store('
            matches = [e for e in ir_events if e['thread'] == threads[name] and same_source(ar, e)
                       and token in e['code'] and re.search(r'\b' + re.escape(obj['id']) + r'\b', e['code'])]
            if not matches:
                omitted.append(dict(trace=name, step=index, kind=kind, reason='ambiguous/unmapped memory event'))
                continue
            ordinal = sum(s['statement'] == step['statement'] for s in trace['steps'][:index + 1])
            if ordinal > len(matches):
                raise ValueError('memory occurrence exceeds reference bound')
            occurrence = None
            if len(matches) > 1:
                origins = {e.get('unrolling_id', -1) for e in matches}
                sites = [sid for sid, stmt in instructions.items()
                         if stmt['kind'] == kind and same_source(stmt, matches[0])]
                if len(origins) != 1 or -1 in origins or sites != [step['statement']]:
                    omitted.append(dict(trace=name, step=index, kind=kind, reason='distinct/unknown static memory origins'))
                    continue
                occurrence = ordinal
            matches.sort(key=lambda e: e['id'])
            e = matches[ordinal - 1]
            if e['id'] in bound:
                raise ValueError('dynamic occurrences collapse to one reference event')
            bound[e['id']] = e
            mapped_steps[name, index] = e['id']
            claim = dict(object=obj['id'], offset=address['offset'], width=address['width'])
            if occurrence is not None:
                claim['memory_occurrence'] = occurrence
            memory_bindings.append(dict(trace=name, step=index, reference_event=e['id'],
                                        occurrence=occurrence, pre_unroll_id=e.get('unrolling_id', -1)))
            if kind == 'load' and 'encoded_result' in step:
                claim['value'] = str(int(step['encoded_result']) % (1 << ar['result']['type']['bits']))
            elif kind == 'store':
                v = ar['operands'][1]
                val = v.get('value') if v['kind'] == 'integer' else values.get(v.get('id'))
                if val is not None and v['type']['kind'] == 'integer':
                    claim['value'] = str(int(val) % (1 << v['type']['bits']))
            claims[e['id']] = claim
    target_ids = []
    for target in replay['targets']:
        key = target['trace'], target['step']
        if key not in mapped_steps:
            raise ValueError('chosen target is unmapped')
        target_ids.append(mapped_steps[key])
    selected = ([bound[i] for i in target_ids] + [e for i, e in bound.items() if i not in target_ids]
                + [e for e, _ in creators.values()])
    if target_ids[0] == target_ids[1]:
        raise ValueError('targets collapse to one dynamic event')
    cert = certificate(ir_hash, selected)
    for i, e in enumerate(selected):
        cert.update({f'event.{i}.{k}': v for k, v in claims.get(e['id'], {}).items()})
        if e['id'] in creators and creators[e['id']][1] is not None:
            cert[f'event.{i}.creation_occurrence'] = creators[e['id']][1]
    return cert, dict(scope='PARTIAL_FIXED_MEMORY_MODEL', threads=threads,
                      mapped_memory_events=len(bound), omitted_steps=omitted,
                      creation_bindings=creation_bindings,
                      memory_bindings=memory_bindings,
                      unbound_claims=['CFG and call occurrence mapping', 'local values and nondet occurrences',
                                      'synchronization history and identities', 'IKOS total schedule and rf choices'],
                      full_counterexample_validated=False)


def main(args):
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    cp = build(args, out)
    report = json.loads(args.report.read_text())
    rows = []
    for report_index, group in enumerate(report['reports']):
        for pair in group.get('info', {}).get('bmc', {}).get('candidates', []):
            replay = pair.get('replay')
            if not replay:
                continue
            bits = replay['program']['pointer_bits']
            folder = out / f'group{report_index}-pair{pair["pair_id"]}'
            folder.mkdir()
            row = dict(report_index=report_index, pair_id=pair['pair_id'], full_counterexample_validated=False)
            source_row = run(args, cp, args.input.resolve(), bits, folder/'source')
            try:
                h, es = events(folder/'source/events.tsv')
                cert, coverage = translate(replay, h, es)
                row.update(coverage)
                result = run(args, cp, args.input.resolve(), bits, folder/'fixed-memory', cert)
                row['result'] = result
                # A real IKOS-derived fixed store value is checked, not reference-generated evidence.
                stores = [(i, e) for i, e in enumerate(cert.keys()) if e.endswith('.value')]
                if stores:
                    key = stores[0][1]
                    corrupted = copy.deepcopy(cert); corrupted[key] = int(cert[key]) + 1
                    row['corruption_probe'] = run(args, cp, args.input.resolve(), bits, folder/'changed-value', corrupted)
            except (ValueError, FileNotFoundError) as exc:
                row.update(status='UNMAPPED', reason=str(exc))
            rows.append(row)
            print(json.dumps({k:v for k,v in row.items() if k != 'omitted_steps'}), flush=True)
    summary = dict(reference_commit=PIN, report_sha256=digest(args.report),
                   input_sha256=digest(args.input), rows=rows,
                   warning='Partial fixed-memory evidence; no full-counterexample verification claim')
    (out/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')


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
