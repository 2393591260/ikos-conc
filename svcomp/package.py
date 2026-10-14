#!/usr/bin/env python3
"""Package the IKOS install tree into a self-contained SV-COMP tool archive.

Turns `install/` into `ikos-conc.zip` with a single top-level directory
`ikos-conc/`, suitable for the SV-COMP Zenodo archive requirement:

  * relocatable  — settings.py PREFIX and the wrapper scripts derive all
                   paths from their own location (unpack anywhere)
  * self-contained — APRON (install/lib) and clang/opt + LLVM-14 shared libs
                   (install/llvm) are bundled; the wrappers export the right
                   LD_LIBRARY_PATH / PYTHONPATH
  * offline       — no network access needed at run time
  * smoketest.sh  — runs a built-in example, exit 0 on success

Run after `make install` (or `cmake --build build --target install`).

Usage:
    python3 svcomp/package.py [--install DIR] [--out FILE] \
        [--llvm /usr/lib/llvm-14] [--apron /usr/local/lib]
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import zipfile
from datetime import datetime, timezone

EXAMPLE_C = r"""// Built-in smoke-test example: a data race between two threads on `x`.
#include <pthread.h>
int x = 0;
void *t(void *arg) { x = 1; return 0; }
int main(void) {
  pthread_t th;
  pthread_create(&th, 0, t, 0);
  x = 2;             /* races with t's write to x */
  pthread_join(th, 0);
  return 0;
}
"""

SAFE_EXAMPLE_C = r"""// Both accesses to x are protected by the same mutex.
#include <pthread.h>
int x = 0;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
void *t(void *arg) {
  pthread_mutex_lock(&lock);
  x = 1;
  pthread_mutex_unlock(&lock);
  return 0;
}
int main(void) {
  pthread_t th;
  pthread_create(&th, 0, t, 0);
  pthread_mutex_lock(&lock);
  x = 2;
  pthread_mutex_unlock(&lock);
  pthread_join(th, 0);
  return 0;
}
"""

SMOKETEST = r"""#!/bin/sh
# Exercise the competition wrapper, verdicts and witness output in both models.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
trap 'exit 1' HUP INT TERM
cd "$WORK"
"$DIR/bin/svcomp_witness.py" --version
printf 'CHECK(init(main()), LTL(G ! data-race))\n' > "$WORK/no-data-race.prp"
for model in ILP32 LP64; do
  if ! output=$("$DIR/bin/svcomp_witness.py" \
    --property "$WORK/no-data-race.prp" --data-model "$model" \
    --out "$WORK/race-$model.yml" "$DIR/examples/smoke.c" 2>&1); then
    printf '%s\n' "$output"
    exit 1
  fi
  printf '%s\n' "$output"
  case "$output" in
    *"The program is definitely UNSAFE"*) ;;
    *) exit 1 ;;
  esac
  python3 - "$WORK/race-$model.yml" "$model" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as f:
    witness = json.load(f)
assert witness[0]["entry_type"] == "violation_sequence"
assert witness[0]["metadata"]["format_version"] == "2.2"
assert witness[0]["metadata"]["task"]["data_model"] == sys.argv[2]
PY
  if ! output=$("$DIR/bin/svcomp_witness.py" \
    --property "$WORK/no-data-race.prp" --data-model "$model" \
    --out "$WORK/safe-$model.yml" "$DIR/examples/safe.c" 2>&1); then
    printf '%s\n' "$output"
    exit 1
  fi
  printf '%s\n' "$output"
  case "$output" in
    *"The program is SAFE"*) ;;
    *) exit 1 ;;
  esac
  test ! -e "$WORK/safe-$model.yml"
done
"""

README = r"""# IKOS-ConC — Concurrent Data-Race Detection

IKOS-ConC is a static analyzer for detecting data races in concurrent C
programs, built on the NASA IKOS framework. It participates in the SV-COMP
`C.no-data-race` category.

The abstract analysis reports potential races. The competition entry point
demotes these warnings to UNKNOWN and uses bounded execution/value/address/
synchronization constraints to confirm supported finite race executions.
Unsupported confirmation cases stay UNKNOWN. Finite regression results are
not a proof of whole-tool soundness.

## Usage

Run the entry point on a C `.c` or preprocessed `.i` input:

    ./bin/svcomp_witness.py --data-model LP64 --out witness.yml input.c

Use ILP32 or LP64 to match the input. The wrapper defaults to ILP32, while the
underlying analyzer defaults to LP64; always specify the model. It locates
`ikos` next to itself, enables demote plus BMC, and emits witness format 2.2
only for a confirmed race. The requested old witness is removed before analysis;
use a distinct output path per task. It also supports `--version`.
For competition tasks, pass `--property FILE.prp`; only the property
`CHECK(init(main()), LTL(G ! data-race))` is accepted.

