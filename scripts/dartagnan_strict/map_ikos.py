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

NONDET = {'__VERIFIER_nondet_' + n for n in
          ('bool', 'char', 'uchar', 'short', 'ushort', 'int', 'uint', 'long',
           'ulong', 'longlong', 'ulonglong')}


def exact_function(program, trace, before, value, depth=0):
    if depth >= 32:
        return None
    if value.get('kind') == 'function':
        return value['id']
    if value.get('kind') != 'variable':
        return None
    instructions = program['instructions']
    owners = {b: name for name, fn in program['functions'].items() for b in fn.get('blocks', {})}
    owner = owners.get(instructions[trace['steps'][before]['statement']]['block'])
    if owner is None:
        return None
    for i in range(before - 1, -1, -1):
        step = trace['steps'][i]
        if 'synthetic_event' in step:
            continue
        ar = instructions[step['statement']]
        if owners.get(ar['block']) != owner:
            continue
        if ar['kind'] == 'return':
            return None
        if ar.get('result', {}).get('id') != value['id']:
            continue
        if (ar['kind'] == 'assign' or ar['kind'] == 'unary' and ar['operator'] == 14) and len(ar['operands']) == 1:
            return exact_function(program, trace, i, ar['operands'][0], depth + 1)
        return None
    return None


def same_source(ar, event):
    return event['line'] == ar.get('line') and Path(event['file']).resolve() == Path(ar.get('file', '')).resolve()


def memory_type(ar):
    if ar.get('kind') == 'load':
        return ar.get('result', {}).get('type', {})
    if ar.get('kind') == 'store':
        return ar['operands'][1]['type']
    return {}


def is_creation(ar):
    operands = ar.get('operands', [])
    return (ar.get('kind') == 'call' and len(operands) == 5
            and operands[0].get('kind') == 'function'
            and operands[0].get('id') in ('pthread_create', 'ar.pthread.create'))


def communication(replay, memory_bindings, initial_writes=None):
    """Bind known SC write order/read sources, leaving incomplete regions explicit."""
    mapped = {(m['trace'], m['step']): m['reference_event'] for m in memory_bindings}
    regions, unsupported, omitted = {}, set(), []
    unknown_writer = False
    for trace in replay['traces']:
        for i, step in enumerate(trace['steps']):
            ar = replay['program']['instructions'][step['statement']]
            if ar['kind'] not in ('load', 'store'):
                continue
            address = step.get('encoded_address', {})
            obj = address.get('object', {})
            if obj.get('kind') != 'global':
                if ar['kind'] == 'store' and obj.get('kind') != 'local':
                    unknown_writer = True
                continue
            name = obj['id']
            if 'offset' not in address or not address.get('width'):
                unsupported.add(name)
                continue
            region = (name, int(address['offset']), int(address['width']))
            # encode.hpp folds startup before either selected path. It has no SMT clock.
            if trace['id'] == 'startup':
                time = (0, i)
            elif 'clock' in step:
                time = (1, int(step['clock']))
            else:
                unsupported.add(name)
                continue
            regions.setdefault(region, []).append(dict(trace=trace['id'], step=i,
                kind=ar['kind'], time=time, event=mapped.get((trace['id'], i))))
    keys = list(regions)
    for i, (name, off, width) in enumerate(keys):
        for other, start, size in keys[i + 1:]:
            if name == other and off < start + size and start < off + width:
                unsupported.add(name)
    edges = []
    for region, accesses in regions.items():
        if region[0] in unsupported:
            omitted.append(dict(region=region, reason='incomplete/overlapping memory regions'))
            continue
        if len({a['time'] for a in accesses}) != len(accesses):
            omitted.append(dict(region=region, reason='memory clock is not unique'))
            continue
        writes = sorted((a for a in accesses if a['kind'] == 'store'), key=lambda a: a['time'])
        known = [a for a in writes if a['event'] is not None]
        for first, last in zip(known, known[1:]):
            edges.append(dict(relation='co', **{'from': first['event'], 'to': last['event']}))
        for read in (a for a in accesses if a['kind'] == 'load' and a['event'] is not None):
            prior = [w for w in writes if w['time'] < read['time']]
            if unknown_writer or any(w['event'] is None for w in writes):
                reason = 'unmapped/unknown write source'
            elif not prior:
                initial = (initial_writes or {}).get(region)
                if initial is not None:
                    edges.append(dict(relation='rf', **{'from': initial, 'to': read['event']}))
                    continue
                reason = 'implicit initial write not mapped'
            else:
                edges.append(dict(relation='rf', **{'from': prior[-1]['event'], 'to': read['event']}))
                continue
            omitted.append(dict(trace=read['trace'], step=read['step'], reason=reason))
    return edges, omitted


