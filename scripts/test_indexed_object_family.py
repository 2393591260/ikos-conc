#!/usr/bin/env python3
"""Generate or run strict indexed-object-family controls.

Default: ILP32; --bits=64 checks native-header controls only. Original963/964
are immutable ILP32 .i files and are never relabelled LP64. --emit only writes
inputs and a case manifest. No mock pthread implementation is used.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import sqlite3
import subprocess
import sys
import tempfile

ORIGINALS = {
    963: {"name": "per-thread-struct-tid-join.i",
          "sha256": "47d121a3ae2309b770fddb1dcc6d4dbf51fd97819d409053370dc1a8e56b3cb4"},
    964: {"name": "per-thread-struct-tid.i",
          "sha256": "50a2ae481121151408fdf04d6395298590d7fe4e22a80bfe577170db9bd894e9"},
}
DEFAULT_SELECTORS = {
    "completion": ("thread_lifecycle_contract", "indexed-object-family-completion-v1"),
    "frame": ("thread_lifecycle_contract", "indexed-object-family-frame-v1"),
    "fields": ("thread_lifecycle_contract", "indexed-object-family-fields-v1"),
    "sibling": ("thread_lifecycle_contract", "indexed-object-family-sibling-v1"),
}
SWITCHES = {
    "producer-off": "IKOS_RACE_DISABLE_INDEXED_OBJECT_PRODUCER",
    "consumer-off": "IKOS_RACE_DISABLE_INDEXED_OBJECT_CONSUMER",
    "fields-off": "IKOS_RACE_DISABLE_INDEXED_OBJECT_FIELDS",
    "completion-off": "IKOS_RACE_DISABLE_INDEXED_OBJECT_COMPLETION",
}
KINDS = frozenset(DEFAULT_SELECTORS)
MODES = ("normal", *SWITCHES)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def source(family, **changes):
    """All changes replace a named semantic slot, not arbitrary source text."""
    slots = dict(
        datum="int",
        record="pthread_t tid;" if family == 963 else "pthread_t tid; datum data;",
        record_attribute="",
        extra_globals="",
        worker_prefix="",
        worker_effect=("pthread_mutex_lock(&mutex); shared = __VERIFIER_nondet_int(); "
                       "pthread_mutex_unlock(&mutex);") if family == 963 else
                      "object *t = argument; t->data = __VERIFIER_nondet_int();",
        worker_tail="",
        count="__VERIFIER_nondet_int()",
        guards="assume_abort_if_not(count >= 0);\n  assume_abort_if_not((size_t)count <= UINTPTR_MAX / sizeof(object *));",
        table_extent="(size_t)count * sizeof(object *)",
        table_allocation="checked_malloc",
        before_create="",
        create_init="int i = 0",
        create_condition="i < count",
        create_step="i++",
        object_allocation="object *t = checked_malloc(sizeof(object));",
        after_allocation="",
        table_store="table[i] = t;",
        create="pthread_create(&t->tid, 0, worker, t);",
        after_create="",
        before_join="",
        join_init="int j = 0",
        join_condition="j < count",
        join_step="j++",
        before_own_join="",
        join="pthread_join(table[j]->tid, 0);",
        own_free="free(table[j]);",
        after_own_free="",
        after_join="",
        table_free="free(table);",
        tail="return shared;" if family == 963 else "return 0;",
    )
    unknown = set(changes) - set(slots)
    if unknown:
        raise ValueError("Unknown source slots: " + repr(unknown))
    slots.update(changes)
    s = slots
    return f"""#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
