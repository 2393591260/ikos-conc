#!/usr/bin/env python3
"""Check typed nested-array certificates independently of BMC."""
import argparse
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    args = parser.parse_args()
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/mutex-array-effects.c').read_text()
    original = 'struct element *heads[COUNT];\npthread_mutex_t guards[COUNT];'
    variants = {}
    layouts = {
        'nested': 'struct cache { struct element *heads[COUNT]; pthread_mutex_t guards[COUNT]; } c;',
        'reordered': 'struct cache { pthread_mutex_t guards[COUNT]; struct element *heads[COUNT]; } c;',
        'double': 'struct cache { long long pad; struct inner { struct element *heads[COUNT]; pthread_mutex_t guards[COUNT]; } part; } outer;',
        'packed': 'struct __attribute__((packed)) cache { char pad; struct element *heads[COUNT]; pthread_mutex_t guards[COUNT]; } c;',
        'ambiguous': 'struct cache { struct element *other[COUNT]; struct element *heads[COUNT]; pthread_mutex_t guards[COUNT]; } c;',
    }
    for name, layout in layouts.items():
        path = 'outer.part' if name == 'double' else 'c'
        text = fixture.replace(original, layout + f'\n#define heads {path}.heads\n#define guards {path}.guards')
        variants[name] = (text, name in {'nested', 'reordered', 'double'}, False if name == 'packed' else (None if name == 'ambiguous' else True))
    base = variants['nested'][0]
    for case in (2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16):
        # CASE11 may prove unrelated protected pairs; its unsafe write must
        # retain a warning. Test verdict rather than banning all owner facts.
        variants[f'case{case}'] = (f'#define CASE {case}\n' + base, case == 15,
                                  None if case in {2, 9, 11, 15} else False)
    variants['root-past-end'] = (base.replace('append(make_element(3), heads[key]);',
                                             'append(make_element(3), heads[COUNT]);'), False, False)
    variants['root-negative'] = (variants['reordered'][0].replace('append(make_element(3), heads[key]);',
                                             'append(make_element(3), heads[-1]);'), False, False)
    variants['wrong-owner'] = (base.replace('append(make_element(3), heads[key]);',
                                           'append(make_element(3), heads[(key+1)%COUNT]);'), False, None)
    variants['address-escape'] = (base.replace('int shared_index;', 'int shared_index; void *escaped;')
                                  .replace('pthread_t child;', 'escaped = &c; pthread_t child;'), False, False)
    variants['slot-address-escape'] = (base.replace('int shared_index;', 'int shared_index; void *escaped;')
                                  .replace('pthread_t child;', 'escaped = &heads[0]; pthread_t child;'), False, False)
    variants['byte-write'] = (base.replace('pthread_t child;',
                              '((unsigned char *)&heads[0])[1] = 0; pthread_t child;'), False, False)
    variants['aggregate-write'] = (base.replace('pthread_t child;',
                                   'c = (struct cache){0}; pthread_t child;'), False, False)
    variants['post-create-root-write'] = (base.replace('datum_type result =',
                                         'heads[first] = make_element(7); datum_type result ='), False, None)
    variants['scalar-sibling'] = (base.replace('struct cache {', 'struct cache { int extra;')
                                  .replace('pthread_t child;', 'c.extra = 1; pthread_t child;'), False, False)
    variants['extra-root-write'] = (variants['ambiguous'][0].replace('pthread_t child;',
                                   'c.other[0] = make_element(7); pthread_t child;'), False, None)
    variants['mutex-before-array'] = (base.replace('#define COUNT 3', '#define COUNT 10').replace('pthread_mutex_t *lock = &guards[key];',
                                      'pthread_mutex_t *lock = &guards[-1];'), False, False)
    variants['changed-root-index'] = (base.replace('append(make_element(3), heads[key]);',
                                       'key += COUNT; append(make_element(3), heads[key]);'), False, False)
    variants['volatile-slots'] = (base.replace('struct element *heads[COUNT];',
                                               'struct element *volatile heads[COUNT];'), False, False)
    variants['late-root-out-of-bounds'] = (base.replace('for (int i = 0; i < COUNT; ++i) {',
        'for (int i = 0; i <= COUNT; ++i) {'), False, False)
    variants['wide-wrap-root'] = (base.replace('append(make_element(3), heads[key]);',
        'long long big = 4294967296LL + COUNT; int wrapped = (int)big; '
        'append(make_element(3), heads[wrapped]);'), False, False)
    variants['helper-container-escape'] = (base.replace('int shared_index;',
        'int shared_index; void *escaped; void remember(void *p) { escaped = p; }')
        .replace('pthread_t child;', 'remember(&c); pthread_t child;'), False, False)
    variants['function-pointer-sibling'] = (base.replace('struct cache {',
        'void callback(void) {} struct cache { void (*callbacks[COUNT])(void);')
        .replace('pthread_t child;', 'c.callbacks[COUNT] = callback; pthread_t child;'), False, False)
    variants['union-container'] = (base.replace('struct cache {', 'union cache { struct {')
        .replace('} c;', '}; } c;'), False, False)
    variants['unobserved-leaf'] = (base.replace('void *entry(void *unused)',
        'struct element *unselected(void) { return heads[0]; } void *entry(void *unused)'), False, False)
    for key in (0, 1):
        constant = variants['reordered'][0].replace('int key = __VERIFIER_nondet_int();',
            f'int key = {key};').replace('int first = __VERIFIER_nondet_int();', f'int first = {key};')
        variants[f'constant-{key}'] = (constant, True, True)
        variants[f'constant-wrong-lock-{key}'] = (constant.replace(
            'pthread_mutex_t *lock = &guards[key];',
            'pthread_mutex_t *lock = &guards[(key+1)%COUNT];'), False, True)
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    with tempfile.TemporaryDirectory(prefix='ikos-nested-array-') as temp:
        work = Path(temp)
        for wide in (False, True):
            for name, (text, expected_safe, expected_preflight) in variants.items():
                source = work / 'renamed.c'
                source.write_text(('#define WIDE\n' if wide else '') + text)
                database = work / 'output.db'
                proc = subprocess.run([str(args.ikos.resolve()), '--analyses=race',
                    '--concurrency=auto', '--demote-race-to-unknown', '--partitioning=guards',
                    '--bmc-deadline-ms=0', '--log=debug', '-m32', '-o', str(database), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                output = proc.stdout + proc.stderr
                safe = 'The program is SAFE' in output
                preflight = 'mutex-array preflight certified' in output
                assert proc.returncode == 0 and safe == expected_safe, (name, wide, output)
                if expected_preflight is not None:
                    assert preflight == expected_preflight, (name, wide, output)
                with sqlite3.connect(database) as db:
                    proofs = [json.loads(row[0]) for row in db.execute(
                        "SELECT info FROM checks WHERE info LIKE '%indexed-live-heap-owner-v1%'")]
                if expected_safe:
                    assert 'Region ownership: complete family and all writer transfers certified' in output, (name, output)
                    # Concrete same-lock filtering can resolve constant cases
                    # before an ownership proof row is consumed.
                    assert proofs or name.startswith('constant-'), (name, wide, output)
                    assert all(p['root_array'] == p['mutex_array'] and
                        p['root_byte_begin'] != p['mutex_byte_begin'] for p in proofs), (name, proofs)
                elif name != 'case11':
                    assert not proofs, (name, wide, proofs)
                print(f'nested={name}/wide={wide}: safe={safe}, preflight={preflight}, proofs={len(proofs)}', flush=True)


if __name__ == '__main__':
    main()
