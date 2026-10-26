#!/usr/bin/env python3
"""Native thread-argument windows: dual-ABI positives and broken premises."""
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
CASES = [
    ('direct', r"""typedef int item;

void *worker(void *argument) { *(item*)argument=1; return 0; }
""", None, True),
    ('helper', r"""typedef int item;
__attribute__((noinline)) void set(item *p) { *p=1; }
void *worker(void *argument) { set(argument); return 0; }
""", None, True),
    ('padded-field', r"""typedef struct { char tag; long long value; } item;

void *worker(void *argument) { ((item*)argument)->value=1; return 0; }
""", None, True),
    ('field-offset', r"""typedef struct { int left; int right; } item;

void *worker(void *argument) { ((item*)argument)->right=1; return 0; }
""", None, True),
    ('branch-window', r"""typedef struct { int values[4]; } item;

void *worker(void *argument) { if (__VERIFIER_nondet_int()) ((item*)argument)->values[0]=1; else ((item*)argument)->values[3]=1; return 0; }
""", None, True),
    ('duplicate-argument', r"""typedef int item;

void *worker(void *argument) { *(item*)argument=1; return 0; }
""", r"""int main(void) {
  int count=__VERIFIER_nondet_int();
  assume_abort_if_not(count>=0);
  assume_abort_if_not((unsigned)count<=UINT32_MAX/sizeof(pthread_t));
  pthread_t *ids=safe_malloc((size_t)count*sizeof(pthread_t));
  assume_abort_if_not((unsigned)count<=UINT32_MAX/sizeof(item));
  item *data=safe_malloc((size_t)count*sizeof(item));
  
  for (int position=0; position<count; position++) {
    pthread_create(&ids[position],0,worker,&data[position/2]);
    
  }
  
  return 0;
}
""", False),
    ('cross-pair', r"""typedef int item;

void *worker(void *argument) { ((item*)argument)[0]=1; ((item*)argument)[1]=1; return 0; }
""", None, False),
    ('wide-write', r"""typedef int item;

void *worker(void *argument) { *(long long*)argument=1; return 0; }
""", None, False),
    ('extra-create', r"""typedef int item;

void *worker(void *argument) { *(item*)argument=1; return 0; }
""", r"""int main(void) {
  int count=__VERIFIER_nondet_int();
  assume_abort_if_not(count>=0);
  assume_abort_if_not((unsigned)count<=UINT32_MAX/sizeof(pthread_t));
  pthread_t *ids=safe_malloc((size_t)count*sizeof(pthread_t));
  assume_abort_if_not((unsigned)count<=UINT32_MAX/sizeof(item));
  item *data=safe_malloc((size_t)count*sizeof(item));
  
  for (int position=0; position<count; position++) {
    pthread_create(&ids[position],0,worker,&data[position]);
    pthread_create(&ids[position],0,worker,&data[position]);
  }
  
  return 0;
}
""", False),
    ('reset', r"""typedef int item;

void *worker(void *argument) { *(item*)argument=1; return 0; }
""", r"""int main(void) {
  int count=__VERIFIER_nondet_int();
  assume_abort_if_not(count>=0);
  assume_abort_if_not((unsigned)count<=UINT32_MAX/sizeof(pthread_t));
  pthread_t *ids=safe_malloc((size_t)count*sizeof(pthread_t));
  assume_abort_if_not((unsigned)count<=UINT32_MAX/sizeof(item));
  item *data=safe_malloc((size_t)count*sizeof(item));
  for (int outer=0; outer<2; outer++) {
  for (int position=0; position<count; position++) {
    pthread_create(&ids[position],0,worker,&data[position]);
    
  }
  }
  return 0;
}
""", False),
    ('retry', r"""typedef int item;

void *worker(void *argument) { *(item*)argument=1; return 0; }
""", r"""int main(void) {
  int count=__VERIFIER_nondet_int();
  assume_abort_if_not(count>=0);
  assume_abort_if_not((unsigned)count<=UINT32_MAX/sizeof(pthread_t));
  pthread_t *ids=safe_malloc((size_t)count*sizeof(pthread_t));
  assume_abort_if_not((unsigned)count<=UINT32_MAX/sizeof(item));
  item *data=safe_malloc((size_t)count*sizeof(item));
  
  for (int position=0; position<count; position++) {
    pthread_create(&ids[position],0,worker,&data[position]);
    if (__VERIFIER_nondet_int()) position--;
  }
  
  return 0;
}
""", False),
    ('shared-rebind', r"""typedef int item; item *shared;

void *worker(void *argument) { shared=argument; *shared=1; return 0; }
""", None, False),
    ('unknown-effect', r"""typedef int item;
extern void unknown(item*);
void *worker(void *argument) { unknown(argument); *(item*)argument=1; return 0; }
""", None, False),
    ('narrow-wrap', r"""typedef int item;

void *worker(void *argument) { *(item*)argument=1; return 0; }
""", r"""int main(void) {
  int count=__VERIFIER_nondet_int();
  assume_abort_if_not(count>=0);
  assume_abort_if_not((unsigned)count<=UINT32_MAX/sizeof(pthread_t));
  pthread_t *ids=safe_malloc((size_t)count*sizeof(pthread_t));
  assume_abort_if_not((unsigned)count<=UINT32_MAX/sizeof(item));
  item *data=safe_malloc((size_t)count*sizeof(item));
  
  for (unsigned char position=0; position<count; position++) {
    pthread_create(&ids[position],0,worker,&data[position]);
    
  }
  
  return 0;
}
""", False),
    ('cancel-stride', r"""typedef int item;

void *worker(void *argument) { *(item*)argument=1; return 0; }
""", r"""int main(void) {
  int count=__VERIFIER_nondet_int();
  assume_abort_if_not(count>=0);
  assume_abort_if_not((unsigned)count<=UINT32_MAX/sizeof(pthread_t));
  pthread_t *ids=safe_malloc((size_t)count*sizeof(pthread_t));
  assume_abort_if_not((unsigned)count<=UINT32_MAX/sizeof(item));
  item *data=safe_malloc((size_t)count*sizeof(item));
  
  for (int position=0; position<count; position++) {
    pthread_create(&ids[position],0,worker,(char*)&data[position]-sizeof(item)*position);
    
  }
  
  return 0;
}
""", False),
    ('numeric-argument', r"""typedef int item;

void *worker(void *argument) { *(item*)argument=1; return 0; }
""", r"""int main(void) {
  int count=__VERIFIER_nondet_int();
  assume_abort_if_not(count>=0);
  assume_abort_if_not((unsigned)count<=UINT32_MAX/sizeof(pthread_t));
  pthread_t *ids=safe_malloc((size_t)count*sizeof(pthread_t));
  assume_abort_if_not((unsigned)count<=UINT32_MAX/sizeof(item));
  item *data=safe_malloc((size_t)count*sizeof(item));
  
  for (int position=0; position<count; position++) {
    pthread_create(&ids[position],0,worker,(void*)(uintptr_t)position);
    
  }
  
  return 0;
}
""", False),
    ('worker-rounddown', r"""typedef int item;

void *worker(void *argument) { *(item*)((uintptr_t)argument & ~(uintptr_t)7)=1; return 0; }
""", None, False),
    ('shared-load', r"""typedef int item; item *volatile shared;

void *worker(void *argument) { shared=argument; *shared=1; return 0; }
""", None, False),
]

