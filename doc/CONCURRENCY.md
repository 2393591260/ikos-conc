# Concurrent data-race analysis

IKOS-ConC extends NASA IKOS to analyze data races in concurrent C programs.
It reuses the LLVM-to-AR frontend, numerical and memory domains, interprocedural
analysis, and reporting infrastructure. Its SV-COMP entry targets `no-data-race`.

## Analysis pipeline

1. Clang and the LLVM frontend translate the input into AR instructions, control
   flow, types, and byte layouts for the selected machine model.
2. Thread-modular abstract interpretation computes local invariants and shared
   summaries. Thread creation, joins, locks, and modeled library effects contribute
   to the concurrent state; changed summaries trigger further analysis.
3. The data-race checker records shared accesses and conservatively filters pairs
   using object, byte-range, thread-lifecycle, and synchronization information.
4. Remaining candidates can be checked by bounded path exploration and a Z3
   encoding of values, addresses, dynamic instances, and supported synchronization.
   Abstract facts provide candidate-specific information subject to their contracts.
5. Confirmed results carry execution evidence. The competition wrapper exports a
   violation witness in SV-Witnesses format 2.2 for a confirmed race.

`SAFE` requires abstract coverage and conservative exclusion of conflicts.
In competition mode, `definitely UNSAFE` denotes bounded confirmation of a race;
`UNKNOWN` denotes an inconclusive result. Unsupported semantics, exhausted bounds,
and bounded UNSAT do not establish unbounded safety. An abstract race warning by
itself is not a confirmed execution. Failed runs are classified separately.

## Build and run

Configure the build using the [source-build instructions](../README.md#build-from-source).
The bounded confirmation module also requires Z3. For an already configured build:

```bash
cmake --build build --target ikos-analyzer -j"$(nproc)"
cp build/analyzer/ikos-analyzer install/bin/ikos-analyzer
```

After Python frontend changes, synchronize the modified modules with the installed
Python package as well. `install/bin/ikos` runs the installed analyzer and modules.

```bash
# Abstract safety analysis with unresolved race warnings reported as UNKNOWN
env -u IKOS_BMC_RECOVER install/bin/ikos --analyses=race --concurrency=auto \
  --demote-race-to-unknown -m 32 input.c

# Abstract analysis followed by bounded confirmation
IKOS_BMC_RECOVER=1 install/bin/ikos --analyses=race --concurrency=auto \
  --demote-race-to-unknown -m 32 input.c
```

Use `-m 32` for ILP32 and `-m 64` for LP64. The presence of `IKOS_BMC_RECOVER`
enables recovery; setting it to `0` still enables it. Remove the variable to disable
recovery. Machine models must match the task definition.

For the packaged competition entry:

```bash
./bin/svcomp_witness.py --property no-data-race.prp \
  --data-model ILP32 input.c
```

The property file must specify `CHECK(init(main()), LTL(G ! data-race))`.
The wrapper locates `ikos` in the same directory and writes `witness.yml` for a
confirmed race. The [BenchExec adapter](../benchexec/tools/ikos-conc.py) forwards
the task's property and data model. It classifies abnormal exits or missing verdicts
as `ERROR`; explicit inconclusive verdicts from successful runs are `UNKNOWN`.
On failure, the competition wrapper writes error diagnostics to stderr and exits
nonzero without emitting an analysis verdict.
Registration metadata is in [fm-tools-entry.yml](../svcomp/fm-tools-entry.yml),
and the archive builder is [package.py](../svcomp/package.py).

## Code navigation

| Component | Source |
| --- | --- |
| Race records, candidate filters, confirmation dispatch | `analyzer/src/checker/data_race.cpp` |
| Thread-modular driver and shared-summary iteration | `analyzer/src/analysis/value/thread_modular.cpp` |
| Concurrent library transfers | `analyzer/include/ikos/analyzer/analysis/execution_engine/concurrent_semantics.hpp` |
| Shared abstract environment | `core/include/ikos/core/domain/concurrent_global_env.hpp` |
| Lock and thread facts | `core/include/ikos/core/domain/lockset/lockset_domain.hpp` |
| Bounded exploration, SMT, facts, and evidence | `analyzer/include/ikos/analyzer/analysis/bmc/` |
| Independent exported AR execution checks | `scripts/validate_bmc_execution.py` |
| Witness export | `svcomp_witness.py` |

## Validation and boundaries

The original soundness regression cases and their controls are retained in
[review-repros](../docs/review-repros/). They cover shared guard values, publication
before initialization, and overlapping access widths:

```bash
python3 docs/review-repros/bmc-2026-10-08/run.py --bits 32
python3 docs/review-repros/bmc-2026-10-08/run.py --bits 64
```

For a supplied benchmark checkout, the task runner reads YAML expectations and
machine models. Use a fixed revision and report timeouts and failures separately:

```bash
IKOS_BMC_RECOVER=1 python3 svcomp_race_test.py /path/to/sv-benchmarks \
  --demote --jobs 4 --timeout 30
```

Support depends on the implemented library, memory, frontend, and bounded-execution
contracts; this is not complete analysis of arbitrary C/POSIX or all weak-memory
behaviors. Independent AR checks validate selected exported executions under their
frontend and API assumptions. They do not certify every SMT model, every SAFE rule,
or acceptance of source-level witnesses by an official validator. Zero observed
benchmark errors are not a proof of soundness for all programs.
