Overview of the source code
===========================

The following illustrates the content of the root directory:

```
.
├── CMakeLists.txt
├── LICENSE.pdf
├── README.md
├── TROUBLESHOOTING.md
├── analyzer
├── ar
├── cmake
├── core
├── doc
├── frontend
├── benchexec
├── boundary-tests
├── scripts
├── svcomp
├── script
└── test
```


* [CMakeLists.txt](../CMakeLists.txt) is the root CMake file.

* [LICENSE.pdf](../LICENSE.pdf) contains the NOSA 1.3 license.

* [TROUBLESHOOTING.md](../TROUBLESHOOTING.md) contains solution for common issues with IKOS.

* [analyzer](../analyzer) contains the implementation of various analyses for specific defect detections. These analyses are implemented on the Abstract Representation and use the fixpoint iterator and abstract domains to perform analysis. More information can be found at [analyzer/README.md](../analyzer/README.md).

* [ar](../ar) contains the implementation of the Abstract Representation, a generic assembly language. More information can be found at [ar/README.md](../ar/README.md).

* [cmake](../cmake) contains CMake files to search for related software libraries.

* [core](../core) contains the implementation of the theory of Abstract Interpretation, which includes the abstract domains, the fixpoint iterator, and various algorithms that support the implementation. More information can be found at [core/README.md](../core/README.md).

* [README.md](../README.md#build-from-source) contains the maintained Linux/WSL2 installation instructions. [CONCURRENCY.md](CONCURRENCY.md) explains the concurrent-analysis architecture, verdicts and supported boundaries.

* [frontend/llvm](../frontend/llvm) contains the implementation of the translation from LLVM to AR. More information can be found at [frontend/llvm/README.md](../frontend/llvm/README.md).

* [script](../script) contains the formatting and static code-checking helpers.

* [scripts](../scripts) contains concurrency regression runners, execution checks and reference-tool comparison helpers.

* [svcomp](../svcomp) and [benchexec](../benchexec) contain packaging, registration and benchmark-tool integration.

* [test](../test), [boundary-tests](../boundary-tests), and [docs/review-repros](../docs/review-repros) contain concurrent regression inputs. Analyzer, frontend and core regression tests also live alongside their respective modules. Run the maintained installed concurrency checks with `ctest --test-dir build -L concurrency --output-on-failure` after installation.