Outputs: `The program is SAFE` (safety under the implemented contract),
`The program is definitely UNSAFE` (bounded confirmation), or
`The program is UNKNOWN` / `potentially UNSAFE` (inconclusive).
A nonzero exit or missing legal verdict is an execution failure.
An exported witness is not automatically independently validated.

For an analyzer report without witness export:

    IKOS_BMC_RECOVER=1 ./bin/ikos --analyses=race --concurrency=auto --demote-race-to-unknown -m 64 -o output.db input.c

For abstract safety analysis only, remove IKOS_BMC_RECOVER and retain the demote
option. Setting IKOS_BMC_RECOVER=0 still enables it. Unresolved abstract candidates
and bounded UNSAT cannot prove a race or unbounded safety respectively.
Internal search/SMT/structural limits are separate from external resource limits.

Runtime: the archive targets Linux x86-64 with the declared Ubuntu 24.04 system
packages and Python. LLVM/abstract-domain libraries are bundled; the archive does
not promise a native Windows/macOS installation or independence of system libraries.
User installation and troubleshooting are maintained in the source repository's
README.md, doc/CONCURRENCY.md, and TROUBLESHOOTING.md.

## SV-COMP integration

BenchExec tool-info module: `benchexec/tools/ikos-conc.py` (tool id `ikos-conc`).
The tool fixes the analysis to `--analyses=race --concurrency=auto
--demote-race-to-unknown` + the bounded re-confirmation (`IKOS_BMC_RECOVER=1`),
and emits TRUE (SAFE) / FALSE (confirmed race) / UNKNOWN. On a confirmed race,
the wrapper writes witness.yml in YAML-compatible JSON, format 2.2.
Run ./smoketest.sh to check the wrapper on bundled race and safe examples in
ILP32 and LP64. The smoke check does not perform independent witness validation.

## Source and build identity

Source repository: https://github.com/2393591260/ikos-conc
BUILD-INFO.json records the source revision, local modifications and binary
hashes of this archive. Uncommitted source changes must also be made publicly
available by the submitter to reproduce the recorded build.
MODIFICATIONS.txt identifies the concurrency extension to the original NASA
IKOS software. LICENSE.txt contains the NASA Open Source Agreement 1.3.
Third-party notices for bundled components are in licenses/.

## Layout

    bin/        frontend + analyzer binaries
    libexec/    bundled Python frontend (site-packages)
    lib/        APRON abstract-domain libraries
    llvm/       clang + opt + LLVM shared libraries (LLVM 14)
    include/    C/C++ headers
    examples/   smoke-test example
    smoketest.sh  self-check (exit 0 on success)