extern int __VERIFIER_nondet_int(void);
typedef {s['datum']} datum;
typedef struct {s['record_attribute']} object {{ {s['record']} }} object;
datum shared;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
{s['extra_globals']}
void assume_abort_if_not(int condition) {{ if (!condition) abort(); }}
void *checked_malloc(size_t size) {{
  void *pointer = malloc(size);
  if (pointer == 0) abort();
  return pointer;
}}
void *worker(void *argument) {{
  {s['worker_prefix']}
  {s['worker_effect']}
  {s['worker_tail']}
  return 0;
}}
int main(void) {{
  int count = {s['count']};
  {s['guards']}
  object **table = {s['table_allocation']}({s['table_extent']});
  {s['before_create']}
  for ({s['create_init']}; {s['create_condition']}; {s['create_step']}) {{
    {s['object_allocation']}
    {s['after_allocation']}
    {s['table_store']}
    {s['create']}
    {s['after_create']}
  }}
  {s['before_join']}
  for ({s['join_init']}; {s['join_condition']}; {s['join_step']}) {{
    {s['before_own_join']}
    {s['join']}
    {s['own_free']}
    {s['after_own_free']}
  }}
  {s['after_join']}
  {s['table_free']}
  {s['tail']}
}}
"""


def cases():
    out = []

    def add(name, family, *, positive=False, unsafe=False, optional=False,
            reason="", ablations=False, **changes):
        kinds = ({"completion", "frame"} if family == 963 else
                 {"fields", "sibling"}) if positive else set()
        out.append(dict(name=f"{family}-{name}", family=family,
                        source=source(family, **changes).encode(),
                        required=sorted(kinds), allow_any=optional,
                        require_safe=positive and not unsafe, must_not_safe=unsafe,
                        reason=reason, modes=MODES if ablations else ("normal",),
                        original=False, bits=("32", "64")))

    for f in (963, 964):
        add("canonical", f, positive=True, ablations=True,
            reason="Complete create/table/own-join/free frame; distinct proof categories")
        add("wide-payload", f, positive=True, datum="long long",
            reason="ILP32 64-bit scalar and target-derived byte layout")
        add("renamed", f, positive=True, reason="No identifier-specific permission")
        out[-1]["source"] = (out[-1]["source"].decode()
            .replace("worker", "entry").replace("object", "record")
            .replace("table", "slots").replace("shared", "payload")
            .replace("checked_malloc", "new_storage").replace("tid", "handle").encode())
        add("zero-count", f, count="0", optional=True,
            reason="No required proof for a vacuous family; no false race")
        add("one-count", f, count="1", optional=True,
            reason="Optimization may eliminate loops; proof presence is not required")
        add("unchecked-outer", f, table_allocation="malloc",
            reason="Actual table nonNULL continuation is missing")
        add("unchecked-inner", f, object_allocation="object *t = malloc(sizeof(object));",
            reason="Actual object nonNULL continuation is missing")
        add("inverted-inner-null", f,
            object_allocation="object *t = malloc(sizeof(object)); if (t != 0) abort();",
            reason="Inverted success path cannot prove live object")
        add("continuing-null-branch", f,
            object_allocation="object *t = malloc(sizeof(object)); if (t == 0) shared = 1;",
            reason="Failed malloc has a continuing side-effect path")
        add("unrelated-inner-guard", f,
            object_allocation="object *t = malloc(sizeof(object)); if (table == 0) abort();",
            reason="NonNULL check names a different allocation")
        add("undersized-object", f,
            object_allocation="object *t = checked_malloc(sizeof(object) - 1);",
            reason="Allocation does not cover the full typed object")
        add("shared-object", f,
            before_create="object *one = checked_malloc(sizeof(object));",
            object_allocation="object *t = one;", own_free="",
            reason="One live allocation is reused by multiple activations")
        add("duplicate-entry", f,
            after_allocation="if (i > 0) t = table[i - 1];",
            reason="Different table indices can name the same object")
        add("table-index-zero", f, table_store="table[0] = t;",
            reason="No injective i-to-cell relation")
        add("table-byte-clobber", f,
            after_create="((unsigned char *)&table[i])[0] = 0;",
            reason="Partial write invalidates pointer cell provenance")
        add("table-memcpy", f,
            after_create="memcpy(&table[i], &t, sizeof(t));",
            reason="Unmodelled duplicate byte-copy effects must not enter the strict frame")
        add("alternate-object-phi", f,
            after_allocation="if (__VERIFIER_nondet_int()) t = 0;",
            reason="MAY source merge does not prove exact current object")
        add("table-escape", f, extra_globals="object **escaped_table;",
            before_join="escaped_table = table;",
            reason="Table publication exceeds the closed parent-only use graph")
        add("object-escape", f, extra_globals="object *escaped;",
            after_allocation="escaped = t;",
            reason="Object publication exceeds own table cell and own child")
        add("interior-argument", f,
            create="pthread_create(&t->tid, 0, worker, (char *)t + 1);",
            reason="Callback argument must be the actual zero-offset object")
        add("table-as-argument", f,
            create="pthread_create(&t->tid, 0, worker, table);",
            reason="Matching MAY allocation class is insufficient")
        add("other-object-argument", f,
            after_allocation="object *other = checked_malloc(sizeof(object));",
            create="pthread_create(&t->tid, 0, worker, other);",
            reason="Create output and callback argument name different births")
        add("numeric-argument", f,
            create="pthread_create(&t->tid, 0, worker, (void *)(uintptr_t)1);",
            reason="Numeric pointers cannot supply object identity")
        add("double-create", f,
            after_create="pthread_create(&t->tid, 0, worker, t);",
            reason="Two children of one object, including retry attempts")
        add("ordinary-worker-call", f, after_create="worker(t);",
            reason="Every activation must arise from the sole create")
        add("detach", f, after_create="pthread_detach(t->tid);",
            reason="Sole ordinary joinable-child contract is broken")
        add("cancel", f, after_create="pthread_cancel(t->tid);",
            reason="Cancellation is outside the frame")
        add("observe-create-return", f,
            create="if (pthread_create(&t->tid, 0, worker, t)) abort();",
            reason="Return-dependent control is outside the first subset")
        add("tid-integer-observation", f, after_create="shared = (int)t->tid;",
            reason="Opaque native handle may not be interpreted numerically")
        add("different-join-N", f,
            before_join="int other = __VERIFIER_nondet_int();",
            join_condition="j < other",
            reason="Equal-looking ranges do not establish actual count identity")
        add("join-skip-first", f, join_init="int j = 1",
            reason="Full index domain and per-object release induction missing")
        add("join-skip-last", f, join_condition="j < count - 1",
            reason="Last child is not consumed")
        add("join-step-two", f, join_step="j += 2",
            reason="Sparse join domain")
        add("conditional-join", f,
            join="if (__VERIFIER_nondet_int()) pthread_join(table[j]->tid, 0);",
            reason="A continuing release may bypass its own join")
        add("early-join-break", f,
            after_own_free="if (__VERIFIER_nondet_int()) break;",
            reason="Loop exit does not entail all children complete")
        add("wrong-join-cell", f, join="pthread_join(table[j / 2]->tid, 0);",
            reason="Join and release can consume different object generations")
        add("wrong-free-cell", f, own_free="free(table[(j + 1) % count]);",
            reason="Release is not tied to the just-consumed own child")
        add("double-free", f, own_free="free(table[j]); free(table[j]);",
            reason="Exactly one per-object release")
        add("interior-free", f, own_free="free((char *)table[j] + 1);",
            reason="Only the exact allocation base can be released")
        add("free-before-join", f, before_own_join="free(table[j]);",
            own_free="", reason="A matching join cannot resurrect released storage")
        add("outer-free-before-join", f, before_join="free(table);",
            table_free="", reason="Table must remain live through every reload")
        add("creation-phase-free", f, after_create="free(t);",
            reason="Earlier objects must remain live throughout creation")
        add("creation-phase-realloc", f,
            after_create="table[i] = realloc(t, sizeof(object));",
            reason="Fresh-family generation and stable table identity invalidated")
        add("nonnull-join-result", f, before_join="void *result;",
            join="pthread_join(table[j]->tid, &result);",
            reason="Additional API output effect is outside this envelope")
        add("parent-held-lock", f, before_join="pthread_mutex_lock(&mutex);",
            after_join="pthread_mutex_unlock(&mutex);",
            reason="Parent lock can block a successful child's completion")
        add("opaque-effect", f, extra_globals="extern void opaque(void *);",
            after_create="opaque(t);",
            reason="Unmodelled effects cannot be waived by a structural certificate")
        add("extent-other-N", f,
            table_extent="(size_t)(count + 1) * sizeof(object *)",
            reason="Table extent must use the exact family count")
        add("create-step-two", f, create_step="i += 2",
            reason="Unwritten join slots violate complete creation induction")

    add("postread-before-joins", 963, unsafe=True, optional=True,
        before_join="datum early = shared;", tail="return early;",
        reason="Concrete global read may precede successful worker writes")
    add("unlocked-global-workers", 963, unsafe=True, optional=True,
        worker_effect="shared = __VERIFIER_nondet_int();",
        reason="Family completion must never order parallel children")
    add("padding-reordered-fields", 964, positive=True,
        record="unsigned char prefix; pthread_t tid; unsigned char padding[3]; datum data;",
        reason="Target layout, not tid==0/data==word")
    add("worker-tid-write", 964,
        worker_tail="t->tid = (pthread_t)0;",
        reason="Worker can clobber handle before own join")
    add("worker-tid-byte-write", 964,
        worker_tail="((unsigned char *)&t->tid)[sizeof(pthread_t) - 1] = 0;",
        reason="Byte overlap invalidates handle footprint")
    add("overlapping-union", 964,
        record="union { pthread_t tid; datum data; };",
        reason="Payload and handle bytes overlap")
    add("packed-unaligned-tid", 964,
        record_attribute="__attribute__((packed))",
        record="unsigned char prefix; pthread_t tid; datum data;",
        reason="Native handle output requires actual alignment")
    add("worker-free", 964, worker_tail="free(t);",
        reason="Per-object release must remain in the parent after own join")
    add("worker-return-object", 964, worker_tail="return t;",
        reason="The first frame rejects object return/publication")
    add("worker-global-alias", 964,
        extra_globals="object *last;",
        after_allocation="last = t;",
        worker_effect="last->data = __VERIFIER_nondet_int();",
        reason="A worker may access another instance rather than its actual argument")
    add("uninitialized-payload-read", 964,
        worker_effect="object *t = argument; t->data = t->data + 1;",
        reason="Fresh allocation is not payload initialization")
    add("create-output-is-data", 964,
        create="pthread_create((pthread_t *)&t->data, 0, worker, t);",
        reason="width0 CallBase is not permission for an arbitrary output footprint")
    add("create-output-is-global", 964,
        create="pthread_create((pthread_t *)&shared, 0, worker, t);",
        reason="Actual create output is not the certified object's tid field")
    add("nonnull-create-attr", 964,
        before_create="pthread_attr_t attr; pthread_attr_init(&attr);",
        create="pthread_create(&t->tid, &attr, worker, t);",
        reason="Additional library input/effect scope")
    add("same-object-with-valid-single-join", 964, unsafe=True,
        before_create="object *one = checked_malloc(sizeof(object));",
        object_allocation="object *t = one;",
        join_init="int j = count > 0 ? count - 1 : 0",
        own_free="", table_free="", reason="Two successful children can concurrently write one live data field")
    return out


def original_case(index, path):
    data = path.read_bytes()
    if sha256(data) != ORIGINALS[index]["sha256"]:
        raise ValueError(f"Original {index} input hash mismatch: {path}")
    kinds = {"completion", "frame"} if index == 963 else {"fields", "sibling"}
    return dict(name=f"original-{index}", family=index, source=data,
                required=sorted(kinds), allow_any=False, require_safe=True,
                must_not_safe=False, reason="Unmodified SHA-bound ILP32 original",
                modes=MODES, original=True, bits=("32",), source_path=str(path))


def case_metadata(case):
    return {**{k: v for k, v in case.items() if k != "source"},
            "input_sha256": sha256(case["source"]), "status": "NOT_RUN"}


def run_process(command, env, timeout, cwd):
    process = subprocess.Popen(command, cwd=cwd, env=env, stdout=subprocess.PIPE,
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


def infos_from_database(path):
    # Read only the checker evidence; this function never reruns an analysis.
    with sqlite3.connect(f"file:{path}?mode=ro", uri=True) as db:
        return [json.loads(row[0]) for row in db.execute("SELECT info FROM checks")
                if row[0] is not None]


def classify(infos, selectors):
    return {kind: [info for info in infos if info.get(key) == value]
            for kind, (key, value) in selectors.items()}


def check_result(case, mode, verdict, hits, *, consumer_policy):
    present = {kind for kind, rows in hits.items() if rows}
    if case["allow_any"]:
        if case["must_not_safe"]:
            if verdict == "SAFE":
                raise AssertionError("Race control was declared SAFE")
        elif verdict == "FALSE":
            raise AssertionError("Vacuous/single-object safe control became FALSE")
        return
    if mode == "producer-off" or (mode == "consumer-off" and consumer_policy == "all"):
        if present:
            raise AssertionError(f"Disabled frame still emitted proofs: {present}")
    elif mode == "consumer-off":
        if "completion" in present:
            raise AssertionError("Disabled completion consumer still emitted completion")
    elif mode == "completion-off":
        if "completion" in present:
            raise AssertionError("Disabled completion consumer still emitted completion")
    elif mode == "fields-off":
        if present & {"fields", "sibling"}:
            raise AssertionError(f"Disabled field consumer still emitted {present}")
    elif case["required"]:
        if not set(case["required"]) <= present:
            raise AssertionError(f"Missing proof categories: {set(case['required']) - present}")
    elif present:
        raise AssertionError(f"Damaged frame received a new certificate: {present}")
    if mode == "normal" and case["require_safe"] and verdict != "SAFE":
        raise AssertionError(f"Positive complete frame is {verdict}")
    if case["must_not_safe"] and verdict == "SAFE":
        raise AssertionError("Race control was declared SAFE")
    if mode in ("producer-off", "consumer-off") and verdict == "SAFE":
        raise AssertionError("Canonical/original ablation unexpectedly remained SAFE")
    if mode == "fields-off" and case["family"] == 964 and verdict == "SAFE":
        raise AssertionError("964 still SAFE without field/sibling separation")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--ikos", type=Path)
    parser.add_argument("--bits", choices=("32", "64", "both"), default="32")
    parser.add_argument("--original963", type=Path)
    parser.add_argument("--original964", type=Path)
    parser.add_argument("--no-originals", action="store_true")
    parser.add_argument("--only", action="append", default=[], help="Exact case name; repeatable")
    parser.add_argument("--emit", type=Path, help="Write sources/metadata only; no analyzer execution")
    parser.add_argument("--output", type=Path, help="Keep per-run evidence; otherwise a temporary directory")
    parser.add_argument("--timeout", type=float, default=45)
    parser.add_argument("--selector", action="append", default=[], metavar="KIND=KEY=VALUE",
                        help="Override exact proof metadata names after implementation coordination")
    parser.add_argument("--consumer-off-policy", choices=("all", "completion"), default="all",
                        help="Contract coordination only: default disables all object consumers")
    args = parser.parse_args()
    selectors = dict(DEFAULT_SELECTORS)
    for item in args.selector:
        kind, key, value = item.split("=", 2)
        if kind not in KINDS or not key or not value:
            parser.error("Invalid exact proof selector: " + item)
        selectors[kind] = (key, value)
    selected = cases()
    bits_list = ("32", "64") if args.bits == "both" else (args.bits,)
    if not args.no_originals and "32" in bits_list:
        for index in (963, 964):
            override = getattr(args, f"original{index}")
            if override:
                selected.append(original_case(index, override))
    if args.only:
        missing = set(args.only) - {c["name"] for c in selected}
        if missing:
            parser.error("Unknown/unavailable cases: " + repr(sorted(missing)))
        selected = [c for c in selected if c["name"] in args.only]
    manifest = dict(schema="indexed-object-family-controls-v1", status="GENERATED_NOT_RUN",
                    default_abi="ILP32", selected_bits=list(bits_list), bmc_deadline_ms=0,
                    originals_only_ilp32=True, selectors=selectors, switches=SWITCHES,
                    consumer_off_policy=args.consumer_off_policy,
                    cases=[case_metadata(c) for c in selected])
    if args.emit:
        args.emit.mkdir(parents=True, exist_ok=True)
        for case in selected:
            suffix = ".i" if case["original"] else ".c"
            (args.emit / (case["name"] + suffix)).write_bytes(case["source"])
        (args.emit / "cases.json").write_text(json.dumps(manifest, indent=2) + "\n")
        print(json.dumps({"status": "EMITTED_NOT_RUN", "cases": len(selected)}))
        return
    sys.path.insert(0, str(args.repo / "scripts"))
    sys.path.insert(0, str(args.repo))
    from ikos_runtime import runtime_command, runtime_identity, verify_runtime
    from analyzer.python.ikos.race_result import validate_race_result

    ikos = (args.ikos or args.repo / "install/bin/ikos").resolve()
    identity = runtime_identity(ikos)
    prefix = runtime_command(ikos)
    base_env = {k: v for k, v in os.environ.items()
                if not k.startswith("IKOS_") and k not in ("PYTHONPATH", "PYTHONHOME")}
    temporary = None
    if args.output:
        work = args.output.resolve()
        if work.exists() and any(work.iterdir()):
            raise FileExistsError("Evidence directory must be empty: " + str(work))
        work.mkdir(parents=True, exist_ok=True)
    else:
        temporary = tempfile.TemporaryDirectory(prefix="ikos-indexed-object-family-")
        work = Path(temporary.name)
    results = []
    manifest["runtime_identity"] = identity
    manifest["status"] = "RUNNING_NOT_STAGE_ACCEPTANCE"
    manifest["runs"] = results
    evidence = work / "results.json"
    try:
        for case in selected:
            suffix = ".i" if case["original"] else ".c"
            input_file = work / (case["name"] + suffix)
            input_file.write_bytes(case["source"])
            for bits in bits_list:
                if bits not in case["bits"]:
                    continue
                for mode in case["modes"]:
                    stem = f"{case['name']}-{bits}-{mode}"
                    db, report = work / (stem + ".db"), work / (stem + ".json")
                    if db.exists() or report.exists():
                        raise FileExistsError("Use a fresh evidence directory: " + stem)
                    env = dict(base_env)
                    if mode != "normal":
                        env[SWITCHES[mode]] = "1"
                    command = [*prefix, "--analyses=race", "--concurrency=auto",
                               "--partitioning=guards", "--demote-race-to-unknown",
                               "--bmc-deadline-ms=0", "-m" + bits, "--format=json",
                               "--report-file=" + str(report), "-o", str(db), str(input_file)]
                    proc = run_process(command, env, args.timeout, work)
                    (work / (stem + ".stdout")).write_text(proc.stdout)
                    (work / (stem + ".stderr")).write_text(proc.stderr)
                    if proc.returncode:
                        raise AssertionError((stem, proc.returncode, proc.stdout, proc.stderr))
                    verdict = validate_race_result(json.loads(report.read_text()))["verdict"]
                    hits = classify(infos_from_database(db), selectors)
                    row = dict(case=case["name"], family=case["family"], bits=bits, mode=mode,
                               input_sha256=sha256(input_file.read_bytes()), verdict=verdict,
                               proofs={k: len(v) for k, v in hits.items()},
                               proof_records=hits, command=command, timeout_wall_seconds=args.timeout,
                               bmc_deadline_ms=0, status="CHECKING")
                    results.append(row)
                    evidence.write_text(json.dumps(manifest, indent=2) + "\n")
                    check_result(case, mode, verdict, hits,
                                 consumer_policy=args.consumer_off_policy)
                    row["status"] = "PASS"
                    print(f"{stem}: {verdict}, proof_counts={row['proofs']}", flush=True)
        manifest["status"] = "LOCAL_CONTROLS_PASS_NOT_STAGE_ACCEPTANCE"
    except BaseException as error:
        manifest["status"] = "FAILED_NOT_ACCEPTED"
        manifest["failure"] = repr(error)
        raise
    finally:
        try:
            verify_runtime(identity)
        except BaseException as identity_error:
            manifest["status"] = "RUNTIME_IDENTITY_FAILED_NOT_ACCEPTED"
            manifest["runtime_failure"] = repr(identity_error)
            raise
        finally:
            evidence.write_text(json.dumps(manifest, indent=2) + "\n")
            if temporary:
                print("Temporary evidence is removed; use --output to retain evidence.", flush=True)
                temporary.cleanup()
    print(json.dumps({"status": manifest["status"], "runs": len(results)}))


if __name__ == "__main__":
    main()