def initial_sources(replay, memory_bindings, ir_events):
    """Only identical integer fields can bind an implicit initial read source."""
    traces = {t['id']: t for t in replay['traces']}
    regions = {}
    for binding in memory_bindings:
        step = traces[binding['trace']]['steps'][binding['step']]
        ar = replay['program']['instructions'][step['statement']]
        if ar['kind'] != 'load':
            continue
        address = step['encoded_address']
        if address['object'].get('kind') != 'global':
            continue
        region = (address['object']['id'], int(address['offset']), int(address['width']))
        t = ar.get('result', {}).get('type', {})
        regions.setdefault(region, set()).add(t.get('bits') if t.get('kind') == 'integer' else None)
    sources = {}
    for (name, offset, width), bits in regions.items():
        if len(bits) != 1 or None in bits:
            continue
        matches = [e for e in ir_events if e.get('initial') ==
                   dict(object=name, offset=offset, bits=next(iter(bits)))
                   and (next(iter(bits)) + 7) // 8 == width]
        if len(matches) == 1:
            sources[name, offset, width] = matches[0]['id']
    return sources


def memory_orderings(replay, bindings):
    traces = {t['id']: t for t in replay['traces']}
    groups, omitted = {}, []
    for binding in bindings:
        trace, i = binding['trace'], binding['step']
        step = traces[trace]['steps'][i]
        if trace == 'startup':
            time = (0, i)
        elif 'clock' in step:
            time = (1, int(step['clock']))
        else:
            omitted.append(dict(trace=trace, step=i, reason='missing memory clock'))
            continue
        groups.setdefault(time, []).append(binding['reference_event'])
    ordered = [groups[t] for t in sorted(groups)]
    # Equal clocks do not specify an inter-thread order.
    return [dict(before=a, after=b) for first, last in zip(ordered, ordered[1:])
            for a in first for b in last], omitted


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
    heap_bindings = []
    heaps = {}
    for trace in traces.values():
        for step in trace['steps']:
            obj = step.get('encoded_address', {}).get('object', {})
            if obj.get('kind') != 'heap':
                continue
            sid = obj.get('allocation')
            if sid in heaps:
                if heaps[sid][1] != obj:
                    raise ValueError('inconsistent heap object claim')
                continue
            occurrences = [(t, i) for t in traces.values() for i, s in enumerate(t['steps'])
                           if s['statement'] == sid and 'synthetic_event' not in s]
            if obj.get('owner') != 'main' or obj.get('occurrence') != 1 or len(occurrences) != 1:
                raise ValueError('heap requires one actual main allocation')
            owner, index = occurrences[0]
            ar = instructions[sid]
            operands = ar.get('operands', [])
            callee = exact_function(replay['program'], owner, index, operands[0]) if operands else None
            fn = replay['program']['functions'].get(callee, {})
            extent = int(obj['extent'])
            if (threads[owner['id']] != 0 or ar.get('abstract_initializer') or ar['kind'] != 'call'
                    or callee != 'ar.libc.malloc' or not fn.get('declaration') or len(operands) != 2
                    or ar.get('result', {}).get('type', {}).get('kind') != 'pointer'
                    or operands[1].get('kind') != 'integer' or int(operands[1]['value']) != extent
                    or operands[1]['type'].get('bits') != replay['program']['pointer_bits']
                    or not 0 < extent <= 1048576):
                raise ValueError('unmapped malloc ABI/extent')
            sites = [s for s, stmt in instructions.items() if stmt['kind'] == 'call'
                     and not stmt.get('abstract_initializer') and same_source(stmt, ar)]
            matches = [e for e in ir_events if e['thread'] == 0 and same_source(ar, e)
                       and e.get('heap_extent') == extent]
            if sites != [sid] or len(matches) != 1:
                raise ValueError('ambiguous/unmapped allocation source occurrence')
            e = matches[0]
            if e['id'] in bound:
                raise ValueError('distinct heap allocations collapse to one event')
            heaps[sid] = e, obj
            bound[e['id']] = e
            claims[e['id']] = dict(allocation_extent=extent)
            heap_bindings.append(dict(trace=owner['id'], step=index, reference_event=e['id'], extent=extent))
    memory_bindings = []
    nondet_bindings = []
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
            if kind == 'call' and step['statement'] in heaps:
                continue
            if kind == 'call' and 'encoded_result' in step and not ar.get('abstract_initializer'):
                callee = exact_function(replay['program'], trace, index, ar['operands'][0])
                fn = replay['program']['functions'].get(callee, {})
                result_type = ar.get('result', {}).get('type', {})
                return_type = fn.get('return_type', {})
                if (callee in NONDET and fn.get('declaration') and not fn.get('parameter_types')
                        and len(ar['operands']) == 1 and result_type.get('kind') == 'integer'
                        and return_type.get('kind') == 'integer'
                        and (return_type.get('bits'), return_type.get('signed'))
                            == (result_type.get('bits'), result_type.get('signed'))):
                    matches = [e for e in ir_events if e['thread'] == threads[name]
                               and same_source(ar, e) and e.get('nondet')
                               and e.get('nondet_bits') == result_type['bits']]
                    sites = [sid for sid, stmt in instructions.items()
                             if stmt['kind'] == 'call' and not stmt.get('abstract_initializer')
                             and same_source(stmt, ar)]
                    # Repeated/optimized nondet sources need further occurrence mapping.
                    ordinal = sum(s['statement'] == step['statement'] and 'synthetic_event' not in s
                                  for s in trace['steps'][:index + 1])
                    if (len(matches) == 1 and sites == [step['statement']]
                            and ordinal == 1 and matches[0]['id'] not in bound):
                        bits = result_type['bits']
                        value = int(step['encoded_result'])
                        lo = -(1 << (bits - 1)) if result_type['signed'] else 0
                        hi = (1 << (bits - int(result_type['signed']))) - 1
                        if not lo <= value <= hi:
                            raise ValueError('nondet result outside AR integer range')
                        e = matches[0]
                        bound[e['id']] = e
                        claims[e['id']] = {'nondet_value': str(value % (1 << bits)), 'nondet_width': bits}
                        nondet_bindings.append(dict(trace=name, step=index, reference_event=e['id'], callee=callee))
                        continue
            if kind not in ('load', 'store') or obj.get('kind') not in ('global', 'heap'):
                omitted.append(dict(trace=name, step=index, kind=kind, reason='not a mapped global/heap memory access'))
                continue
            access_type = memory_type(ar)
            if access_type.get('kind') == 'integer' and (access_type['bits'] + 7) // 8 != int(address['width']):
                raise ValueError('AR integer type/access width mismatch')
            if access_type.get('kind') == 'pointer' and (heaps or replay['program']['pointer_bits'] != 64):
                # Dat3M's pointer storage is 64-bit even for C ILP32.
                omitted.append(dict(trace=name, step=index, kind=kind, reason='pointer storage/value binding omitted for heap or incompatible reference pointer ABI'))
                continue
            token = 'load(' if kind == 'load' else 'store('
            matches = [e for e in ir_events if e['thread'] == threads[name] and same_source(ar, e)
                       and token in e['code'] and (access_type.get('kind') != 'integer'
                           or 'memory_bits' not in e or e['memory_bits'] == access_type['bits'])]
            named = ([e for e in matches if re.search(r'\b' + re.escape(obj['id']) + r'\b', e['code'])]
                     if obj['kind'] == 'global' else [])
            if named:
                matches = named
            else:
                # Indirect addresses need not print the object's name. Only a
                # unique static source site may fall back to the SMT address claim.
                sites = [sid for sid, stmt in instructions.items()
                         if stmt['kind'] == kind and same_source(stmt, ar) and memory_type(stmt) == access_type]
                if sites != [step['statement']]:
                    matches = []
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
                         if stmt['kind'] == kind and same_source(stmt, matches[0]) and memory_type(stmt) == access_type]
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
            claim = dict(offset=address['offset'], width=address['width'])
            if obj['kind'] == 'heap':
                claim.update(heap_allocation=heaps[obj['allocation']][0]['id'], heap_extent=obj['extent'])
            else:
                claim['object'] = obj['id']
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
    initial_writes = initial_sources(replay, memory_bindings, ir_events)
    edges, communication_omissions = communication(replay, memory_bindings, initial_writes)
    used_initial = {edge['from'] for edge in edges if edge['relation'] == 'rf'} & set(initial_writes.values())
    initial_bindings = []
    for e in ir_events:
        if e['id'] not in used_initial:
            continue
        init = e['initial']
        bound[e['id']] = e
        claims[e['id']] = dict(object=init['object'], offset=init['offset'], width=(init['bits'] + 7) // 8)
        initial_bindings.append(dict(reference_event=e['id'], **init))
    selected = ([bound[i] for i in target_ids] + [e for i, e in bound.items() if i not in target_ids]
                + [e for e, _ in creators.values()])
    if target_ids[0] == target_ids[1]:
        raise ValueError('targets collapse to one dynamic event')
    cert = certificate(ir_hash, selected)
    for i, e in enumerate(selected):
        cert.update({f'event.{i}.{k}': v for k, v in claims.get(e['id'], {}).items()})
        if e['id'] in creators and creators[e['id']][1] is not None:
            cert[f'event.{i}.creation_occurrence'] = creators[e['id']][1]
    cert['edge_count'] = len(edges)
    for i, edge in enumerate(edges):
        for key, value in edge.items():
            cert[f'edge.{i}.{key}'] = value
    orders, order_omissions = memory_orderings(replay, memory_bindings)
    cert['order_count'] = len(orders)
    for i, order in enumerate(orders):
        for key, value in order.items():
            cert[f'order.{i}.{key}'] = value
    return cert, dict(scope='PARTIAL_FIXED_MEMORY_MODEL', threads=threads,
                      mapped_memory_events=len(memory_bindings), omitted_steps=omitted,
                      creation_bindings=creation_bindings,
                      heap_bindings=heap_bindings,
                      memory_bindings=memory_bindings,
                      nondet_bindings=nondet_bindings,
                      initial_bindings=initial_bindings,
                      communication_bindings=edges,
                      communication_omissions=communication_omissions,
                      memory_orderings=orders, memory_order_omissions=order_omissions,
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