"""

WRAPPER = r"""#!/bin/sh
# Relocatable wrapper: expose bundled APRON/LLVM libs + ikos site-packages,
# then run the frontend with the system Python.
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export LD_LIBRARY_PATH="$DIR/../lib:$DIR/../llvm/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PYTHONPATH="$DIR/../libexec/lib/python3.12/site-packages${PYTHONPATH:+:$PYTHONPATH}"
exec python3 "$DIR/{script}" "$@"
"""

SCRIPTS = {
    "ikos": "ikos-frontend.py",
    "ikos-config": "ikos-config.py",
    "ikos-report": "ikos-report.py",
    "ikos-scan": "ikos-scan.py",
    "ikos-scan-c++": "ikos-scan-c++.py",
    "ikos-scan-cc": "ikos-scan-cc.py",
    "ikos-scan-extract": "ikos-scan-extract.py",
    "ikos-view": "ikos-view.py",
}


def patch_settings(py_path):
    """Make PREFIX and the LLVM/clang paths relocatable in the installed settings."""
    import re
    with open(py_path) as f:
        src = f.read()
    if "PREFIX = os.path.abspath(" in src:
        return  # already relocatable (idempotent re-run / re-package)
    new_prefix = (
        "PREFIX = os.path.abspath(\n"
        "    os.path.join(os.path.dirname(os.path.abspath(__file__)),\n"
        "                 *([os.pardir] * 6)))"
    )
    src, n = re.subn(r"^PREFIX = '[^']*'$", new_prefix, src, flags=re.M)
    assert n == 1, "settings.py PREFIX line not found"
    repl = [
        ("LLVM_PREFIX = '/usr/lib/llvm-14'", "LLVM_PREFIX = os.path.join(PREFIX, 'llvm')"),
        ("LLVM_BIN_DIR = '/usr/lib/llvm-14/bin'", "LLVM_BIN_DIR = os.path.join(PREFIX, 'llvm', 'bin')"),
        ("LLVM_INCLUDE_DIR = '/usr/lib/llvm-14/include'", "LLVM_INCLUDE_DIR = os.path.join(PREFIX, 'llvm', 'include')"),
        ("LLVM_LIB_DIR = '/usr/lib/llvm-14/lib'", "LLVM_LIB_DIR = os.path.join(PREFIX, 'llvm', 'lib')"),
        ("CLANG = '/usr/lib/llvm-14/bin/clang'", "CLANG = os.path.join(PREFIX, 'llvm', 'bin', 'clang')"),
        ("CLANGXX = '/usr/lib/llvm-14/bin/clang++'", "CLANGXX = os.path.join(PREFIX, 'llvm', 'bin', 'clang++')"),
    ]
    for old, new in repl:
        assert old in src, f"settings.py missing pattern: {old[:40]}..."
        src = src.replace(old, new)
    with open(py_path, "w") as f:
        f.write(src)


def package(install_dir, out, llvm_root, apron_dir):
    install = os.path.abspath(install_dir)
    llvm = os.path.abspath(llvm_root)
    apron = os.path.abspath(apron_dir)

    # 1. bundle APRON shared libs
    for lib in ["libapron.so", "libboxMPQ.so", "liboctMPQ.so",
                "libpolkaMPQ.so", "libap_ppl.so", "libap_pkgrid.so"]:
        shutil.copy(os.path.join(apron, lib), os.path.join(install, "lib", lib))

    # 1b. bundle Z3 (the BMC's SMT solver). ikos-analyzer links libz3.so.4; it
    # is a system lib (not statically linked), so ship the exact version the
    # binary was built against — ubuntu:24.04's libz3-4 may differ. The wrapper
    # already puts install/lib first on LD_LIBRARY_PATH.
    for z3 in ["/lib/x86_64-linux-gnu/libz3.so.4",
               "/usr/lib/x86_64-linux-gnu/libz3.so.4"]:
        if os.path.isfile(z3):
            shutil.copy(z3, os.path.join(install, "lib", "libz3.so.4"))
            break

    # 2. bundle clang + opt + LLVM shared libs
    os.makedirs(os.path.join(install, "llvm", "bin"), exist_ok=True)
    os.makedirs(os.path.join(install, "llvm", "lib"), exist_ok=True)
    for tool in ["clang", "opt"]:
        shutil.copy(os.path.join(llvm, "bin", tool),
                    os.path.join(install, "llvm", "bin", tool))
    for lib in ["libclang-cpp.so.14"]:
        shutil.copy(os.path.join(llvm, "lib", lib),
                    os.path.join(install, "llvm", "lib", lib))
    shutil.copy("/usr/lib/x86_64-linux-gnu/libLLVM-14.so.1",
                os.path.join(install, "llvm", "lib", "libLLVM-14.so.1"))
    # clang 内建头文件（stddef.h / stdarg.h / float.h / ...）：编译 C 必需。
    # 不打包的话，干净容器（无 gcc）里 clang 找不到 stddef.h（模拟考官环境
    # 踩到的坑：仅装 required_ubuntu_packages + gcc 也不够，因为 clang 只认自己
    # 资源目录里的 stddef.h）。目录版本是 14.0.6（非 14）。
    shutil.copytree(os.path.join(llvm, "lib", "clang"),
                    os.path.join(install, "llvm", "lib", "clang"),
                    dirs_exist_ok=True)

    # 3. relocatable settings
    patch_settings(os.path.join(
        install, "libexec", "lib", "python3.12", "site-packages",
        "ikos", "settings", "__init__.py"))

    # 4. drop the venv Python (wrappers use the system python3)
    for p in [os.path.join(install, "libexec", "bin"),
              os.path.join(install, "libexec", "pyvenv.cfg"),
              os.path.join(install, "libexec", "lib64")]:
        if os.path.islink(p):
            os.remove(p)
        elif os.path.isdir(p):
            shutil.rmtree(p)
        elif os.path.isfile(p):
            os.remove(p)

    # 5. relocatable wrappers
    for name, script in SCRIPTS.items():
        with open(os.path.join(install, "bin", name), "w") as f:
            # .replace, not .format: the shell `${VAR:+...}` expansions in
            # WRAPPER contain braces that str.format would try to parse.
            f.write(WRAPPER.replace("{script}", script))
        os.chmod(os.path.join(install, "bin", name), 0o755)

    # 6. smoketest + example + README + LICENSE
    os.makedirs(os.path.join(install, "examples"), exist_ok=True)
    with open(os.path.join(install, "examples", "smoke.c"), "w") as f:
        f.write(EXAMPLE_C)
    with open(os.path.join(install, "examples", "safe.c"), "w") as f:
        f.write(SAFE_EXAMPLE_C)
    with open(os.path.join(install, "smoketest.sh"), "w") as f:
        f.write(SMOKETEST)
    os.chmod(os.path.join(install, "smoketest.sh"), 0o755)
    with open(os.path.join(install, "README.md"), "w") as f:
        f.write(README)
    if not os.path.exists(os.path.join(install, "LICENSE.txt")):
        shutil.copy("LICENSE.txt", os.path.join(install, "LICENSE.txt"))
    # Bundle the SV-COMP entry point (verdict forwarding + witness generation)
    # next to `ikos`; BenchExec runs this entry directly using its shebang.
    shutil.copy("svcomp_witness.py", os.path.join(install, "bin", "svcomp_witness.py"))
    os.chmod(os.path.join(install, "bin", "svcomp_witness.py"), 0o755)

    # Record the concrete binaries; HEAD alone cannot identify a dirty build.
    def git_output(*args):
        return subprocess.check_output(["git", *args], text=True).strip()

    runtime_files = [
        "bin/ikos-analyzer", "bin/ikos-pp", "bin/svcomp_witness.py",
        "llvm/bin/clang", "llvm/bin/opt",
        "llvm/lib/libLLVM-14.so.1", "llvm/lib/libclang-cpp.so.14",
    ]
    runtime_files.extend("bin/" + name for name in SCRIPTS)
    runtime_files.extend("bin/" + script for script in SCRIPTS.values())
    for root, _, files in os.walk(os.path.join(install, "libexec")):
        runtime_files.extend(os.path.relpath(os.path.join(root, name), install)
                             for name in files if name.endswith(".py"))
    runtime_files.extend("lib/" + name for name in os.listdir(os.path.join(install, "lib"))
                         if ".so" in name)
    build_info = {
        "created_at": datetime.now(timezone.utc).isoformat(),
        "repository": "https://github.com/2393591260/ikos-conc",
        "git_head": git_output("rev-parse", "HEAD"),
        "working_tree_status": git_output("status", "--short", "--untracked-files=no"),
        "working_tree_status_scope": "tracked files; unrelated untracked workspace files are not included",
        "sha256": {},
        "integration_sha256": {},
    }
    for name in sorted(set(runtime_files)):
        with open(os.path.join(install, name), "rb") as f:
            build_info["sha256"][name] = hashlib.sha256(f.read()).hexdigest()
    for name in ["svcomp/fm-tools-entry.yml", "benchexec/tools/ikos-conc.py", "svcomp/package.py"]:
        with open(name, "rb") as f:
            build_info["integration_sha256"][name] = hashlib.sha256(f.read()).hexdigest()
    with open(os.path.join(install, "BUILD-INFO.json"), "w") as f:
        json.dump(build_info, f, indent=2)
        f.write("\n")
    with open(os.path.join(install, "MODIFICATIONS.txt"), "w") as f:
        f.write(
            "IKOS-ConC modifications, maintained by Ronghui Ruan, "
            + build_info["created_at"][:10] + "\n"
            "This software is derived from original IKOS software provided by "
            "NASA and is characterized as a Modification.\n"
            "The extension adds concurrent thread/memory analysis, race checking, "
            "bounded confirmation and SV-COMP integration.\n"
            "See BUILD-INFO.json and the source repository for build identity.\n"
        )

    notice_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              "third-party-licenses")
    shutil.copytree(notice_dir, os.path.join(install, "licenses"),
                    dirs_exist_ok=True)

    # 7. zip (single top-level dir, executable bit preserved)
    # Exclude Python bytecode caches (__pycache__/*.pyc) and stray result
    # databases (*.db) — the submission rules forbid "unnecessary data"
    # (auxiliary folders like __pycache__); they are regenerable / scratch.
    # Exclude the installed C++ source headers (include/ikos/{ar,core,frontend})
    # — only include/ikos/analyzer/intrinsic.h is referenced at run time by the
    # frontend's `-isystem` (analyzer.py), the rest is `make install` dev output.
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for root, dirs, files in os.walk(install):
            dirs[:] = sorted(d for d in dirs if d != "__pycache__" and d != ".git")
            files.sort()
            for name in files:
                if name.endswith((".pyc", ".db")):
                    continue
                full = os.path.join(root, name)
                rel = os.path.relpath(full, install)
                if rel.startswith(("include/ikos/ar/", "include/ikos/core/",
                                   "include/ikos/frontend/")):
                    continue
                arc = os.path.join("ikos-conc", rel)
                info = zipfile.ZipInfo.from_file(full, arc)
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = (os.stat(full).st_mode & 0xFFFF) << 16
                with open(full, "rb") as fh:
                    z.writestr(info, fh.read())

    print(f"wrote {out} ({os.path.getsize(out) // (1024*1024)} MB)")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--install", default="install")
    ap.add_argument("--out", default="ikos-conc.zip")
    ap.add_argument("--llvm", default="/usr/lib/llvm-14")
    ap.add_argument("--apron", default="/usr/local/lib")
    args = ap.parse_args()
    package(args.install, args.out, args.llvm, args.apron)
