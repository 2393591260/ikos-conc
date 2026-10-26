#!/usr/bin/env python3
"""Native pthread initialization bytes, actual indices and modular-offset controls."""
import argparse
import json
import math
import os
from pathlib import Path
import re
import sqlite3
import subprocess
import tempfile

from ikos_runtime import runtime_command, runtime_identity, verify_runtime

ROOT = Path(__file__).resolve().parents[1]
HEAP = "constant-heap-runtime-byte-frame-v1"
ARRAY = "finite-global-array-runtime-byte-frame-v1"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ikos", type=Path, default=ROOT / "install/bin/ikos")
    parser.add_argument("--bits", choices=("32", "64"), default="32")
    args = parser.parse_args()
    bits = int(args.bits)
    identity = runtime_identity(args.ikos)
    command = runtime_command(args.ikos)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith("IKOS_") and k not in {"PYTHONPATH", "PYTHONHOME"}}
    heap = (ROOT / "analyzer/test/regression/bmc-values/heap-pthread-initialization.c").read_text()
    array = (ROOT / "analyzer/test/regression/bmc-values/pthread-array-initialization.c").read_text()
    total = 0
    with tempfile.TemporaryDirectory(prefix="ikos-pthread-init-abi-") as temp:
        work = Path(temp)

        def control(name, body, contract, proof, safe, llvm=False, consumer_blocked=False):
            nonlocal total
            source = work / ("control.ll" if llvm else "control.c")
            source.write_text(body)
            dbpath = work / "result.db"
            proc = subprocess.run(command + ["--analyses=race", "--concurrency=auto",
                "--partitioning=guards", "--demote-race-to-unknown",
                "--bmc-deadline-ms=0", "-m" + args.bits,
                "-o", str(dbpath), str(source)], env=env,
                capture_output=True, text=True, timeout=30)
            output = proc.stdout + proc.stderr
            assert proc.returncode == 0, (name, output)
            with sqlite3.connect(dbpath) as db:
                rows = [json.loads(row[0]) for row in db.execute("SELECT info FROM checks")]
            hits = [row for row in rows if row.get("pthread_initialization_contract") == contract]
            assert bool(hits) == proof, (name, hits, output)
            assert ("The program is SAFE" in output) == safe, (name, rows, output)
            if consumer_blocked:
                assert any(row.get("unknown_reason") ==
                    "mutex array consumer does not establish an initialized leaf"
                    for row in rows), (name, rows, output)
            total += 1
            print(f"pthread-init-native={name}/bits={bits}: proof={proof} SAFE={safe}",
                  flush=True)

        control("heap-native", heap, HEAP, True, True)
        control("heap-wide-datum", "#define WIDE\n" + heap, HEAP, True, True)
        control("heap-native-long", heap.replace("typedef int datum;", "typedef long datum;"),
                HEAP, True, True)
        control("heap-race-preserved", "#define CASE 1\n" + heap, HEAP, True, False)
        control("heap-packed-mutex", heap.replace(
            "struct cell { datum value;", "struct __attribute__((packed)) cell { datum value; char skew;"),
            HEAP, False, False)
        control("heap-partial-handle", heap.replace("  pthread_create(&thread",
            "  ((unsigned char *)&thread)[sizeof thread - 1] = 1;\n  pthread_create(&thread"),
            HEAP, False, False)
        control("heap-narrow-handle", heap.replace("pthread_t thread;", "unsigned int thread;")
            .replace("pthread_create(&thread,", "pthread_create((pthread_t *)&thread,"),
            HEAP, bits == 32, bits == 32)
        control("heap-relay-upper-byte", heap.replace("  pthread_create(&thread",
            "  ((unsigned char *)&cells)[sizeof cells - 1] = 0;\n  pthread_create(&thread"),
            HEAP, False, False)
        control("heap-mutex-last-byte", heap.replace("  pthread_create(&thread",
            "  ((unsigned char *)&cells[0].mutex)[sizeof(pthread_mutex_t) - 1] = 0;\n  pthread_create(&thread"),
            HEAP, False, False)

        control("array-native", "#define CASE 1\n" + array, ARRAY, True, True)
        control("array-wide-datum", "#define CASE 1\n#define WIDE\n" + array,
                ARRAY, True, True)
        control("array-zero-leaf", "#define CASE 1\n" + array.replace("&locks[4]", "&locks[0]"),
                ARRAY, True, True)
        control("array-guarded-index", "#define CASE 2\n" + array, ARRAY, True, False)
        control("array-native-long-index", "#define CASE 2\n" + array.replace(
            "int index = __VERIFIER_nondet_int();", "long index = __VERIFIER_nondet_int();"),
            ARRAY, True, False)
        control("array-long-long-index", "#define CASE 2\n" + array.replace(
            "int index = __VERIFIER_nondet_int();", "long long index = __VERIFIER_nondet_int();"),
            ARRAY, True, False)
        control("array-old-style-nondet", "#define CASE 2\n" + array.replace(
            "extern int __VERIFIER_nondet_int(void);", "extern int __VERIFIER_nondet_int();"),
            ARRAY, True, False)
        control("array-unbounded-consumer", "#define CASE 4\n" + array,
                ARRAY, True, False, consumer_blocked=True)
        control("array-wrong-byte-stride", "#define CASE 2\n" + array.replace(
            "&locks[index]", "(pthread_mutex_t *)((char *)locks + (sizeof(pthread_mutex_t)-1)*index)"),
            ARRAY, False, False)
        control("array-extra-nondet-actual", "#define CASE 2\n" + array.replace(
            "extern int __VERIFIER_nondet_int(void);", "extern int __VERIFIER_nondet_int();")
            .replace("= __VERIFIER_nondet_int();", "= __VERIFIER_nondet_int(1);"),
            ARRAY, False, False)
        control("array-wrong-nondet-return", "#define CASE 2\n" + array.replace(
            "extern int __VERIFIER_nondet_int(void);", "extern long long __VERIFIER_nondet_int(void);"),
            ARRAY, False, False)

        control("array-same-name-body", "#define CASE 2\n" + array.replace(
            "extern int __VERIFIER_nondet_int(void);",
            "int __VERIFIER_nondet_int(void) { ((unsigned char *)&locks[0])[0] = 1; return 0; }"),
            ARRAY, False, False)
        control("array-ordinary-vararg", "#define CASE 1\n" + array.replace(
            "  pthread_create(&thread",
            "  extern void unrelated(int, ...); unrelated(1);\n  pthread_create(&thread"),
            ARRAY, False, False)

        compiler = json.loads(subprocess.check_output(command[:-1] + ["-c",
            "import json; from ikos import settings, analyzer; "
            "print(json.dumps([settings.clang(), analyzer.clang_ikos_flags()]))"], text=True))
        source = work / "native.c"
        # Expose the target's actual cell allocation stride without a host ABI assumption.
        source.write_text(heap + "\nconst unsigned long long abi_cell_extent = sizeof(struct cell);\n")
        ir = work / "native.ll"
        subprocess.run([compiler[0], *compiler[1], "-S", "-emit-llvm",
                        "-m" + args.bits, str(source), "-o", str(ir)],
                       check=True, capture_output=True, env=env)
        original = ir.read_text()
        control("heap-llvm-positive", original, HEAP, True, True, llvm=True)
        match = re.search(r"^@abi_cell_extent = .*constant i64 (\d+)", original, re.MULTILINE)
        assert match, original
        stride = int(match.group(1))
        index = (1 << bits) // math.gcd(stride, 1 << bits) + 1
        assert 0 < index < (1 << (bits - 1))
        assert (index * stride) % (1 << bits) == stride
        # Old fixed-width accumulation maps this huge product back to one cell.
        # The mathematical product must now refuse before that modular result.
        changed, mutations = re.subn(
            r"(getelementptr(?: inbounds)? %struct\.cell, %struct\.cell\* [^,\n]+, i"
            + args.bits + r" )1\b", lambda m: m.group(1) + str(index), original)
        assert mutations, "expected a real cell-step GEP"
        # Do not rely on inbounds making the deliberately wrapping IR poison.
        changed = changed.replace("getelementptr inbounds ", "getelementptr ")
        control("heap-modular-byte-product", changed, HEAP, False, False, llvm=True)
        changed, mutations = re.subn(r"(^@attributes = [^\n]*\balign )\d+",
            r"\g<1>1", original, flags=re.MULTILINE)
        assert mutations == 1
        control("attribute-alignment", changed, HEAP, False, False, llvm=True)
        changed, mutations = re.subn(r"(alloca i" + args.bits + r", align )\d+",
            r"\g<1>1", original)
        assert mutations
        control("handle-alignment", changed, HEAP, False, False, llvm=True)
        changed, mutations = re.subn(r"^%union\.pthread_mutex_t = type .+$",
            "%union.pthread_mutex_t = type { [" + ("40" if bits == 32 else "24") + " x i8] }",
            original, flags=re.MULTILINE)
        assert mutations == 1
        control("wrong-native-mutex-size", changed, HEAP, False, False, llvm=True)
        layout = next(line for line in original.splitlines() if line.startswith("target datalayout = "))
        fields = [field for field in layout.split('"')[1].split("-") if not field.startswith("p:")]
        fields.insert(1, "p:" + args.bits + ":" + args.bits + ":" + args.bits + ":" +
                      ("16" if bits == 32 else "32"))
        control("native-index-width", original.replace(layout, 'target datalayout = "' +
                "-".join(fields) + '"'), HEAP, False, False, llvm=True)
    verify_runtime(identity)
    print(f"PASS {total} native pthread initialization controls, bits={bits}", flush=True)


if __name__ == "__main__":
    main()
