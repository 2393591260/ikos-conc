#!/usr/bin/env python3
"""Positive and damaged-premise controls for indexed handle-family completion.

ILP32 is the default; --bits=64 checks LP64. Analysis uses the default domain
and disables BMC so that execution confirmation cannot mask a bad exclusion.
"""
import argparse
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
void assume_abort_if_not(int b) { if (!b) abort(); }
void *safe_malloc(size_t bytes) {
  void *p = malloc(bytes);
  if (p == 0) abort();
  return p;
}
typedef int datum;
datum payload;
pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
void *worker(void *argument) {
  pthread_mutex_lock(&guard);
  payload = __VERIFIER_nondet_int();
  pthread_mutex_unlock(&guard);
  return 0;
}
int main(void) {
  int count = __VERIFIER_nondet_int();
  assume_abort_if_not(count >= 0);
  assume_abort_if_not((unsigned)count <= UINT32_MAX / sizeof(pthread_t));
  pthread_t *handles = safe_malloc((size_t)count * sizeof(pthread_t));
  for (int i = 0; i < count; i++)
    pthread_create(&handles[i], 0, worker, 0);
  for (int j = 0; j < count; j++)
    pthread_join(handles[j], 0);
  free(handles);
  return payload;
}
"""
UNSIGNED_BASE = BASE
SIGNED_GUARD = "  assume_abort_if_not((size_t)count <= (size_t)PTRDIFF_MAX / sizeof(pthread_t));\n"
SIGNED_BASE = BASE.replace("  pthread_t *handles =", SIGNED_GUARD + "  pthread_t *handles =")
CREATE = "pthread_create(&handles[i], 0, worker, 0);"
JOIN = "pthread_join(handles[j], 0);"
def cases():
    return [
        ("canonical", BASE, True, False),
        ("original-unsigned-limit", UNSIGNED_BASE, True, False),
        ("stronger-signed-limit", SIGNED_BASE, True, False),
        ("unchecked-malloc", BASE.replace("  if (p == 0) abort();\n", ""), False, False),
        ("inverted-null-guard", BASE.replace("if (p == 0)", "if (p != 0)"), False, False),
        ("ineffective-null-guard", BASE.replace("  if (p == 0) abort();", "  if (p == 0) { (void)0; }"), False, False),
        ("guard-unrelated-pointer", BASE.replace("  if (p == 0) abort();", "  if ((void*)&payload == 0) abort();").replace("void *safe_malloc", "extern int payload;\nvoid *safe_malloc"), False, False),
        ("parent-held-lock", BASE.replace("  for (int j", "  pthread_mutex_lock(&guard);\n  for (int j").replace("  free(handles);", "  pthread_mutex_unlock(&guard);\n  free(handles);"), False, False),
        ("non-null-join-output", BASE.replace("  for (int j", "  void *result;\n  for (int j").replace("pthread_join(handles[j], 0)", "pthread_join(handles[j], &result)"), False, False),
        ("signed-extent-product", BASE.replace("safe_malloc((size_t)count * sizeof(pthread_t))", "safe_malloc((size_t)(count * (int)sizeof(pthread_t)))"), False, False),
        ("extent-different-N", BASE.replace("safe_malloc((size_t)count * sizeof(pthread_t))", "safe_malloc((size_t)(count + 1) * sizeof(pthread_t))"), False, False),
        ("cross-signed-boundary", BASE.replace("  pthread_t *handles =", "  assume_abort_if_not(count >= 536870912);\n  assume_abort_if_not(count <= 536870913);\n  pthread_t *handles ="), True, False),
        ("extent-wrap-equality", BASE.replace("UINT32_MAX / sizeof(pthread_t)", "UINT32_MAX / sizeof(pthread_t) + 1"), False, False),
        ("unrelated-count-guard", BASE.replace(
            "  assume_abort_if_not((unsigned)count <= UINT32_MAX / sizeof(pthread_t));",
            "  int other = __VERIFIER_nondet_int();\n"
            "  assume_abort_if_not((unsigned)other <= UINT32_MAX / sizeof(pthread_t));"), False, False),
        ("guard-one-path-only", BASE.replace(
            "  assume_abort_if_not((unsigned)count <= UINT32_MAX / sizeof(pthread_t));",
            "  if (__VERIFIER_nondet_int()) assume_abort_if_not((unsigned)count <= UINT32_MAX / sizeof(pthread_t));"), False, False),
        ("guard-or-unrelated-truth", BASE.replace(
            "(unsigned)count <= UINT32_MAX / sizeof(pthread_t)",
            "((unsigned)count <= UINT32_MAX / sizeof(pthread_t)) || __VERIFIER_nondet_int()"), False, False),
        ("guard-truncated-count", BASE.replace(
            "(unsigned)count <= UINT32_MAX / sizeof(pthread_t)",
            "(unsigned char)count <= 127"), False, False),
        ("count-replaced-after-guard", BASE.replace(
            "  pthread_t *handles =", "  count = __VERIFIER_nondet_int();\n  pthread_t *handles ="), False, False),
        ("guard-after-extent", BASE.replace(
            "  assume_abort_if_not((unsigned)count <= UINT32_MAX / sizeof(pthread_t));\n", "").replace(
            "  for (int i =", "  assume_abort_if_not((unsigned)count <= UINT32_MAX / sizeof(pthread_t));\n  for (int i ="), False, False),
        ("wide-data", BASE.replace("typedef int datum;", "typedef long long datum;"), True, False),
        ("renamed", BASE.replace("worker", "entry").replace("count", "limit")
            .replace("handles", "ids").replace("payload", "shared_data"), True, False),
        ("no-free", BASE.replace("  free(handles);\n", ""), True, False),
        ("missing-join", BASE.replace("  for (int j = 0; j < count; j++)\n    " + JOIN, ""), False, True),
        ("short-bound", BASE.replace("j < count;", "j < count - 1;"), False, True),
        ("skip-first", BASE.replace("int j = 0;", "int j = 1;"), False, True),
        ("skip-stride", BASE.replace("j++)", "j += 2)"), False, True),
        ("conditional-join", BASE.replace(JOIN, "if (__VERIFIER_nondet_int()) " + JOIN), False, True),
        ("early-break", BASE.replace(JOIN, "{ " + JOIN + " if (__VERIFIER_nondet_int()) break; }"), False, True),
        ("different-dynamic-bound", BASE.replace("  for (int j", "  int other = __VERIFIER_nondet_int();\n  for (int j")
            .replace("j < count;", "j < other;"), False, True),
        ("reverse-slots", BASE.replace("pthread_join(handles[j]", "pthread_join(handles[count - j - 1]"), False, False),
        ("duplicate-slots", BASE.replace("pthread_join(handles[j]", "pthread_join(handles[j / 2]"), False, True),
        ("create-step-two", BASE.replace("i++)", "i += 2)"), False, False),
        ("counter-wrap", BASE.replace("int j = 0;", "unsigned char j = 0;"), False, False),
        ("mixed-loop-signedness", BASE.replace(
            "int count = __VERIFIER_nondet_int();",
            "unsigned short count = (unsigned short)__VERIFIER_nondet_int();")
            .replace("int i = 0; i < count; i++", "unsigned short i = 0; i < count; i++")
            .replace("int j = 0; j < count; j++", "short j = 0; j < (short)count; j++"), False, True),
        ("narrow-unsigned-counter", BASE.replace(
            "int count = __VERIFIER_nondet_int();",
            "unsigned short count = (unsigned short)__VERIFIER_nondet_int();")
            .replace("int i = 0; i < count; i++", "unsigned short i = 0; i < count; i++")
            .replace("int j = 0; j < count; j++", "unsigned short j = 0; j < count; j++"), False, False),
        ("wide-cap-signed-range", BASE.replace(
            "extern int __VERIFIER_nondet_int(void);",
            "extern int __VERIFIER_nondet_int(void);\nextern unsigned __VERIFIER_nondet_uint(void);")
            .replace("int count = __VERIFIER_nondet_int();",
                "unsigned count = __VERIFIER_nondet_uint();\n"
                "  assume_abort_if_not((int)count >= -100);\n"
                "  assume_abort_if_not((int)count <= 100);")
            .replace("assume_abort_if_not(count >= 0);", "")
            .replace("UINT32_MAX / sizeof(pthread_t)", "UINT32_MAX - 1")
            .replace("int i = 0;", "unsigned i = 0;")
            .replace("int j = 0;", "unsigned j = 0;"), False, False),
        ("no-bound-span", BASE.replace(
            "  assume_abort_if_not((unsigned)count <= UINT32_MAX / sizeof(pthread_t));\n", "")
            .replace(SIGNED_GUARD, ""), False, False),
        ("slot-overwrite", BASE.replace("  for (int j", "  handles[0] = 0;\n  for (int j"), False, True),
        ("byte-overwrite", BASE.replace("  for (int j",
            "  ((unsigned char*)handles)[sizeof(pthread_t)-1] = 0;\n  for (int j"), False, True),
        ("base-escape", BASE.replace("typedef int datum;", "extern void escape(void*);\ntypedef int datum;")
            .replace("  for (int j", "  escape(handles);\n  for (int j"), False, False),
        ("second-create", BASE.replace(CREATE, "{ " + CREATE + " " + CREATE + " }"), False, True),
        ("detach", BASE.replace(CREATE, "{ " + CREATE + " pthread_detach(handles[i]); }"), False, True),
        ("worker-joiner", BASE.replace("  payload =", "  pthread_join(pthread_self(), 0);\n  payload ="), False, False),
        ("ordinary-worker-activation", BASE.replace("  return payload;", "  worker(0);\n  return payload;"), False, False),
        ("endpoint-before-complete", BASE.replace("  for (int j",
            "  datum before = payload;\n  for (int j").replace("  return payload;", "  return before;"), False, True),
        ("join-loop-reentry", BASE.replace("  for (int j", "  for (int r=0;r<2;r++) {\n  for (int j")
            .replace("  free(handles);", "  }\n  free(handles);"), False, False),
        ("family-reentry", BASE.replace("  for (int i", "  for (int r=0;r<2;r++) {\n  for (int i")
            .replace("  free(handles);", "  }\n  free(handles);"), False, False),
        ("free-before-join", BASE.replace("  free(handles);\n", "")
            .replace("  for (int j", "  free(handles);\n  for (int j"), False, False),
        ("distinct-allocation", BASE.replace("  for (int j",
            "  pthread_t *other = safe_malloc((size_t)count * sizeof(pthread_t));\n  for (int j")
            .replace("pthread_join(handles[j]", "pthread_join(other[j]"), False, True),
        ("same-site-new-activation", BASE.replace("  for (int j",
            "  handles = safe_malloc((size_t)count * sizeof(pthread_t));\n  for (int j"), False, True),
        ("worker-no-lock", BASE.replace("  pthread_mutex_lock(&guard);\n", "")
            .replace("  pthread_mutex_unlock(&guard);\n", ""), True, True),
        ("non-native-callback", BASE.replace("void *worker(void *argument)", "void worker(int argument)")
            .replace("  return 0;\n}", "  return;\n}")
            .replace(", 0, worker, 0)", ", 0, (void *(*)(void*))worker, 0)"), False, False),
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



def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path("/home/ruan/ikos-conc"))
    parser.add_argument("--ikos", type=Path)
    parser.add_argument("--bits", choices=("32", "64", "both"), default="32")
    parser.add_argument("--original", type=Path, help="Unmodified original 976 preprocessed input; include in analyzer checks")
    parser.add_argument("--emit", type=Path, help="Write C inputs and metadata; do not run the analyzer")
    args = parser.parse_args()
    selected = cases()
    if args.original:
        selected = [("original-976", args.original.read_text(), True, False)] + selected
    if args.emit:
        args.emit.mkdir(parents=True, exist_ok=True)
        rows = []
        for name, source, proof, unsafe in selected:
            (args.emit / (name + (".i" if name == "original-976" else ".c"))).write_text(source)
            rows.append(dict(name=name, indexed_proof=proof, must_not_be_safe=unsafe))
        (args.emit / "cases.json").write_text(json.dumps(rows, indent=2) + "\n")
        return
    sys.path.insert(0, str(args.repo / "scripts"))
    from ikos_runtime import runtime_command, runtime_identity, verify_runtime
    ikos = args.ikos or args.repo / "install/bin/ikos"
    identity = runtime_identity(ikos)
    command = runtime_command(ikos)
    base_env = {k: v for k, v in os.environ.items()
                if not k.startswith("IKOS_") and k not in ("PYTHONPATH", "PYTHONHOME")}
    with tempfile.TemporaryDirectory(prefix="ikos-indexed-join-") as temp:
        work = Path(temp)
        for bits in ("32", "64") if args.bits == "both" else (args.bits,):
            for name, source, positive, unsafe in selected:
                # A nonnegative i32 count times an eight-byte handle cannot wrap
                # LP64 size_t: these guard variants remain valid without a cap.
                if bits == "64" and name in {
                    "extent-wrap-equality", "unrelated-count-guard",
                    "guard-one-path-only", "guard-or-unrelated-truth",
                    "guard-truncated-count", "guard-after-extent", "no-bound-span",
                    "wide-cap-signed-range",
                }:
                    positive = True
                controls = ("normal", "producer-off", "consumer-off") if name in ("canonical", "original-976") else ("normal",)
                for mode in controls:
                    env = dict(base_env)
                    if mode != "normal":
                        env["IKOS_RACE_DISABLE_INDEXED_JOIN_" + mode.split("-")[0].upper()] = "1"
                    input_file = work / ("input.i" if name == "original-976" else "input.c")
                    input_file.write_text(source)
                    database = work / f"{name}-{bits}-{mode}.db"
                    proc = run_analyzer([*command, "--analyses=race", "--concurrency=auto",
                        "--partitioning=guards", "--demote-race-to-unknown",
                        "--bmc-deadline-ms=0", "-m" + bits, "-o", str(database), str(input_file)],
                        env=env, timeout=45)
                    output = proc.stdout + proc.stderr
                    assert proc.returncode == 0, (name, bits, mode, output)
                    with sqlite3.connect(database) as db:
                        proofs = [json.loads(row[0]) for row in db.execute("SELECT info FROM checks")
                                  if json.loads(row[0]).get("thread_lifecycle_contract")
                                  == "indexed-native-handle-completion-v1"]
                    expected_proof = positive and mode == "normal"
                    assert bool(proofs) == expected_proof, (name, bits, mode, proofs, output)
                    safe = "The program is SAFE" in output
                    if (positive and not unsafe and mode == "normal"):
                        assert safe, (name, bits, mode, output)
                    if unsafe or mode != "normal":
                        assert not safe, (name, bits, mode, output)
                    print(f"{name}/{bits}/{mode}: proofs={len(proofs)}, SAFE={safe}", flush=True)
    verify_runtime(identity)

if __name__ == "__main__":
    main()
