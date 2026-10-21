#!/usr/bin/env python3
"""Check shared heap instances, dynamic arguments, and GEP boundaries on ILP32."""
import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from validate_bmc_execution import Replay, Reject, Unsupported, check, digest_json

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from analyzer.python.ikos.race_result import validate_race_result


def gep_contract():
    replay = Replay.__new__(Replay)
    replay.semaphores = {}
    replay.program = dict(pointer_bits=32)
    replay.objects = {'heap': dict(size=8, live=True, thread_local=False, allocation='alloc')}
    replay.memory = {'heap': [None] * 8}
    inst = dict(inbounds=True, index_bits=32, factors=['4'],
                operands=[{}, dict(type=dict(bits=64))])
    assert replay.pointer_shift(inst, [('heap', 0), 2]) == ('heap', 8)
    assert replay.pointer_shift(inst, [('heap', 8), -2]) == ('heap', 0)
    bad = [(inst, [('heap', 0), 3]), (inst, [('heap', 0), 1 << 32]),
           (dict(inst, factors=['4', '4'], operands=[{}, inst['operands'][1], inst['operands'][1]]),
            [('heap', 0), 3, -3]),
           (dict(inst, factors=['2147483648']), [('heap', 0), 2]),
           (dict(inst, factors=[]), [('heap', 0), 0]),
           (dict(inst, operands=[{}, dict(type=dict(bits=1))]), [('heap', 0), 1])]
    for operation, values in bad:
        try: replay.pointer_shift(operation, values)
        except Reject: pass
        else: raise AssertionError('accepted invalid/truncated/intermediate GEP')
    for operation in (dict(inst, inbounds=False), dict(inst, index_bits=64)):
        try: replay.pointer_shift(operation, [('heap', 0), 0])
        except Unsupported: pass
        else: raise AssertionError('accepted unsupported heap GEP metadata')
    replay.objects['heap']['live'] = False
    try: replay.pointer_shift(inst, [('heap', 0), 0])
    except Reject: pass
    else: raise AssertionError('accepted expired heap GEP')
    return 11


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    args = parser.parse_args()
    text = (ROOT / 'analyzer/test/regression/bmc-auxiliary/shared-heap.c').read_text()
    cases = [(mode, 0, variant, True) for mode in range(5)
             for variant in ('renamed', 'wide', 'constant', 'helper', 'guard-helper', 'select-helper')]
    cases += [(0, case, 'renamed', False) for case in (1, 2, 3, 5, 6, 7, 9, 11, 13, 17)]
    cases += [(3, case, 'renamed', False) for case in (4, 8)]
    cases += [(0, case, 'renamed', True) for case in (10, 12, 15, 16)]
    assertions = gep_contract()
    with tempfile.TemporaryDirectory(prefix='ikos-shared-heap-') as temp:
        work = Path(temp)
        for mode, case, variant, confirmed in cases:
            body = text.replace('worker', 'entry').replace('published', 'shared_base')
            if variant == 'helper':
                body = body.replace('void *entry(void *argument) {',
                    'void update(struct cell *p) { p->data = __VERIFIER_nondet_int(); }\nvoid *entry(void *argument) {')
                # Replace only the worker write, preserving the helper body.
                split = body.index('void *entry(')
                body = body[:split] + body[split:].replace('p->data = __VERIFIER_nondet_int();', 'update(p);')
            elif variant == 'guard-helper':
                body = body.replace('void *entry(void *argument) {',
                    'void check(int condition) { if (!condition) abort(); }\nvoid *entry(void *argument) {')
                body = body.replace('p->data = __VERIFIER_nondet_int();',
                    'if (__VERIFIER_nondet_int()) check(0);\ncheck(1); check(2);\np->data = __VERIFIER_nondet_int();')
            elif variant == 'select-helper':
                body = body.replace('void *entry(void *argument) {',
                    'void update(struct cell *p, int enabled) { if (enabled) p->data = __VERIFIER_nondet_int(); }\nvoid *entry(void *argument) {')
                split = body.index('void *entry(')
                body = body[:split] + body[split:].replace('p->data = __VERIFIER_nondet_int();', 'update(p, 0); update(p, 1);')
            source = work / 'case.c'; report = work / 'report.json'
            source.write_text(f'#define MODE {mode}\n#define CASE {case}\n' +
                              ('#define WIDE\n' if variant == 'wide' else '') +
                              ('#define CONSTANT\n' if variant == 'constant' else '') + body)
            report.unlink(missing_ok=True)
            result = subprocess.run([str(args.ikos.resolve()), '--analyses=race', '--concurrency=auto',
                '--demote-race-to-unknown', '-m', '32', '--format=json', '--report-file=' + str(report),
                '-o', str(work / 'output.db'), str(source)],
                env=dict(os.environ, IKOS_BMC_RECOVER='1'), capture_output=True, text=True, timeout=40)
            assert result.returncode == 0, result.stdout + result.stderr
            data = json.loads(report.read_text())
            metadata = validate_race_result(data)
            # MODE4 with constant count=2 passes only numeric zero (NULL).
            if mode == 3 or (mode == 4 and variant != 'constant'):
                assert metadata['verdict'] == 'UNKNOWN' and metadata['coverage'] == 'Blocked', metadata
                assert not metadata['search_performed'], metadata
                assert any(o['reason'] == 'integer-derived pointer aliasing is not modeled'
                           and not o['contract'] for o in metadata['obligations']), metadata
                confirmed = False
            certificates = [row['info']['counterexample'] for row in data['reports'] if 'counterexample' in row['info']]
            assert bool(certificates) == confirmed, (mode, case, variant, result.stdout + result.stderr)
            assertions += 1
            for execution in certificates:
                certificate = copy.deepcopy(execution); program = certificate.pop('program')
                certificate['program_sha256'] = digest_json(program)
                checked = check(program, certificate)
                assert checked['status'] == 'ACCEPT', (mode, case, variant, checked)
                assertions += 1
                # A certificate must not silently transfer one worker's heap address to another.
                for trace in certificate['traces']:
                    for step in trace['steps']:
                        address = step.get('encoded_address', {})
                        if address.get('object', {}).get('kind') != 'heap' or not address.get('width'): continue
                        corrupted = copy.deepcopy(certificate)
                        target = corrupted['traces'][certificate['traces'].index(trace)]['steps'][trace['steps'].index(step)]
                        target['encoded_address']['object']['allocation'] = 'wrong-allocation'
                        assert check(program, corrupted)['status'] == 'REJECT'
                        assertions += 1
                        break
            print(f'{mode}/{case}/{variant}: PASS', flush=True)
    print(f'{assertions} shared heap assertions passed')


if __name__ == '__main__':
    main()
