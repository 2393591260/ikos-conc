# Building and releasing

[Documentation index](../../CONCURRENCY.md)

A source commit, an installed binary and an uploaded archive have separate
identities. Verifying one does not automatically verify the others.

## Build from source

Build the concurrency extension from this repository. The upstream
`brew install nasa-sw-vnv/core/ikos` package provides NASA IKOS.
The `concurrency` branch is a development branch; fix a reviewed commit when
reproducing results or distributing a build.

The developer build below uses Ubuntu 24.04 and LLVM 14; the packaged runtime
is separately checked on Ubuntu 26.04. Install the build and dual-ABI dependencies:

```bash
sudo apt-get update
sudo apt-get install --yes \
  build-essential cmake python3 python3-venv python3-pip \
  libgmp-dev libboost-filesystem-dev libboost-system-dev \
  libboost-thread-dev libboost-test-dev libsqlite3-dev libtbb-dev \
  libz-dev libedit-dev libz3-dev \
  llvm-14 llvm-14-dev llvm-14-tools clang-14 \
  libc6-dev-i386 lib32gcc-13-dev

git clone --branch concurrency https://github.com/2393591260/ikos-conc.git
cd ikos-conc
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PWD/install" \
  -DLLVM_CONFIG_EXECUTABLE=/usr/lib/llvm-14/bin/llvm-config \
  -DPYTHON_EXECUTABLE=/usr/bin/python3
cmake --build build -j2
cmake --install build
./install/bin/ikos --version
```

LLVM/Clang 14 and the Z3 development library are required. APRON is optional;
install it before configuring if you need its numerical domains. Domain and
build choices must match when comparing results with another build. Installation
creates the Python environment and installs the frontend; normal users do not
need to copy individual binaries or Python modules. Initial installation uses
Python packages from the network. `-j2` limits concurrent compiler memory use;
increase it only when the machine has enough memory.

Analyze a C file with abstract interpretation and bounded confirmation:

```bash
IKOS_BMC_RECOVER=1 ./install/bin/ikos \
  --analyses=race --concurrency=auto --demote-race-to-unknown \
  -m 64 -o output.db program.c
```

This command produces the analyzer report/database. To export a source witness
from a source checkout, use:

```bash
./install/bin/svcomp_witness.py \
  --data-model LP64 --out witness.yml program.c
```

## Build the candidate

Use the source-build instructions above.
The top-level project fixes all configurations to Release with assertions off,
including when reusing a cache previously configured for Debug. Both CI build
entries use this profile. Reconfiguring an existing build may require rebuilding
all objects; retain the accepted installation until that build succeeds.

For evaluation, check the actual CMake cache and compiler flags: a `BUILD_MODE`
label alone does not establish a Release build. Changes to shared headers must
rebuild every dependent object. Restoring patches with old timestamps can leave
incompatible objects in one executable.

Preserve an accepted install and freeze a separate candidate before evaluation.
Do not replace an entry used by an active benchmark run. Record the analyzer,
preprocessor, Python entry/modules and required libraries along with the inputs,
options and validator version.

## Verify the delivery path

Run the relevant regressions and complete paired acceptance for the final
candidate. Check the actual wrapper, result protocol and witness output in a
clean supported container using the declared dependencies. Exercise non-root,
read-only and relocated installations where the archive promises support.

Check the actual ZIP after extraction, including executable permissions,
entry-point links, bundled examples, Python modules, LLVM/Clang and shared
libraries. Match the binaries against `BUILD-INFO.json` and the added runtime
libraries against `BUNDLED-LIBRARIES.json`. The current package bundles Boost
Filesystem/Thread, PPL, oneTBB and its allocator, ICU 74 and libxml2 in addition
to LLVM, APRON and Z3. These files must survive packaging and relocation.

In a clean Ubuntu 26.04 environment with only the declared system packages,
check that dynamic dependencies resolve without missing libraries and that
bundled dependencies resolve inside the extracted tree. Run that archive's
`smoketest.sh` and relevant deployment checks in both ILP32 and LP64. A successful
run from the developer installation does not verify the ZIP.

Packaging is implemented in
[svcomp/package.py](../../../svcomp/package.py); its documentation and dependency
declarations must match the built archive.

Witness format checks, independent AR checks and external source-witness
validation must be recorded separately. A successful smoke test exercises a
small sample and does not replace full evaluation.

## Publish an identifiable version

Archive filenames, checksums, source revision and build information must identify
the tested runtime. A repository push does not update an external archive.
Registration metadata must point to the published artifact and use the accepted
competition-year configuration. For a routine archive update, change only the
DOI of the existing version in `svcomp/fm-tools-entry.yml` and the FM-Tools entry.
Keep the registration name (for example, `svcomp27`) and its matching
`tool_version`: this name can identify an archive reporting tool version 1.0.
Adjust other fields only when actual dependencies, interfaces or competition
requirements change.

Run `python3 svcomp/check-fm-entry.py --fm-tools /path/to/fm-tools` for the official
format, sorting, 2027 registration and LF checks. After uploading, read the actual
remote file and verify its bytes. Download the public ZIP and compare its hash
with the accepted archive, then inspect the official CI execution. Source push,
registration checks and CI smoke tests do not establish completion of a full
competition prerun or source-witness acceptance.

Use the competition's current submission rules for deadlines and registration;
the [benchmarking guide](../user-guide/benchmarking.md) describes the distinction
between local checks and official resource evaluation.

## Refactor acceptance profiles

Use a fixed accepted runtime for paired comparison. The development profile and
competition profile may have different wall, BMC, SMT and allocator-refinement
settings; pair each against its own baseline. The final archive check must run
the actual packaged entry through BenchExec in the declared Ubuntu container,
with the official CPU, memory and core limits. Preserve commands, XML, tool
revision, input hashes, result files and witness checks.

A new accepted source commit does not replace an already uploaded archive.
Prepare and verify a separate local candidate package, then publish it only as
a separate authorized action. Keep the previous accepted artifact available.
