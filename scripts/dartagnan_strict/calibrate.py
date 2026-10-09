#!/usr/bin/env python3
"""Build an isolated pinned Dat3M hook and calibrate processed-IR constraints.

This does not yet translate arbitrary IKOS certificates or validate YAML 2.2.
No changes are made to the reference installation or analyzer.
"""
import argparse
import base64
import copy
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PIN = '7caef5c850dc2dbf81c5554dcf41e402007f7a19'
ORIGINAL_SHA = 'af24b399c1fbef3f8287197d3904977db3634457b4741461618d727b5feecdf4'


def digest(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError('pinned source anchor mismatch: ' + old)
    return text.replace(old, new)


def build(args, out):
    source = args.source / 'dartagnan/src/main/java/com/dat3m/dartagnan/verification/solving/AssumeSolver.java'
    if digest(source) != ORIGINAL_SHA:
        raise ValueError('AssumeSolver source does not match pinned version')
    text = source.read_text()
    text = replace_once(text, 'prover.addConstraint(symmetryEncoder.encodeFullSymmetryBreaking());',
                        'StrictWitness.constrain(context, prover);\n'
                        '        if (!StrictWitness.enabled()) prover.addConstraint(symmetryEncoder.encodeFullSymmetryBreaking());')
    text = replace_once(text, 'prover.writeComment("Bound encoding");',
                        'if (StrictWitness.enabled()) System.out.println("REFERENCE_CERTIFICATE_UNSAT_AT_BOUND");\n'
                        '            prover.writeComment("Bound encoding");')
    text = replace_once(text, 'res = FAIL;',
                        'res = FAIL;\n'
                        '            if (StrictWitness.enabled()) System.out.println("REFERENCE_CERTIFICATE_SAT");')
    java = out / 'AssumeSolver.java'
    java.write_text(text)
    classes = out / 'classes'
    classes.mkdir()
    target = args.reference / 'dartagnan/target'
    cp = str(target / 'dartagnan.jar') + ':' + str(target / 'libs/*')
    cmd = [str(args.javac), '--release', '17', '-cp', cp, '-d', str(classes),
           str(java), str(HERE / 'StrictWitness.java')]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    (out / 'compile.log').write_text(proc.stdout + proc.stderr)
    if proc.returncode:
        raise RuntimeError(proc.stderr)
    return str(classes) + ':' + cp


def write_certificate(path, cert):
    # All strings are decimal IDs, relation names, hex hashes or base64: no Java
    # properties escapes, duplicate keys or multiline values are admitted.
    if any('\n' in str(v) or '\r' in str(v) for v in cert.values()):
        raise ValueError('multiline property')
    path.write_text(''.join(f'{k}={v}\n' for k, v in cert.items()))


def run(args, cp, source, bits, folder, cert=None):
    folder.mkdir()
    env = dict(os.environ)
    for key in ('IKOS_REFERENCE_PROBE', 'IKOS_STRICT_CERTIFICATE', 'IKOS_STRICT_EVENT_DUMP'):
        env.pop(key, None)
    env.update(DAT3M_HOME=str(args.reference), DAT3M_OUTPUT=str(folder / 'output'),
               CFLAGS=f'-fgnu89-inline -m{bits}', IKOS_STRICT_EVENT_DUMP=str(folder / 'events.tsv'))
    if cert is not None:
        write_certificate(folder / 'certificate.properties', cert)
        env['IKOS_STRICT_CERTIFICATE'] = str(folder / 'certificate.properties')
    cmd = ['java', '-Xmx2g', '-cp', cp, 'com.dat3m.dartagnan.Dartagnan',
           'cat/svcomp.cat', str(source), 'svcomp.properties', '--property=DATARACEFREEDOM',
           f'--bound={getattr(args, "bound", 2)}', '--program.processing.skipAssertionsOfType=USER']
    start = time.monotonic()
    proc = subprocess.Popen(cmd, cwd=args.reference, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, start_new_session=True)
    try:
        log, _ = proc.communicate(timeout=args.timeout)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        log, _ = proc.communicate()
        status = 'TIMEOUT'
    else:
        text = log.decode(errors='replace')
        if proc.returncode not in (0, 13):
            status = 'UNSUPPORTED' if 'REFERENCE_CERTIFICATE_UNSUPPORTED:' in text else 'ERROR'
        elif 'REFERENCE_CERTIFICATE_SAT' in text:
            status = 'SAT'
        elif 'REFERENCE_CERTIFICATE_UNSAT_AT_BOUND' in text:
            status = 'UNSAT_AT_BOUND'
        elif 'Result: FAIL' in text:
            status = 'SOURCE_RACE'
        else:
            status = 'OTHER'
    (folder / 'run.log').write_bytes(log)
    row = dict(status=status, returncode=proc.returncode, seconds=time.monotonic()-start,
               command=cmd, source_sha256=digest(source), data_model=f'{bits}')
    (folder / 'result.json').write_text(json.dumps(row, indent=2) + '\n')
    return row


def events(path):
    lines = path.read_text().splitlines()
    rows = []
    for line in lines[1:]:
        eid, thread, creator, file64, lineno, code64 = line.split('\t')
        rows.append(dict(id=int(eid), thread=int(thread), creator=int(creator),
                         file=base64.b64decode(file64).decode(), line=int(lineno),
                         code=base64.b64decode(code64).decode(),
                         descriptor=base64.b64encode(line.encode()).decode()))
    return lines[0], rows


def certificate(ir_hash, targets):
    cert = dict(schema=1, ir_sha256=ir_hash, count=len(targets), edge_count=0,
                target_a=targets[0]['id'], target_b=targets[1]['id'])
    for i, e in enumerate(targets):
        for key in ('id', 'thread', 'creator', 'descriptor'):
            cert[f'event.{i}.{key}'] = e[key]
    return cert


def calibrate(args):
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    cp = build(args, out)
    results = []
    source = ROOT / 'docs/review-repros/dartagnan-strict/same-entry.c'
    for bits in (32, 64):
        baseline_folder = out / f'{bits}-source'
        baseline = run(args, cp, source, bits, baseline_folder)
        if baseline['status'] != 'SOURCE_RACE':
            raise AssertionError(baseline)
        h, es = events(baseline_folder / 'events.tsv')
        stores = [e for e in es if e['line'] == 5 and 'store(' in e['code']]
        if len(stores) != 2 or stores[0]['thread'] == stores[1]['thread']:
            raise AssertionError('expected two exact worker instances: ' + repr(stores))
        cert = certificate(h, stores)
        cert['event.0.value'] = cert['event.1.value'] = 7
        for i in (0, 1):
            cert.update({f'event.{i}.object': 'data', f'event.{i}.offset': 0, f'event.{i}.width': 4})
        cases = [('valid', cert, 'SAT')]
        def change(name, updates, expected='UNSUPPORTED'):
            c = copy.deepcopy(cert); c.update(updates); cases.append((name, c, expected))
        change('wrong-object', {'event.0.object': 'other'}, 'UNSAT_AT_BOUND')
        change('wrong-width', {'event.0.width': 1})
        change('unknown-object', {'event.0.object': 'nonexistent'})
        change('out-of-range-value', {'event.0.value': 2**32})
        change('wrong-descriptor', {'event.0.descriptor': 'bm90LWFuLWV2ZW50'})
        change('wrong-thread', {'event.0.thread': 999})
        change('wrong-creator', {'event.0.creator': stores[1]['creator']})
        change('wrong-source-ir', {'ir_sha256': '0' * 64})
        change('unknown-field', {'silently_ignore': 1})
        change('wrong-value', {'event.0.value': 8}, 'UNSAT_AT_BOUND')
        change('wrong-address', {'event.0.address': 0}, 'UNSAT_AT_BOUND')
        change('same-instance', {'target_b': stores[0]['id']}, 'UNSAT_AT_BOUND')
        other = next(e for e in es if e['line'] == 6 and 'store(' in e['code'] and e['thread'] == stores[1]['thread'])
        c = certificate(h, [stores[0], other])
        cases.append(('other-race-cannot-substitute', c, 'UNSAT_AT_BOUND'))
        c = copy.deepcopy(cert)
        # These are explicitly CAT hb constraints, NOT an IKOS total schedule.
        c.update(edge_count=2)
        for i, (a, b) in enumerate(((stores[0], stores[1]), (stores[1], stores[0]))):
            c.update({f'edge.{i}.relation': 'hb', f'edge.{i}.from': a['id'], f'edge.{i}.to': b['id']})
        cases.append(('cyclic-order', c, 'UNSAT_AT_BOUND'))
        for name, c, expected in cases:
            row = run(args, cp, source, bits, out / f'{bits}-{name}', c)
            row.update(case=name, expected=expected, matched=row['status'] == expected)
            results.append(row)
            print(bits, name, row['status'], 'expected', expected, flush=True)
        locked = source.with_name('same-entry-locked.c')
        folder = out / f'{bits}-locked-source'
        base = run(args, cp, locked, bits, folder)
        if base['status'] != 'OTHER' or base['returncode'] != 0:
            raise AssertionError(base)
        h, es = events(folder / 'events.tsv')
        targets = [e for e in es if e['line'] == 7 and 'store(' in e['code']]
        if len(targets) != 2:
            raise AssertionError(targets)
        row = run(args, cp, locked, bits, out / f'{bits}-common-lock', certificate(h, targets))
        row.update(case='common-lock', expected='UNSAT_AT_BOUND', matched=row['status'] == 'UNSAT_AT_BOUND')
        results.append(row)
        print(bits, 'common-lock', row['status'], flush=True)
    summary = dict(reference_commit=PIN, adapter_sha256=digest(HERE/'StrictWitness.java'),
                   reference_jar_sha256=digest(args.reference/'dartagnan/target/dartagnan.jar'),
                   scope='processed-IR constraints; not arbitrary IKOS witness validation',
                   results=results, passed=all(r['matched'] for r in results))
    (out / 'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    if not summary['passed']:
        raise AssertionError('calibration failed')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--reference', type=Path, default=Path('/home/ruan/dartagnan-svcomp2026/dartagnan'))
    p.add_argument('--source', type=Path, default=Path('/tmp/ikos-dartagnan-source/Dat3M-' + PIN))
    p.add_argument('--javac', type=Path, default=Path('/tmp/ikos-reference-java/jdk/usr/lib/jvm/java-17-openjdk-amd64/bin/javac'))
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--timeout', type=int, default=60)
    p.add_argument('--bound', type=int, default=2)
    calibrate(p.parse_args())
