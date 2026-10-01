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
import os
import shutil
import subprocess
import zipfile

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

SMOKETEST = r"""#!/bin/sh
# Smoke test: the analyzer must run on a built-in example and produce a
# verdict. Exits 0 on success (the tool is functional), non-zero on failure.
# Covers both data models: the SV-COMP no-data-race category is entirely
# ILP32, so the -m32 run is the path the competition actually exercises (and
# needs the 32-bit headers declared in required_ubuntu_packages:
# libc6-dev-i386 + lib32gcc-13-dev).
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
"$DIR/bin/ikos" --version
"$DIR/bin/ikos" --analyses=race --concurrency=auto --format=no "$DIR/examples/smoke.c"
"$DIR/bin/ikos" -m 32 --analyses=race --concurrency=auto --format=no "$DIR/examples/smoke.c"
# SV-COMP scoring switch: --demote-race-to-unknown turns every abstract race
# into UNKNOWN, then IKOS_BMC_RECOVER=1 lets the bounded checker re-confirm the
# provable ones as "definitely UNSAFE" (FALSE). The smoke race is provable, so
# the shipped config must report "definitely UNSAFE" — sound (FN=0/FP=0) with
# recovered TRUE positives.
IKOS_BMC_RECOVER=1 "$DIR/bin/ikos" -m 32 --analyses=race --concurrency=auto \
  --demote-race-to-unknown --format=no "$DIR/examples/smoke.c" 2>&1 \
  | grep -q "definitely UNSAFE"
"""

README = r"""# IKOS-ConC — Concurrent Data-Race Detection

IKOS-ConC is a static analyzer for detecting data races in concurrent C
programs, built on the NASA IKOS framework. It participates in the SV-COMP
`C.no-data-race` category.

The analysis is sound with respect to data races (no missed races): it reports
every potential data race it can prove, at the cost of some false positives.

## Usage (standalone)

    ./bin/ikos --analyses=race --concurrency=auto <file.c>

Verdicts: `The program is SAFE` (no race), `The program is definitely UNSAFE`
(race found), `The program is UNKNOWN` (model boundary).

### Demoting races to UNKNOWN + bounded re-confirmation (SV-COMP scoring)

The abstract analysis cannot always separate a REAL race from a false positive
caused by unmodelled synchronization, and SV-COMP penalizes a wrong race
(-16) far more than it rewards a right one (+1). So the SV-COMP entry point
runs IKOS in two stages:

1. `--demote-race-to-unknown` — every race the abstract checker finds is
   reported as `potentially UNSAFE` (= UNKNOWN) instead of `definitely UNSAFE`
   (= FALSE). This is sound for FN=0: UNKNOWN is never SAFE.
2. A bounded model checker (enabled via `IKOS_BMC_RECOVER=1`) re-confirms the
   races it can PROVE by unrolling the two thread paths and checking the
   happens-before / alias encoding with SMT. A proven race is reported as
   `definitely UNSAFE` (= FALSE, +1); everything else stays UNKNOWN (0).

The re-confirmation is sound for the FALSE direction — it only claims a race
when the HB/alias encoding proves one — so the net result is FN=0 and FP=0
with a few dozen recovered TRUE positives. This is the scoring configuration
the entry point ships (a recovered race is claimed by the bounded checker, not
the imprecise abstract front end).

    ./bin/ikos --analyses=race --concurrency=auto --demote-race-to-unknown <file.c>   # + env IKOS_BMC_RECOVER=1

### Data model (32-bit / 64-bit)

SV-COMP tasks specify an `ILP32` (32-bit) or `LP64` (64-bit) data model; the
C type widths (`long`, pointers) differ between the two. Pass `-m 32` / `-m 64`
to select it (default is 64-bit):

    ./bin/ikos -m 32 --analyses=race --concurrency=auto <file.c>   # ILP32
    ./bin/ikos -m 64 --analyses=race --concurrency=auto <file.c>   # LP64

The SV-COMP entry point (`bin/svcomp_witness.py`) takes `--data-model ILP32|LP64`
and forwards it, recording the matching `data_model` in `witness.yml`.

## SV-COMP integration

BenchExec tool-info module: `benchexec/tools/ikos-conc.py` (tool id `ikos-conc`).
The tool fixes the analysis to `--analyses=race --concurrency=auto
--demote-race-to-unknown` + the bounded re-confirmation (`IKOS_BMC_RECOVER=1`),
and emits TRUE (SAFE) / FALSE (race, only when proven) / UNKNOWN. Because a
race is only ever claimed when the bounded checker proves it, the FALSE
direction is sound: no missed race (FN=0) and no false positive (FP=0).

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
    with open(os.path.join(install, "smoketest.sh"), "w") as f:
        f.write(SMOKETEST)
    os.chmod(os.path.join(install, "smoketest.sh"), 0o755)
    with open(os.path.join(install, "README.md"), "w") as f:
        f.write(README)
    if not os.path.exists(os.path.join(install, "LICENSE.txt")):
        shutil.copy("LICENSE.txt", os.path.join(install, "LICENSE.txt"))
    # Bundle the SV-COMP entry point (verdict forwarding + witness generation)
    # next to the `ikos` executable: the BenchExec tool-info module locates it
    # via Path(executable).parent / "svcomp_witness.py".
    shutil.copy("svcomp_witness.py", os.path.join(install, "bin", "svcomp_witness.py"))

    # 7. zip (single top-level dir, executable bit preserved)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for root, dirs, files in os.walk(install):
            dirs.sort()
            files.sort()
            for name in files:
                full = os.path.join(root, name)
                arc = os.path.join("ikos-conc", os.path.relpath(full, install))
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
