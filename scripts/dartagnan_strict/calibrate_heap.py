#!/usr/bin/env python3
"""Check actual IKOS heap bindings and reject corrupted partial certificates."""
import argparse
import copy
import json
import os
from pathlib import Path
import subprocess

from calibrate import PIN, ROOT, build, digest, events, run
from map_ikos import translate


def main(args):
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    cp = build(args, out)
    rows = []
    for bits in (32, 64):
        source = ROOT / 'docs/review-repros/dartagnan-strict/heap-two-allocations.c'
        folder = out / str(bits)
        folder.mkdir()
        report = folder / 'report.json'
        cmd = [str(ROOT / 'install/bin/ikos'), '--analyses=race', '--concurrency=auto',
               '--demote-race-to-unknown', '-m', str(bits), '--format=json',
               '--report-file=' + str(report), '-o', str(folder / 'output.db'), str(source)]
        proc = subprocess.run(cmd, env=dict(os.environ, IKOS_BMC_RECOVER='1', IKOS_BMC_DIAGNOSTICS='1'),
                              capture_output=True, text=True, timeout=30)
        (folder / 'ikos.log').write_text(proc.stdout + proc.stderr)
        assert proc.returncode == 0 and 'The program is definitely UNSAFE' in proc.stdout + proc.stderr
        replays = [c['replay'] for g in json.loads(report.read_text())['reports']
                   for c in g.get('info', {}).get('bmc', {}).get('candidates', []) if c.get('replay')]
        assert replays
        native = run(args, cp, source, bits, folder / 'native')
        assert native['status'] == 'SOURCE_RACE', native
        h, es = events(folder / 'native/events.tsv')
        cert, coverage = translate(replays[0], h, es)
        assert coverage['heap_bindings'] and not coverage['full_counterexample_validated']
        cases = [('valid-ikos-heap', cert, 'SAT')]
        address_key = next(k for k in cert if k.endswith('.heap_allocation'))
        prefix = address_key.removesuffix('heap_allocation')
        allocation_id = cert[address_key]
        allocator_prefix = next(k.removesuffix('id') for k in cert
                                if k.startswith('event.') and k.endswith('.id') and cert[k] == allocation_id)

        def change(name, updates, expected='UNSUPPORTED'):
            changed = copy.deepcopy(cert)
            changed.update(updates)
            cases.append((name, changed, expected))

        change('wrong-value', {prefix + 'value': int(cert[prefix + 'value']) + 1}, 'UNSAT_AT_BOUND')
        change('wrong-extent', {prefix + 'heap_extent': 8})
        change('wrong-width', {prefix + 'width': 1})
        change('out-of-bounds', {prefix + 'offset': 4})
        change('unknown-allocation', {address_key: 99999})
        change('wrong-allocation-thread', {allocator_prefix + 'thread': 1})
        change('wrong-allocation-descriptor', {allocator_prefix + 'descriptor': 'bm90LWFuLWV2ZW50'})
        change('wrong-allocation-extent', {allocator_prefix + 'allocation_extent': 8})
        change('conflicting-object-kind', {prefix + 'object': 'pointer'})
        # Bind a second genuine, executed, same-sized malloc; it cannot supply
        # the first allocation's address even when descriptors are all valid.
        other = next(e for e in es if e.get('heap_extent') == 4 and e['id'] != allocation_id)
        changed = copy.deepcopy(cert)
        if other['id'] not in (v for k, v in changed.items() if k.startswith('event.') and k.endswith('.id')):
            index = int(changed['count'])
            changed['count'] = index + 1
            for key in ('id', 'thread', 'creator', 'descriptor'):
                changed[f'event.{index}.{key}'] = other[key]
            changed[f'event.{index}.allocation_extent'] = 4
        changed[address_key] = other['id']
        cases.append(('other-real-allocation', changed, 'UNSAT_AT_BOUND'))
        changed = {k: v for k, v in cert.items() if not k.startswith('event.')}
        entries = [{k.rsplit('.', 1)[1]: v for k, v in cert.items() if k.startswith(f'event.{i}.')}
                   for i in range(int(cert['count']))]
        entries = [e for e in entries if e['id'] != allocation_id]
        changed['count'] = len(entries)
        for i, entry in enumerate(entries):
            changed.update({f'event.{i}.{key}': value for key, value in entry.items()})
        cases.append(('unbound-allocation', changed, 'UNSUPPORTED'))
        for name, c, expected in cases:
            row = run(args, cp, source, bits, folder / name, c)
            row.update(case=name, expected=expected, matched=row['status'] == expected,
                       full_counterexample_validated=False)
            rows.append(row)
            print(bits, name, row['status'], 'expected', expected, flush=True)
        changed_replay = copy.deepcopy(replays[0])
        for trace in changed_replay['traces']:
            for step in trace['steps']:
                obj = step.get('encoded_address', {}).get('object', {})
                if obj.get('kind') == 'heap':
                    obj['occurrence'] = 2
        try:
            translate(changed_replay, h, es)
        except ValueError:
            rows.append(dict(case='unsupported-occurrence', data_model=str(bits), matched=True,
                             full_counterexample_validated=False))
        else:
            raise AssertionError('repeated heap instance claim accepted')
    summary = dict(reference_commit=PIN, adapter_sha256=digest(Path(__file__).with_name('StrictWitness.java')),
                   rows=rows, matched=all(r['matched'] for r in rows), full_counterexample_validated=False)
    (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    assert summary['matched'], summary


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--reference', type=Path, default=Path('/home/ruan/dartagnan-svcomp2026/dartagnan'))
    p.add_argument('--source', type=Path, default=Path('/tmp/ikos-dartagnan-source/Dat3M-' + PIN))
    p.add_argument('--javac', type=Path, default=Path('/tmp/ikos-reference-java/jdk/usr/lib/jvm/java-17-openjdk-amd64/bin/javac'))
    p.add_argument('--timeout', type=int, default=60)
    p.add_argument('--bound', type=int, default=2)
    main(p.parse_args())
