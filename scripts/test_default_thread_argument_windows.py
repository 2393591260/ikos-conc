#!/usr/bin/env python3
"""Default-interval source argument windows: dedicated dual-ABI controls.

Draft only: no execution has been performed. The default domain is intentionally
not selected with --domain, and every database must confirm interval+BMC0.
"""
import argparse
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile
from ikos_runtime import runtime_command

PREFIX = r"""#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
extern int __VERIFIER_nondet_int(void);
void assume_abort_if_not(int b) { if (!b) abort(); }
void *safe_malloc(size_t size) { void *p=malloc(size); if (!p) abort(); return p; }
"""
MAIN = r"""int main(void) {
  int count=__VERIFIER_nondet_int();
  assume_abort_if_not(count>=0);
  assume_abort_if_not((unsigned)count<=UINT32_MAX/sizeof(pthread_t));
  pthread_t *ids=safe_malloc((size_t)count*sizeof(pthread_t));
  assume_abort_if_not((unsigned)count<=UINT32_MAX/sizeof(item));
  item *data=safe_malloc((size_t)count*sizeof(item));
  for (int position=0; position<count; position++) {
    pthread_create(&ids[position],0,worker,&data[position]);
  }
  return 0;
}
"""
DIRECT = 'typedef int item;\nvoid *worker(void *argument) { *(item*)argument=1; return 0; }\n'
CASES = [
    ('renamed-direct', DIRECT, MAIN, True),
    # 961's imported AR is a multi-term shift 4*0+1*0, not a single term.
    ('renamed-struct-zero', 'typedef struct { int data; } item;\n'
     'void *worker(void *argument) { ((item*)argument)->data=1; return 0; }\n', MAIN, True),
    ('byte-stride', 'typedef unsigned char item;\n'
     'void *worker(void *argument) { *(item*)argument=1; return 0; }\n', MAIN, True),
    ('short-stride', 'typedef unsigned short item;\n'
     'void *worker(void *argument) { *(item*)argument=1; return 0; }\n', MAIN, True),
    # ILP32 still exercises an actual 64-bit datum and width=8.
    ('long-long-stride', 'typedef long long item;\n'
     'void *worker(void *argument) { *(item*)argument=1; return 0; }\n', MAIN, True),
    ('nonzero-field', 'typedef struct { int left; int right; } item;\n'
     'void *worker(void *argument) { ((item*)argument)->right=1; return 0; }\n', MAIN, True),
    ('wide-field', 'typedef struct { long long left; long long right; } item;\n'
     'void *worker(void *argument) { ((item*)argument)->right=1; return 0; }\n', MAIN, True),
    ('wide-counter-value-preserving-trunc', DIRECT,
     MAIN.replace('int position=0', 'long long position=0')
         .replace('&data[position]', '&data[(int)position]'), True),
    ('unsigned-counter', DIRECT, MAIN.replace('int position=0', 'unsigned position=0'), True),
    ('duplicate-argument', DIRECT, MAIN.replace('&data[position]', '&data[position/2]'), False),
    ('constant-argument', DIRECT, MAIN.replace('&data[position]', 'data'), False),
    ('byte-overlap-wide-store', 'typedef int item;\n'
     'void *worker(void *argument) { *(long long*)argument=1; return 0; }\n', MAIN, False),
    ('cross-endpoints', 'typedef int item;\n'
     'void *worker(void *argument) { ((item*)argument)[0]=1; ((item*)argument)[1]=1; return 0; }\n', MAIN, False),
    ('second-create', DIRECT, MAIN.replace('pthread_create(&ids[position],0,worker,&data[position]);',
     'pthread_create(&ids[position],0,worker,&data[position]);\n'
     'pthread_create(&ids[position],0,worker,&data[position]);'), False),
    ('counter-retry', DIRECT, MAIN.replace('pthread_create(&ids[position],0,worker,&data[position]);',
     'pthread_create(&ids[position],0,worker,&data[position]);\n'
     'if (__VERIFIER_nondet_int()) position--;'), False),
    ('family-reentry', DIRECT, MAIN.replace('  for (int position=0;',
     '  for (int round=0; round<2; round++) {\n  for (int position=0;')
     .replace('  return 0;', '  }\n  return 0;'), False),
    ('ordinary-worker-activation', DIRECT, MAIN.replace('  return 0;',
     '  worker(data);\n  return 0;'), False),
    ('unknown-worker-origin', 'typedef int item;\nextern item *unknown_pointer(void);\n'
     'void *worker(void *argument) { *unknown_pointer()=1; return 0; }\n', MAIN, False),
    ('helper-origin', 'typedef int item;\n'
     '__attribute__((noinline)) item *pick(item *p) { if (__VERIFIER_nondet_int()) return p; return p+1; }\n'
     'void *worker(void *argument) { *pick(argument)=1; return 0; }\n', MAIN, False),
    ('merged-origin', 'typedef int item;\n'
     'void *worker(void *argument) { item *p=argument; if (__VERIFIER_nondet_int()) p++; *p=1; return 0; }\n', MAIN, False),
    ('narrowing-argument-index', DIRECT, MAIN.replace('&data[position]', '&data[(unsigned char)position]'), False),
    ('induction-wrap', DIRECT, MAIN.replace('int position=0', 'unsigned char position=0'), False),
    ('mutable-base', DIRECT, MAIN.replace('    pthread_create(',
     '    if (__VERIFIER_nondet_int()) data++;\n    pthread_create('), False),
    ('defined-fake-create', DIRECT + r"""
int pthread_create(pthread_t *a,const pthread_attr_t *b,void *(*c)(void*),void *d) { return 0; }
""", MAIN, False),

    ('main-worker-conflict', DIRECT,
     MAIN.replace('  return 0;', '  data[0]=2;\n  return 0;'), False),
    # Actual native index contains the high bit. A source cast/type spelling
    # cannot silently make that value a nonnegative signed GEP index.
    ('native-gep-high-bit', 'typedef unsigned char item;\nvoid *worker(void *argument) { *(item*)argument=1; return 0; }\n', r"""int main(void) {
  uintptr_t count=(uint32_t)__VERIFIER_nondet_int();
#if UINTPTR_MAX == UINT64_MAX
  count=(count<<32)|(uint32_t)__VERIFIER_nondet_int();
#endif
  pthread_t slot;
  item *data=safe_malloc(sizeof(item));
  for (uintptr_t position=0; position<count; position++) {
    pthread_create(&slot,0,worker,&data[position]);
  }
  return 0;
}
""", False),
    # Keep a valid native-stride family, then cross the offset modulus only
    # at the worker byte endpoint. LP64 needs a large *type* stride to reach
    # that boundary with the same signed-int induction family; no huge object
    # is allocated or concretely executed by this static-analysis test runner.
    ('large-residual-byte-end', r"""
#if UINTPTR_MAX == UINT64_MAX
  typedef struct { unsigned char bytes[UINT64_C(1)<<33]; } item;
#else
  typedef struct { unsigned char bytes[8]; } item;
#endif
void *worker(void *argument) {
  ((unsigned char*)argument)[INTPTR_MAX]=1;
  return 0;
}
""", MAIN.replace('UINT32_MAX', 'UINTPTR_MAX'), False),
]

