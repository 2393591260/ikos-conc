# IKOS-ConC

IKOS-ConC analyzes data races in concurrent C programs. It extends NASA IKOS
with thread-modular abstract interpretation and bounded SMT confirmation of
remaining race candidates. Its competition entry targets `no-data-race`.

IKOS-ConC uses independent release numbering, starting at **1.0**. Earlier
archives reported inherited IKOS 3.x version numbers; use the entry point's
`--version` and `BUILD-INFO.json` to identify the actual archive.

- [Install and run](#install-and-run): start here.
- [Concurrent analysis](doc/CONCURRENCY.md): modes, verdicts, supported boundaries,
  witnesses, and architecture.
- [Troubleshooting](TROUBLESHOOTING.md): installation errors, inconclusive results,
  and reproducible bug reports.

## Install and run

### Use the competition archive

The registered archive is available at
[Zenodo, DOI 10.5281/zenodo.23122650](https://doi.org/10.5281/zenodo.23122650).
Packages built from this revision target **Linux x86-64 with Ubuntu 26.04 runtime
packages**, matching the SV-COMP 2027 environment. Windows users can use Ubuntu
26.04 under WSL2. The DOI may still serve an earlier build: check the packaged
README and `BUILD-INFO.json` for its runtime requirements, source revision and
binary hashes. A newer development branch does not update a published archive.

For the Ubuntu 26.04 package, install the declared runtime dependencies:

```bash
sudo apt-get update
sudo apt-get install --yes \
  unzip python3 libc6-dev libc6-dev-i386 lib32gcc-15-dev \
  libgmp10 libgmpxx4ldbl libmpfr6 \
  libsqlite3-0 libtinfo6 zlib1g libffi8 \
  libedit2 libbsd0 liblzma5 libmd0
```

Download `ikos-conc.zip` from the record, then unpack and check it:

```bash
unzip ikos-conc.zip
cd ikos-conc
./smoketest.sh
./bin/svcomp_witness.py --data-model LP64 --out witness.yml examples/smoke.c
```

The smoke test checks race and mutex-protected examples in ILP32 and LP64, using
the real entry point. It checks witness output, not independent witness validity.
New archives bundle LLVM, APRON, Z3, Boost Filesystem/Thread, PPL, oneTBB and
its allocator, plus the older ICU/libxml2 ABIs needed by LLVM 14. Check
`BUNDLED-LIBRARIES.json` for the added libraries' versions and hashes; PPL's
corresponding source and third-party licenses are included in `licenses/`.
Older archives may still require the four system packages `libppl14`,
`libboost-filesystem1.83.0`, `libboost-thread1.83.0` and `libtbb12`.
The archive still uses system Python and the declared runtime
libraries. There is no separate published container image
promised by these instructions.
For concurrent analysis, retain the underlying analyzer's default `--jobs=1`;
other analyzer worker counts are unsupported. Running multiple separate benchmark
tasks with the harness's `--jobs` option is independent of this restriction.
Native macOS is not a validated release target. Its exploratory build workflow
is manual-only and currently has an unresolved compilation failure. Automatic
build and concurrent-analysis acceptance target Linux.

To analyze your input, replace `examples/smoke.c` with its path. Choose ILP32 or
LP64 to match the program's target or task definition. The wrapper accepts C
`.c` and preprocessed `.i` files, confirms supported races, and writes the
requested witness only for a confirmed race. Use a different `--out` path for
each task; an old output at that path is removed before analysis.

### Build from source

Use this repository for the concurrency extension. The upstream command
`brew install nasa-sw-vnv/core/ikos` installs NASA IKOS, not IKOS-ConC.
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

## Checking a source installation

After installation, developers can run the focused concurrency regressions:

```bash
./install/libexec/bin/python -m pip install benchexec
ctest --test-dir build -L concurrency --output-on-failure
```

These checks default to ILP32 and cover verdict modes, supported value/address
semantics, exported execution checks and witness/adapter failure controls.
The relevant Python suites accept `--bits 64` for explicit LP64 checks.
They are separate from the full external benchmark sweep and official validation.

For a local benchmark sweep with evidence from the same analyzer invocations:

```bash
IKOS_BMC_RECOVER=1 python3 svcomp_race_test.py /path/to/sv-benchmarks \
  --ikos /path/to/install/bin/ikos --demote --jobs 4 --timeout 30 \
  --bmc-deadline-ms 20000 --bmc-solver-timeout-ms 1000 \
  --evidence-dir /path/outside/repository/new-run --evidence-source database
```

Use a new evidence directory for each run. The runner explicitly uses the
selected installation's Python and checks the actual modules, binaries and
resolved dependencies before and after the run. Keep that installation and its
dependencies unchanged during evaluation. Once all analyses finish, it checks
the saved FALSE executions offline and retains compressed evidence. Interrupted
runs retain partial results; timeouts terminate the analysis process group.
`complete` means the task scan finished; baseline pairing and source-witness
validation remain separate acceptance steps. Coverage includes UNKNOWN and
errors in the task population. Database capture preserves the normal text verdict
and extracts report rows with the selected installation's reporter after all
analyses finish. It hashes the same invocation's database and avoids exporting
the full statement/object tables during the timed run. The original capture mode
remains available with `--evidence-source json` and adds JSON reporting overhead.
Local wall times must not be compared directly with another reporting mode or
official CPU limits. Runtime/harness unit tests need no analyzer build:

```bash
python3 scripts/test_ikos_runtime.py
python3 scripts/test_svcomp_race_runner.py
```

## Understanding results

| Output | Meaning in the recommended confirmation mode |
| --- | --- |
| `The program is SAFE` | No race under the implemented abstract coverage and semantic contracts |
| `The program is definitely UNSAFE` | Bounded confirmation found a supported finite race execution |
| `The program is UNKNOWN` or `potentially UNSAFE` | Inconclusive; a potential race is not a confirmed counterexample |
| Nonzero exit, signal, or no legal verdict | Execution failure; do not interpret it as SAFE or a race |

A confirmed race and an independently validated witness are different evidence
levels. Bounded UNSAT, unsupported behavior, and exhausted search never establish
unbounded safety. This is not complete analysis of arbitrary C/POSIX or every
weak-memory behavior; see [supported boundaries](doc/CONCURRENCY.md#supported-boundaries).

## License and upstream

IKOS-ConC derives from [NASA IKOS](https://github.com/NASA-SW-VnV/ikos).
It retains the NASA Open Source Agreement 1.3; see [LICENSE.pdf](LICENSE.pdf).
Bundled archives also contain third-party notices. Report IKOS-ConC issues at
[this repository's issue tracker](https://github.com/2393591260/ikos-conc/issues).
The original numerical analyzer documentation remains in [analyzer/README.md](analyzer/README.md)
and [doc/OVERVIEW.md](doc/OVERVIEW.md).

Contributors
------------

See [CONTRIBUTORS.md](CONTRIBUTORS.md)

Publications
------------

* Sung Kook Kim, Arnaud J. Venet, Aditya V. Thakur. **Deterministic Parallel Fixpoint Computation.** In _Principles of Programming Languages (POPL 2020)_, New Orleans, Louisiana ([PDF](https://arxiv.org/pdf/1909.05951.pdf)).

* Guillaume Brat, Jorge Navas, Nija Shi and Arnaud Venet. **IKOS: a Framework for Static Analysis based on Abstract Interpretation.** In _Proceedings of the International Conference on Software Engineering and Formal Methods (SEFM 2014)_, Grenoble, France ([PDF](http://ti.arc.nasa.gov/publications/16610/download/)).

* Arnaud Venet. **The Gauge Domain: Scalable Analysis of Linear Inequality Invariants.** In _Proceedings of Computer Aided Verification (CAV 2012)_, Berkeley, California, USA 2012. Lecture Notes in Computer Science, pages 139-154, volume 7358, Springer 2012 ([PDF](http://ti.arc.nasa.gov/publications/4767/download/)).

Coding Standards
----------------

See [doc/CODING_STANDARDS.md](doc/CODING_STANDARDS.md)

Overview of the source code
---------------------------

See [doc/OVERVIEW.md](doc/OVERVIEW.md)
