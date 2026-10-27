#!/usr/bin/env python3
"""Check explicit native/legacy pthread routes without widening proof permissions."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

from ikos_runtime import runtime_command, runtime_identity, verify_runtime

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r"""
#include <pthread.h>
#include <time.h>
int shared;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_rwlock_t rwlock = PTHREAD_RWLOCK_INITIALIZER;
void *worker(void *unused) {
#if CASE == 2 || CASE == 3 || CASE == 6
  pthread_mutex_lock(&mutex);
#elif CASE == 4
  pthread_rwlock_rdlock(&rwlock);
#elif CASE == 5
  pthread_rwlock_wrlock(&rwlock);
#endif
  shared++;
#if CASE == 2 || CASE == 3 || CASE == 6
  pthread_mutex_unlock(&mutex);
#elif CASE == 4 || CASE == 5
  pthread_rwlock_unlock(&rwlock);
#endif
  return 0;
}
int main(void) {
  pthread_t threads[1];
  pthread_create(threads, 0, worker, 0);
#if CASE == 0
  pthread_join(threads[0], 0);
#elif CASE == 2
  pthread_mutex_lock(&mutex);
#elif CASE == 3
  if (pthread_mutex_trylock(&mutex) == 0) {
    pthread_mutex_unlock(&mutex);
    return 0;
  }
#elif CASE == 4
  pthread_rwlock_rdlock(&rwlock);
#elif CASE == 5
  pthread_rwlock_wrlock(&rwlock);
#elif CASE == 6
  struct timespec deadline = {0, 0};
  if (pthread_mutex_timedlock(&mutex, &deadline) == 0) {
    pthread_mutex_unlock(&mutex);
    return 0;
  }
#endif
  shared++;
#if CASE == 2
  pthread_mutex_unlock(&mutex);
#elif CASE == 4 || CASE == 5
  pthread_rwlock_unlock(&rwlock);
#endif
  return 0;
}
"""
LEGACY = r"""
typedef void *pthread_t;
extern int pthread_create(pthread_t *, void *, void *(*)(void *), void *);
extern int pthread_join(pthread_t, void **);
int shared;
void *worker(void *p) { shared++; return 0; }
int main(void) {
  pthread_t threads[1];
  pthread_create(threads, 0, worker, 0);
  pthread_join(threads[0], 0);
  shared++;
  return 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--bits', type=int, choices=(32, 64), default=32)
    args = parser.parse_args()
    command = runtime_command(args.ikos.resolve())
    identity = runtime_identity(args.ikos.resolve())
    clang = identity['tools']['clang']['path']
    env = {k: v for k, v in os.environ.items()
           if not k.startswith('IKOS_') and k not in ('PYTHONPATH', 'PYTHONHOME')}
    checked = 0
    with tempfile.TemporaryDirectory(prefix='ikos-pthread-routing-') as directory:
        work = Path(directory)

        def check(name, body, *, no_debug=False, legacy=False, warning=False, safe=None):
            nonlocal checked
            source = work / 'input.c'
            source.write_text(body)
            if no_debug:
                llvm = work / 'input.ll'
                compile_result = subprocess.run([clang, '-S', '-emit-llvm', '-g0', '-O0',
                    '-Xclang', '-disable-O0-optnone', '-m' + str(args.bits),
                    str(source), '-o', str(llvm)], capture_output=True, text=True, timeout=30)
                assert compile_result.returncode == 0, (name, compile_result.stderr)
                source = llvm
            database, report = work / 'result.db', work / 'result.json'
            for path in (database, report):
                path.unlink(missing_ok=True)
            proc = subprocess.run(command + ['--analyses=race', '--concurrency=auto',
                '--partitioning=guards', '--demote-race-to-unknown', '--bmc-deadline-ms=0',
                '--display-ar', '--format=json', '--report-file=' + str(report),
                '-m' + str(args.bits), '-o', str(database), str(source)],
                env=env, capture_output=True, text=True, timeout=30)
            output = proc.stdout + proc.stderr
            assert proc.returncode == 0, (name, output)
            result = json.loads(report.read_text())['race_result']
            assert not result['search_performed'], (name, result)
            mismatch = 'llvm function pthread_join and ar intrinsic ar.pthread.join have a different type' in output
            assert mismatch == warning, (name, output)
            # Neither type nor source callback operands may disappear into a new create intrinsic.
            assert re.search(r'declare .*@pthread_create\(', output), (name, output)
            assert 'call @ar.pthread.create(' not in output, (name, output)
            join_name = 'ar.pthread.join' if legacy else 'pthread_join'
            if name.startswith(('native-join', 'legacy-pointer-join', 'wrong-')):
                assert re.search(r'declare .*@' + re.escape(join_name) + r'\(', output), (name, output)
            if safe is not None:
                assert result['verdict'] == ('SAFE' if safe else 'UNKNOWN'), (name, result, output)
            if name.startswith('mutex'):
                assert 'call @ar.pthread.mutex.lock(' in output and 'call @ar.pthread.mutex.unlock(' in output
            if name.startswith(('trylock', 'timedlock')):
                conditional = 'pthread_mutex_trylock' if name.startswith('trylock') else 'pthread_mutex_timedlock'
                assert 'call @' + conditional + '(' in output, (name, output)
            checked += 1
            print(f'{name}/bits={args.bits}: {result["verdict"]} coverage={result["coverage"]}', flush=True)

        for case, name, safe in [(0, 'native-join', True), (1, 'no-join', False),
                (2, 'mutex', True), (3, 'trylock-failure', False),
                (4, 'shared-read-lock-writes', False), (5, 'exclusive-write-lock', True),
                (6, 'timedlock-failure', False)]:
            body = f'#define CASE {case}\n' + SOURCE
            check(name, body, safe=safe)
            if case in (0, 1, 3, 4, 5):
                check(name + '-no-debug', body, no_debug=True, safe=safe)
        check('legacy-pointer-join', LEGACY, legacy=True)
        check('legacy-pointer-join-no-debug', LEGACY, no_debug=True, legacy=True)
        # Incompatible C runtime declarations test routing/diagnostics only.
        # They provide no defined-program verdict oracle; real races above do.
        word = 'unsigned int' if args.bits == 64 else 'unsigned long long'
        native = LEGACY.replace('typedef void *pthread_t;', 'typedef unsigned long pthread_t;')
        variants = {
            'wrong-width': native.replace('pthread_join(pthread_t,', f'pthread_join({word},'),
            'wrong-return': native.replace('extern int pthread_join', 'extern long long pthread_join'),
            'wrong-output': native.replace('pthread_join(pthread_t, void **)', 'pthread_join(pthread_t, int)')
        }
        for name, body in variants.items():
            for no_debug in (False, True):
                check(name + ('-no-debug' if no_debug else ''), body,
                      no_debug=no_debug, warning=True)
    verify_runtime(identity)
    print(f'{checked} pthread route controls passed for {args.bits}-bit')


if __name__ == '__main__':
    main()
