#!/usr/bin/env python3
"""Check native CAS value paths without claiming an exact atomic execution model."""
import argparse
import json
import os
from pathlib import Path
import signal
import sqlite3
import subprocess
import tempfile
from ikos_runtime import runtime_command, runtime_identity, verify_runtime

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--bits', type=int, choices=(32, 64), default=32)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/cmpxchg-private-values.c').read_text()
    env = {k: v for k, v in os.environ.items() if not k.startswith('IKOS_')}
    before = runtime_identity(args.ikos.resolve())
    rows = []
    with tempfile.TemporaryDirectory(prefix='ikos-cmpxchg-') as temp:
        work = Path(temp)
        def run(name, source, safe, opt='none'):
            path = work / (name + '.c')
            path.write_text(source)
            database = work / (name + '.db')
            command = runtime_command(args.ikos.resolve()) + ['--analyses=race', '--concurrency=auto',
                '--partitioning=guards', '--demote-race-to-unknown', '--bmc-deadline-ms=0',
                '-m' + str(args.bits), '-o', str(database)]
            if opt == 'none': command.append('--opt=none')
            proc = subprocess.Popen(command + [str(path)], env=env, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, text=True, start_new_session=True)
            try:
                stdout, stderr = proc.communicate(timeout=45)
            except BaseException:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.communicate()
                raise
            output = stdout + stderr
            assert proc.returncode == 0, (name, output)
            with sqlite3.connect(database) as db:
                result = json.loads(db.execute("SELECT value FROM settings WHERE name='race_result'").fetchone()[0])
            assert result['analysis_complete'] and result['checks_complete'], (name, result)
            if safe is not None:
                assert (result['verdict'] == 'SAFE') == safe, (name, result, output)
            else:
                assert result['verdict'] in ('SAFE', 'UNKNOWN'), (name, result, output)
            if safe is False:
                assert result['verdict'] == 'UNKNOWN' and result['remaining_pairs'] > 0, (name, result)
            rows.append(dict(name=name, bits=args.bits, opt=opt, result=result))
            print(f'cmpxchg {name}/{args.bits}: {result["verdict"]}', flush=True)
        for width in (0, 1):
            for case in range(7):
                source = f'#define CASE {case}\n#define CELL64 {width}\n' + fixture
                # Aggregate flag precision can be conservative; the failure value
                # and all reachable conflicts must still be preserved.
                run(f'case{case}-wide{width}', source, True if case == 2 else (None if case in (3, 5) else False))
        for case in (0, 1, 4):
            run(f'default-case{case}', f'#define CASE {case}\n' + fixture, False, 'default')
        pointer = """#include <pthread.h>
#include <stdatomic.h>
int data;
static void *worker(void *arg) {
  (void)arg; int a,b; _Atomic(int*) p=&a;
  int *expected=EXPECTED;
  _Bool ok=atomic_compare_exchange_STRONG_explicit(&p,&expected,&b,memory_order_seq_cst,memory_order_seq_cst);
  if (CONDITION) data++;
  return 0;
}
int main(void) { pthread_t a,b; pthread_create(&a,0,worker,0);pthread_create(&b,0,worker,0);return 0; }
"""
        for name, kind, expected, condition, safe in [
            ('pointer-fail', 'strong', '&b', '!ok && atomic_load(&p)==&a', False),
            ('pointer-success', 'strong', '&a', 'ok && atomic_load(&p)==&b', False),
            ('pointer-weak-fail', 'weak', '&a', '!ok && atomic_load(&p)==&a', False),
            ('pointer-impossible', 'strong', '&b', 'ok', None),
        ]:
            run(name, pointer.replace('STRONG', kind).replace('EXPECTED', expected).replace('CONDITION', condition), safe)
        # Exercise repeated output splitting in one LLVM block, and pointer NULL.
        sequential = """#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
int data;
static void *worker(void *arg) {
  (void)arg; _Atomic(uint64_t) x=1; uint64_t expected=0;
  atomic_compare_exchange_strong(&x,&expected,2);
  expected=1;
  atomic_compare_exchange_strong(&x,&expected,3);
  if (atomic_load(&x)==3) data++;
  return 0;
}
int main(void) { pthread_t a,b; pthread_create(&a,0,worker,0);pthread_create(&b,0,worker,0);return 0; }
"""
        run('sequential-fail-then-success', sequential, False)
        run('desired-equals-old', sequential.replace('expected=0;', 'expected=1;').replace('expected,2)', 'expected,1)'), False)
        run('pointer-null-failure', pointer.replace('STRONG', 'strong').replace('EXPECTED', '0').replace('CONDITION', '!ok && atomic_load(&p)==&a'), False)
    verify_runtime(before)
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(dict(passed=True, runtime=before, runtime_unchanged=True,
                                               analyses=len(rows), rows=rows), indent=2) + '\n')

if __name__ == '__main__':
    main()
