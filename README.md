# IKOS-ConC

IKOS-ConC analyzes data races in concurrent C programs. It extends NASA IKOS
with thread-modular abstract interpretation and bounded SMT confirmation of
remaining race candidates. Its competition entry targets `no-data-race`.

- [Install and run](#install-and-run): start here.
- [Concurrent analysis](doc/CONCURRENCY.md): modes, verdicts, supported boundaries,
  witnesses, and architecture.
- [Troubleshooting](TROUBLESHOOTING.md): installation errors, inconclusive results,
  and reproducible bug reports.

## Install and run

### Use the competition archive

The registered archive is available at
[Zenodo, DOI 10.5281/zenodo.23122650](https://doi.org/10.5281/zenodo.23122650).
It targets **Linux x86-64 with Ubuntu 24.04 runtime packages**; Windows users can
use an Ubuntu WSL2 environment. This archive identifies its own source revision
and runtime hashes in `BUILD-INFO.json`. A newer development branch does not
update an already published archive.

On Ubuntu 24.04, install the archive's declared runtime dependencies:

```bash
sudo apt-get update
sudo apt-get install --yes \
  unzip python3 libc6-dev libc6-dev-i386 lib32gcc-13-dev \
  libgmp10 libgmpxx4ldbl libmpfr6 libppl14 \
  libboost-filesystem1.83.0 libboost-thread1.83.0 libtbb12 \
  libsqlite3-0 libtinfo6 libz1 libz3-4 libicu74 libxml2 libffi8 \
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
The archive bundles LLVM and abstract-domain libraries but still uses system
Python and runtime libraries. There is no separate published container image
promised by these instructions.

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

On Ubuntu 24.04, install the build and dual-ABI header dependencies:

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