CASES += [
    ("helper-return-window", r"""typedef struct { int values[4]; } item;
__attribute__((noinline)) int *choose(item *p, int last) {
  if (last) return &p->values[3];
  return &p->values[0];
}
void *worker(void *argument) {
  *choose(argument, __VERIFIER_nondet_int())=1; return 0;
}
""", None, True),
    ("worker-loop-window", r"""typedef struct { int values[4]; } item;
void *worker(void *argument) {
  for (int i=0; i<4; ++i) ((item*)argument)->values[i]=1;
  return 0;
}
""", None, True),
]

def native_configuration(db):
    names = ("machine-int-domain", "use-partitioning-domain",
             "auto-boolean-partitioning", "use-checks")
    values = {name: db.execute(
        "SELECT value FROM settings WHERE name=?", (name,)).fetchone()[0]
              for name in names}
    return {name: value if name == "machine-int-domain" else json.loads(value)
            for name, value in values.items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ikos", type=Path, required=True)
    parser.add_argument("--bits", type=int, choices=(32, 64))
    args = parser.parse_args()
    env = {k: v for k, v in os.environ.items()
           if not k.startswith("IKOS_") and k not in ("PYTHONPATH", "PYTHONHOME")}
    checks = 0
    with tempfile.TemporaryDirectory(prefix="ikos argument windows ") as temp:
        work = Path(temp)
        for bits in ([args.bits] if args.bits else [32, 64]):
            for name, body, alternate_main, positive in CASES:
                source = work / "renamed.c"
                source.write_text(PREFIX + body + (alternate_main or MAIN))
                for mode in (["enabled", "consumer-off", "producer-off"] if positive else ["enabled"]):
                    enabled = mode == "enabled"
                    run_env = dict(env)
                    if mode == "consumer-off":
                        run_env["IKOS_RACE_DISABLE_THREAD_ARGUMENT_CONSUMER"] = "1"
                    elif mode == "producer-off":
                        run_env["IKOS_RACE_DISABLE_THREAD_ARGUMENT_PRODUCER"] = "1"
                    trace = work / "rules.jsonl"
                    trace.unlink(missing_ok=True)
                    run_env["IKOS_RACE_RULE_REPORT"] = str(trace)
                    proc = subprocess.run(
                        [*runtime_command(args.ikos), "--analyses=race", "--concurrency=auto",
                         "--demote-race-to-unknown", "--bmc-deadline-ms=0",
                         "--partitioning=guards", "--domain=var-pack-apron-polka-polyhedra",
                         "-m" + str(bits), "-o", str(work / "output.db"), str(source)],
                        env=run_env, capture_output=True, text=True, timeout=30)
                    output = proc.stdout + proc.stderr
                    assert proc.returncode == 0, (name, bits, output)
                    with sqlite3.connect(work / "output.db") as db:
                        configuration = native_configuration(db)
                        assert configuration == {
                            "machine-int-domain": "var-pack-apron-polka-polyhedra",
                            "use-partitioning-domain": True,
                            "auto-boolean-partitioning": True,
                            "use-checks": True}, (name, bits, configuration)
                        infos = [json.loads(row[0]) for row in db.execute(
                            "SELECT info FROM checks WHERE info IS NOT NULL")]
                        result = json.loads(db.execute(
                            "SELECT value FROM settings WHERE name='race_result'").fetchone()[0])
                    proofs = [info for info in infos if info.get("thread_argument_contract")]
                    summaries = [json.loads(line) for line in
                                 (trace.read_text().splitlines() if trace.exists() else [])
                                 if json.loads(line).get("kind") == "pair-rule-summary"]
                    if not summaries:
                        # Coverage can block before final-pair enumeration.
                        assert not positive and result["coverage"] == "Blocked" and result["unresolved_count"] > 0, (
                            name, bits, mode, result, "missing rule summary")
                    stats = summaries[-1]["thread_argument_windows"] if summaries else {}
                    if positive and mode != "producer-off":
                        assert stats["candidate_family"] and stats["native_creator_queries"] > 0 and stats["native_window_queries"] > 0, (name, bits, mode, stats)
                    if mode == "producer-off":
                        assert not stats, (name, bits, mode, stats)
                    assert (result["verdict"] == "SAFE") == (positive and enabled), (
                        name, bits, enabled, result, output)
                    if positive:
                        assert bool(proofs) == enabled, (name, bits, enabled, proofs)
                    elif name == "cross-pair":
                        assert proofs and result["remaining_pairs"] > 0, (name, bits, result)
                    elif name == "shared-rebind":
                        # LLVM forwards this load to the local argument; shared writes remain.
                        assert result["remaining_pairs"] > 0, (name, bits, result)
                    else:
                        assert not proofs, (name, bits, proofs)
                    checks += 1
                    print(name, bits, mode, result["verdict"], len(proofs), stats, flush=True)
        for bits in ([args.bits] if args.bits else [32, 64]):
            source = work / "unrelated-mode.c"
            source.write_text(PREFIX + CASES[0][1] + MAIN)
            for mode in ("default-domain", "non-race", "checks-off",
                         "no-partitioning", "return-partitioning"):
                partitioning = {"no-partitioning": "no",
                                "return-partitioning": "return"}.get(mode, "guards")
                options = ["--concurrency=auto", "--partitioning=" + partitioning,
                           "--bmc-deadline-ms=0"]
                if mode == "default-domain":
                    options += ["--analyses=race", "--demote-race-to-unknown"]
                else:
                    options += ["--domain=var-pack-apron-polka-polyhedra"]
                    options += ["--analyses=dbz"] if mode == "non-race" else [
                        "--analyses=race", "--demote-race-to-unknown"]
                    if mode == "checks-off":
                        options += ["--no-checks"]
                database = work / ("mode-" + mode + ".db")
                proc = subprocess.run(
                    [*runtime_command(args.ikos), *options, "-m" + str(bits),
                     "-o", str(database), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                assert proc.returncode == 0, (mode, bits, proc.stdout, proc.stderr)
                with sqlite3.connect(database) as db:
                    configuration = native_configuration(db)
                    assert configuration["use-partitioning-domain"] == (partitioning != "no"), (mode, bits, configuration)
                    assert configuration["auto-boolean-partitioning"] == (partitioning == "guards"), (mode, bits, configuration)
                    assert configuration["use-checks"] == (mode != "checks-off"), (mode, bits, configuration)
                    if mode != "default-domain":
                        assert configuration["machine-int-domain"] == "var-pack-apron-polka-polyhedra", (mode, bits, configuration)
                    prepared = db.execute(
                        "SELECT count(*) FROM times WHERE pass=?",
                        ("ikos-analyzer.prepare.thread-argument-windows",)).fetchone()[0]
                assert prepared == 0, (mode, bits, "unexpected native-window helper")
                checks += 1
                print(mode, bits, "no window helper/ghost", flush=True)
    print("PASS", checks, "native argument-window checks")


if __name__ == "__main__":
    main()
