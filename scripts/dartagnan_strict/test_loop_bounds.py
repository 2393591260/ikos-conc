#!/usr/bin/env python3
"""Calibrate the extra loop-header visit required to exit a creation loop."""
import argparse
import json
from pathlib import Path
from types import SimpleNamespace
from calibrate import ROOT, PIN, build, certificate, digest, events, run


def main(args):
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    cp = build(args, out)
    source = ROOT / 'docs/review-repros/dartagnan-strict/loop-create-nondet.c'
    rows = []
    for bits in (32, 64):
        for bound in (3, 4):
            config = SimpleNamespace(**vars(args), bound=bound)
            folder = out / f'{bits}-bound{bound}'
            folder.mkdir()
            native = run(config, cp, source, bits, folder/'source')
            assert native['status'] == 'SOURCE_RACE', native
            h, es = events(folder/'source/events.tsv')
            creates = [e for e in es if e['code'].startswith('ThreadCreate(')]
            reads = [e for e in es if e['thread'] == 0 and 'load(&data)' in e['code']]
            choices = [e for e in es if e['thread'] == 0 and e.get('nondet')]
            assert len(creates) == bound and len(reads) == len(choices) == 1
            first = min(creates, key=lambda e: e['id'])
            stores = [e for e in es if e['creator'] == first['id'] and 'store(&data,' in e['code']]
            assert len(stores) == 1
            cert = certificate(h, [stores[0], reads[0], choices[0], first])
            cert.update({'event.0.value': 7, 'event.1.value': 7,
                         'event.2.nondet_width': 32, 'event.2.nondet_value': 3,
                         'event.3.creation_occurrence': 1})
            row = run(config, cp, source, bits, folder/'fixed-count3', cert)
            expected = 'UNSAT_AT_BOUND' if bound == 3 else 'SAT'
            row.update(bound=bound, fixed_count=3, expected=expected, matched=row['status'] == expected)
            rows.append(row)
            print(bits, bound, row['status'], 'expected', expected, flush=True)
    summary = dict(reference_commit=PIN, adapter_sha256=digest(Path(__file__).with_name('StrictWitness.java')),
                   scope='bounded creation-loop exit calibration; no IKOS validation claim',
                   results=rows, passed=all(r['matched'] for r in rows))
    (out/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    assert summary['passed']


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--reference', type=Path, default=Path('/home/ruan/dartagnan-svcomp2026/dartagnan'))
    p.add_argument('--source', type=Path, default=Path('/tmp/ikos-dartagnan-source/Dat3M-' + PIN))
    p.add_argument('--javac', type=Path, default=Path('/tmp/ikos-reference-java/jdk/usr/lib/jvm/java-17-openjdk-amd64/bin/javac'))
    p.add_argument('--timeout', type=int, default=60)
    main(p.parse_args())
