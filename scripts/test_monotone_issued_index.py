#!/usr/bin/env python3
"""Serialized issue uniqueness, payload provenance and family-completion controls.

Default Interval, BMC disabled, with genuine target headers for each ABI.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import signal
import subprocess
import sys
import tempfile

BASE = r"""
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
extern int __VERIFIER_nondet_int(void);
extern void opaque(void*);
void assume_abort_if_not(int b) { if (!b) abort(); }
void *safe_malloc(size_t size) {
  void *p = malloc(size);
  if (!p) abort();
  return p;
}
typedef int datum;
datum *datas;
int next_j;
pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
void *worker(void *unused) {
  pthread_mutex_lock(&gate);
  int j = next_j;
  next_j++;
  pthread_mutex_unlock(&gate);
  datas[j] = __VERIFIER_nondet_int();
  return 0;
}
int main(void) {
  int count = __VERIFIER_nondet_int();
  assume_abort_if_not(count >= 0);
  assume_abort_if_not((unsigned)count <= UINT32_MAX / sizeof(pthread_t));
  assume_abort_if_not((unsigned)count <= UINT32_MAX / sizeof(datum));
  pthread_t *handles = safe_malloc((size_t)count * sizeof(pthread_t));
  datas = safe_malloc((size_t)count * sizeof(datum));
  for (int i=0; i<count; i++)
    pthread_create(&handles[i], 0, worker, 0);
  for (int k=0; k<count; k++)
    pthread_join(handles[k], 0);
  free(handles);
  free(datas);
  return 0;
}
"""
WRITE = "datas[j] = __VERIFIER_nondet_int();"
CREATE = "pthread_create(&handles[i], 0, worker, 0);"
JOIN = "pthread_join(handles[k], 0);"

def cases():
    return [
        ("canonical", BASE, True, False),
        ("wide-payload", BASE.replace("typedef int datum", "typedef long long datum"), True, False),
        ("renamed", BASE.replace("next_j", "issued").replace("datas", "cells")
            .replace("worker", "claim_cell").replace("gate", "issuer_lock"), True, False),
        ("multiple-same-cell-accesses", BASE.replace(WRITE, WRITE + " datas[j] += 1;"), True, False),
        ("never-free", BASE.replace("  free(datas);", ""), True, False),
        ("never-free-handles", BASE.replace("  free(handles);", ""), True, False),
        ("never-free-either", BASE.replace("  free(datas);", "").replace("  free(handles);", ""), True, False),
        ("missing-lock", BASE.replace("  pthread_mutex_lock(&gate);", ""), False, True),
        ("missing-unlock", BASE.replace("  pthread_mutex_unlock(&gate);", ""), False, False),
        ("capture-before-lock", BASE.replace("  pthread_mutex_lock(&gate);\n  int j = next_j;",
            "  int j = next_j;\n  pthread_mutex_lock(&gate);"), False, True),
        ("update-after-unlock", BASE.replace("  next_j++;\n  pthread_mutex_unlock(&gate);",
            "  pthread_mutex_unlock(&gate);\n  next_j++;"), False, True),
        ("reset-after-issue", BASE.replace(WRITE, "next_j = 0; " + WRITE), False, True),
        ("decrement", BASE.replace("next_j++;", "next_j--;"), False, True),
        ("no-increment", BASE.replace("  next_j++;", ""), False, True),
        ("increment-twice", BASE.replace("next_j++;", "next_j++; next_j++;"), False, False),
        ("nonzero-initializer", BASE.replace("int next_j;", "int next_j = 1;"), False, False),
        ("counter-main-write", BASE.replace("  for (int i", "  next_j = 1;\n  for (int i"), False, False),
        ("worker-loop", BASE.replace("  pthread_mutex_lock(&gate);", "  for (int repeat=0;repeat<2;repeat++) {\n  pthread_mutex_lock(&gate);")
            .replace("  return 0;\n}", "  }\n  return 0;\n}", 1), False, False),
        ("bypass-transaction", BASE.replace("  pthread_mutex_lock(&gate);", "  int j; if (__VERIFIER_nondet_int()) goto payload;\n  pthread_mutex_lock(&gate);")
            .replace("  int j = next_j;", "  j = next_j;").replace(WRITE, "payload: " + WRITE), False, False),
        ("constant-index", BASE.replace("datas[j]", "datas[0]"), False, True),
        ("modulo-index", BASE.replace("datas[j]", "datas[j & 1]"), False, True),
        ("truncated-index", BASE.replace("datas[j]", "datas[(unsigned char)j]"), False, True),
        ("neighbor-index", BASE.replace(WRITE, WRITE + " datas[j+1]=0;"), False, True),
        ("byte-stride", BASE.replace("datas[j] =", "*(datum*)((char*)datas+j) ="), False, True),
        ("overlapping-wide-write", BASE.replace("datas[j] =", "*(long long*)(datas+j) ="), False, True),
        ("unknown-counter-effect", BASE.replace(WRITE, "opaque(&next_j); " + WRITE), False, False),
        ("counter-byte-effect", BASE.replace(WRITE, "*(char*)&next_j = 0; " + WRITE), False, False),
        ("worker-publication", BASE.replace(WRITE, "datas = safe_malloc(32); " + WRITE), False, False),
        ("second-publication", BASE.replace("  for (int i", "  datas = safe_malloc((size_t)count * sizeof(datum));\n  for (int i"), False, False),
        ("offset-publication", BASE.replace("datas = safe_malloc((size_t)count * sizeof(datum));", "datas = (datum*)safe_malloc((size_t)count * sizeof(datum)) + 1;"), False, False),
        ("signed-payload-product", BASE.replace("safe_malloc((size_t)count * sizeof(datum))",
            "safe_malloc((size_t)(count * (int)sizeof(datum)))"), False, False),
        ("payload-size-short", BASE.replace("safe_malloc((size_t)count * sizeof(datum))", "safe_malloc((size_t)count * sizeof(datum)-1)"), False, False),
        ("payload-other-N", BASE.replace("safe_malloc((size_t)count * sizeof(datum))", "safe_malloc((size_t)__VERIFIER_nondet_int() * sizeof(datum))"), False, False),
        ("unchecked-malloc", BASE.replace("  if (!p) abort();", ""), False, False),
        ("inverted-nonnull", BASE.replace("if (!p)", "if (p)"), False, False),
        ("payload-early-free", BASE.replace("  free(datas);", "").replace("  for (int k", "  free(datas);\n  for (int k"), False, False),
        ("payload-double-free", BASE.replace("  free(datas);", "  free(datas); free(datas);"), False, False),
        ("handle-early-free", BASE.replace("  free(handles);", "").replace("  for (int k", "  free(handles);\n  for (int k"), False, False),
        ("handle-is-payload", BASE.replace("  for (int i", "  handles = (pthread_t*)datas;\n  for (int i"), False, False),
        ("handle-output-counter", BASE.replace("&handles[i]", "(pthread_t*)&next_j"), False, False),
        ("short-join", BASE.replace("k<count;", "k<count-1;"), False, False),
        ("missing-join", BASE.replace("  for (int k=0; k<count; k++)\n    " + JOIN, ""), False, False),
        ("second-create", BASE.replace(CREATE, "{ " + CREATE + " " + CREATE + " }"), False, False),
        ("ordinary-worker-call", BASE.replace("  free(handles);", "  worker(0);\n  free(handles);"), False, False),
        ("preprocessor-inlined-allocation-helper", BASE.replace("void *safe_malloc", "__attribute__((noinline)) void *safe_malloc"), True, False),
        ("wide-late-only-bound", BASE.replace("typedef int datum", "typedef long long datum")
            .replace("  assume_abort_if_not((unsigned)count <= UINT32_MAX / sizeof(datum));\n", "")
            .replace("  datas =", "  assume_abort_if_not((unsigned)count <= UINT32_MAX / sizeof(datum));\n  datas ="), False, False),
    ]

def run_analyzer(command, env, timeout):
    process = subprocess.Popen(command, env=env, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True, start_new_session=True)
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except BaseException:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.communicate()
        raise
    return subprocess.CompletedProcess(command, process.returncode, stdout, stderr)



def modular_oracle():
    """Independent finite arithmetic check; never interprets LLVM poison."""
    checks = 0
    for bits in range(4, 9):
        modulus = 1 << bits
        for stride in (1, 2, 4, 8):
            if stride >= modulus:
                continue
            for count in range(1, (modulus - 1) // stride + 1):
                for base in (0, modulus // 2 - 1, modulus - 1):
                    cells = [{(base + stride * j + byte) % modulus
                              for byte in range(stride)} for j in range(count)]
                    assert len(set().union(*cells)) == count * stride
                    checks += 1
            # Crossing signed scale alone preserves separation.
            count = modulus // (2 * stride) + 2
            if count * stride < modulus:
                cells = [{(stride * j + byte) % modulus
                          for byte in range(stride)} for j in range(count)]
                assert len(set().union(*cells)) == count * stride
                checks += 1
            # Relaxing the modulus bound really permits duplicate cells.
            count = modulus // stride + 1
            assert 0 == (stride * (count - 1)) % modulus
            checks += 1
            # Width > stride really permits adjacent byte overlap.
            assert stride in set(range(stride + 1)) & set(range(stride, 2 * stride + 1))
            checks += 1
    for bits in (32, 64):
        modulus = 1 << bits
        for stride in (4, 8):
            threshold = modulus // (2 * stride)
            # Integer differences are nonzero yet stay strictly inside modulus.
            pairs = [(threshold - 1, threshold), (0, threshold + 1)]
            for left, right in pairs:
                for u in range(stride):
                    for v in range(stride):
                        delta = stride * (left - right) + u - v
                        assert delta and abs(delta) < modulus and delta % modulus
                        checks += 1
            assert (stride * (modulus // stride)) % modulus == 0
            checks += 1
    # A pointer-width theorem is invalid when LLVM uses a narrower GEP index.
    assert (4 * (1 << 30)) % (1 << 32) == 0
    assert 4 * ((1 << 30) + 1) < (1 << 64)
    checks += 1
    return checks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path)
    parser.add_argument('--bits', choices=('32', '64'), default='32')
    parser.add_argument('--original', type=Path)
    parser.add_argument('--case', help='Run one named case')
    args = parser.parse_args()
    print('independent modular checks:', modular_oracle(), flush=True)
    repo = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(repo/'scripts'))
    from ikos_runtime import runtime_command, runtime_identity, verify_runtime
    ikos = args.ikos or repo/'install/bin/ikos'
    identity = runtime_identity(ikos)
    command = runtime_command(ikos)
    selected = cases()
    if args.original:
        assert args.bits == '32'
        assert hashlib.sha256(args.original.read_bytes()).hexdigest() == '110c82ddba409eb37ace8c4a22c3cdef948429a0b6a515d67684ad9fe87b118d'
        selected.insert(0, ('original959', args.original.read_text(), True, False))
    if args.case:
        selected = [row for row in selected if row[0] == args.case]
        assert selected, args.case
    base_env = {k:v for k,v in os.environ.items() if not k.startswith('IKOS_') and k not in ('PYTHONPATH', 'PYTHONHOME')}
    with tempfile.TemporaryDirectory(prefix='ikos-issued-index-') as temp:
        directory = Path(temp)
        for name, text, positive, unsafe in selected:
            if args.bits == '64' and name == 'wide-late-only-bound':
                positive = True  # signed32 count times eight cannot wrap LP64
            controls = ('normal', 'issued-off', 'producer-off', 'consumer-off') if name in ('canonical','original959') else ('normal',)
            for mode in controls:
                env = dict(base_env)
                if mode == 'issued-off': env['IKOS_DISABLE_MONOTONE_ISSUED_INDEX'] = '1'
                elif mode != 'normal': env['IKOS_RACE_DISABLE_INDEXED_JOIN_' + mode.split('-')[0].upper()] = '1'
                src = directory/('input.i' if name == 'original959' else 'input.c')
                src.write_text(text)
                dbfile = directory/(name+'-'+mode+'.db')
                result = run_analyzer([*command, '--analyses=race', '--concurrency=auto', '--partitioning=guards',
                    '--demote-race-to-unknown', '--bmc-deadline-ms=0', '-m'+args.bits,
                    '-o', str(dbfile), str(src)], env, 45)
                output = result.stdout + result.stderr
                assert result.returncode == 0, (name, mode, output)
                with sqlite3.connect(dbfile) as db:
                    proofs = [json.loads(x[0]) for x in db.execute('SELECT info FROM checks')
                        if json.loads(x[0]).get('issued_index_contract') == 'closed-monotone-issued-index-v1']
                admitted = positive and mode == 'normal'
                assert bool(proofs) == admitted, (name, args.bits, mode, proofs, output)
                safe = 'The program is SAFE' in output
                if admitted: assert safe, (name, mode, output)
                if unsafe or mode != 'normal': assert not safe, (name, mode, output)
                print(f'{name}/{args.bits}/{mode}: proofs={len(proofs)}, SAFE={safe}', flush=True)
    verify_runtime(identity)

if __name__ == '__main__':
    main()