def analyze(args, bits, name, source, mode, directory, env):
    source_path = directory / 'renamed.c'
    source_path.write_text(PREFIX + source)
    database = directory / 'result.db'
    database.unlink(missing_ok=True)
    run_env = dict(env)
    trace = directory / 'rules.jsonl'
    trace.unlink(missing_ok=True)
    run_env['IKOS_RACE_RULE_REPORT'] = str(trace)
    if mode == 'producer-off':
        run_env['IKOS_RACE_DISABLE_THREAD_ARGUMENT_PRODUCER'] = '1'
    elif mode == 'consumer-off':
        run_env['IKOS_RACE_DISABLE_THREAD_ARGUMENT_CONSUMER'] = '1'
    proc = subprocess.run(
        [*runtime_command(args.ikos), '--analyses=race', '--concurrency=auto',
         '--demote-race-to-unknown', '--bmc-deadline-ms=0', '--partitioning=guards',
         '-m' + str(bits),
         '-o', str(database), str(source_path)],
        env=run_env, capture_output=True, text=True, timeout=40)
    assert proc.returncode == 0, (name, bits, mode, proc.stdout, proc.stderr)
    with sqlite3.connect(database) as db:
        settings = dict(db.execute('SELECT name,value FROM settings'))
        assert settings['machine-int-domain'] == 'interval', (name, bits, settings)
        assert json.loads(settings['use-partitioning-domain']) is True, (name, bits, settings)
        assert json.loads(settings['auto-boolean-partitioning']) is True, (name, bits, settings)
        infos = [json.loads(row[0]) for row in db.execute(
            'SELECT info FROM checks WHERE info IS NOT NULL')]
        result = json.loads(settings['race_result'])
    proofs = [p for p in infos if p.get('thread_argument_contract') ==
              'creation-family-source-affine-window-v1']
    assert not [p for p in infos if p.get('thread_argument_contract') ==
                'creation-family-native-window-v1'], (name, bits, mode, infos)
    if name in ('large-residual-byte-end', 'native-gep-high-bit'):
        summaries = [json.loads(line) for line in trace.read_text().splitlines()
                     if json.loads(line).get('kind') == 'pair-rule-summary']
        stats = summaries[-1]['thread_argument_windows'] if summaries else {}
        assert stats.get('candidate_family') and stats.get('native_creator_queries', 0) > 0, (name, bits, result, stats)
        assert stats.get('pair_queries', 0) > 0, (name, bits, result, stats)
        assert result['coverage'] == 'Checked' and result['remaining_pairs'] > 0, (name, bits, result)
    if name == 'main-worker-conflict':
        assert proofs and result['coverage'] == 'Checked' and result['remaining_pairs'] > 0, (name, bits, result, proofs)
    return result, proofs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, required=True)
    parser.add_argument('--bits', type=int, choices=(32, 64))
    parser.add_argument('--case', action='append', dest='cases', metavar='NAME',
                        help='Run only this case; repeat to select several cases.')
    args = parser.parse_args()
    unknown = set(args.cases or []) - {case[0] for case in CASES}
    if unknown:
        parser.error('unknown case(s): ' + ', '.join(sorted(unknown)))
    env = {k: v for k, v in os.environ.items()
           if not k.startswith('IKOS_') and k not in ('PYTHONPATH', 'PYTHONHOME')}
    checks = 0
    with tempfile.TemporaryDirectory(prefix='ikos default argument windows ') as temp:
        work = Path(temp)
        for bits in ([args.bits] if args.bits else [32, 64]):
            for name, body, main_source, positive in CASES:
                if args.cases and name not in args.cases:
                    continue
                for mode in (['enabled', 'producer-off', 'consumer-off'] if positive else ['enabled']):
                    result, proofs = analyze(args, bits, name, body + main_source, mode, work, env)
                    if positive and mode == 'enabled':
                        assert result['coverage'] == 'Checked' and result['verdict'] == 'SAFE', (name, bits, result)
                        assert proofs, (name, bits, result)
                        assert all(int(p['a_width']) > 0 and int(p['b_width']) > 0 for p in proofs)
                    elif positive:
                        assert result['verdict'] == 'UNKNOWN' and not proofs, (name, bits, mode, result)
                        assert result['coverage'] == 'Checked' and result['remaining_pairs'] > 0, (name, bits, mode, result)
                    else:
                        # Some pairs can be separated even when another overlaps.
                        # Defined fake APIs may be safe under ordinary semantics;
                        # the rule still must not grant an authenticated family.
                        if name not in ('cross-endpoints', 'main-worker-conflict'):
                            assert not proofs, (name, bits, result, proofs)
                        if name not in ('defined-fake-create',):
                            assert result['verdict'] != 'SAFE', (name, bits, result)
                    checks += 1
                    print(name, bits, mode, result['verdict'], len(proofs), flush=True)
    print('PASS', checks, 'default interval source-window controls')

if __name__ == '__main__':
    main()
