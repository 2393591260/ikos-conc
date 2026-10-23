# IKOS-ConC

IKOS-ConC checks concurrent C programs for data races using abstract
interpretation and bounded model checking with Z3. It extends
[NASA IKOS](https://github.com/NASA-SW-VnV/ikos) and targets the SV-COMP
`no-data-race` property.

## Install and run

Download **IKOS-ConC 1.0** from
[Zenodo — DOI 10.5281/zenodo.23560094](https://doi.org/10.5281/zenodo.23560094).
The archive targets **Linux x86-64 / Ubuntu 26.04**, including Ubuntu 26.04 under
WSL2, and accepts ILP32 and LP64 programs.

First install the [runtime dependencies](doc/concurrency/user-guide/running.md#install-the-release-archive),
then download `ikos-conc.zip` from the record and run:

```bash
unzip ikos-conc.zip
cd ikos-conc
./smoketest.sh
./bin/svcomp_witness.py --data-model LP64 --out witness.yml examples/smoke.c
```

Replace `examples/smoke.c` with your `.c` or `.i` file. Choose `ILP32` or `LP64`
to match the input, and use a separate witness path for each run. A confirmed
race produces a YAML violation witness in SV-Witnesses format 2.2.

Keep the unpacked directory intact: it includes LLVM/Clang, Z3, APRON and the
bundled runtime libraries. See the [installation guide](doc/concurrency/user-guide/running.md#install-the-release-archive)
for dependency details. The `concurrency` branch contains newer development work;
use the archive's `BUILD-INFO.json` to identify the published build.

## Results

| Result | Meaning |
| --- | --- |
| SAFE / TRUE | The analysis excludes races under its supported semantic contracts. |
| Confirmed race / FALSE | A supported finite race execution passes the entry's independent AR check. |
| UNKNOWN | The analysis is inconclusive, including unsupported behavior or exhausted search. |
| ERROR / nonzero exit | Analysis or witness export failed; inspect stderr. |

A smoke test or AR execution check does not establish independent source-witness
validation. Bounded UNSAT does not prove unbounded safety. See
[results and witnesses](doc/concurrency/user-guide/results-and-witnesses.md) and
[supported boundaries](doc/concurrency/user-guide/assumptions.md).

## Build from source

Follow the [Ubuntu/WSL2 build instructions](doc/concurrency/developer-guide/releasing.md#build-from-source).
They cover the required LLVM/Clang 14 and Z3 dependencies, configuration and
installation. A source build and the published archive have separate identities.

## Documentation

| Topic | Guide |
| --- | --- |
| Analysis modes and command-line options | [Running concurrent analysis](doc/concurrency/user-guide/running.md) |
| Time limits, diagnostics and local benchmarks | [Budgets and benchmarking](doc/concurrency/user-guide/benchmarking.md) |
| Installation errors and inconclusive results | [Troubleshooting](TROUBLESHOOTING.md) |
| Source-installation checks and regressions | [Testing](doc/concurrency/developer-guide/testing.md#check-a-source-installation) |
| Architecture, semantic contracts and developer guides | [Documentation index](doc/CONCURRENCY.md) |
| Source layout and upstream references | [Source overview](doc/OVERVIEW.md) |

## License and contributions

IKOS-ConC retains the NASA Open Source Agreement 1.3; see
[LICENSE.txt](LICENSE.txt). Release archives include third-party notices.
See [contributors](CONTRIBUTORS.md) and [coding standards](doc/CODING_STANDARDS.md).
Report issues in the [IKOS-ConC issue tracker](https://github.com/2393591260/ikos-conc/issues).
