# Concurrent data-race analysis

Start with the [installation and first example](../README.md#install-and-run).
This guide explains how to use the concurrency extension and interpret its
results. Installation and failure diagnosis are maintained in the
[README](../README.md) and [troubleshooting guide](../TROUBLESHOOTING.md).

## Analysis modes

All commands below use the installed frontend, not a binary in the build tree.
Select `-m 32` for ILP32 or `-m 64` for LP64 to match the input's target.

### Abstract safety analysis

```bash
env -u IKOS_BMC_RECOVER ./install/bin/ikos \
  --analyses=race --concurrency=auto --demote-race-to-unknown \
  -m 64 -o output.db input.c
```

The abstract interpreter tries to prove safety. Unresolved race candidates and
coverage gaps remain inconclusive. It does not construct a concrete race.

### Abstract analysis with bounded confirmation

```bash
IKOS_BMC_RECOVER=1 ./install/bin/ikos \
  --analyses=race --concurrency=auto --demote-race-to-unknown \
  -m 64 -o output.db input.c
```

This is the recommended analyzer mode. The abstract interpreter first excludes
conflicts where its conservative proofs apply; BMC searches remaining candidates
for actual finite executions. The environment variable is enabled by presence:
`IKOS_BMC_RECOVER=0` still enables it. Remove it to disable confirmation.
The demote option is also required to enter the current confirmation path.
An abstract warning from another mode is not evidence of an actual race.
Without confirmation, unresolved candidates are reported as warnings rather
than definite races. This applies to the ordinary non-demote mode as well.

### Competition entry and source witnesses

In an unpacked archive:

```bash
./bin/svcomp_witness.py --data-model ILP32 --out witness.yml input.c
```

From a source checkout after installation:

```bash
./install/bin/svcomp_witness.py \
  --data-model ILP32 --out witness.yml input.c
```

The wrapper fixes the analysis to demote plus BMC, accepts `.c`/`.i` inputs,
and emits a witness in SV-Witnesses format 2.2 for a confirmed race. It defaults
to ILP32; direct `ikos` defaults to LP64, so specify the model explicitly.
Use `--property FILE.prp` for a competition task. The supported property is:

```text
CHECK(init(main()), LTL(G ! data-race))
```

Other properties are rejected. The wrapper locates `ikos` next to itself unless
`--ikos` is specified. It prints a confirmed verdict only after writing the
witness. Output files are per invocation: the requested old witness is removed
before analysis, including unsuccessful runs. Parallel tasks need distinct
witness paths and analyzer databases.

## Results and evidence

- **SAFE:** abstract coverage and conservative exclusion of all conflicts under
  the supported contract. Missing accesses or incomplete effects cannot justify SAFE.
- **Confirmed race:** an encoded finite execution reaches conflicting accesses
  with the required values, objects, instances, and synchronization. In the
  recommended mode it is printed as `definitely UNSAFE`.
- **Potential race / UNKNOWN:** inconclusive. Abstract candidates alone,
  bounded UNSAT, truncation, unsupported operations, and solver unknown do not
  establish either safety or a confirmed race.
- **Execution failure:** a nonzero exit, signal, or missing legal output is an
  error. BenchExec handles external resource termination separately.

To save analyzer details, add `--format=json --report-file=report.json` to an
`ikos` command. The report contains per-check information and structured BMC
outcomes. `IKOS_BMC_DIAGNOSTICS=1` enables more detailed candidate diagnostics;
these may be large. A group may have several unsuccessful search reasons.

Confirmed executions can be checked offline by
`scripts/validate_bmc_execution.py`. This interprets exported AR executions under
explicit frontend and library assumptions. A normal run does not automatically
receive that acceptance. Witness format validity, AR acceptance, and acceptance
by an independent source-level validator must be reported separately.
An independent tool finding another race does not validate this tool's witness.

## Supported boundaries

The current target is concurrent C data-race analysis. Support depends on the
implemented operation and library contracts; it is not a blanket guarantee for
all C/POSIX programs.

| Area | Current scope and boundary |
| --- | --- |
| Input and layout | C via LLVM/Clang 14 and AR; explicit ILP32/LP64; competition wrapper accepts `.c`/`.i` |
| Memory model | BMC models its supported sequentially consistent executions; complete C11 weak-memory behavior is not implemented |
| Threads | Modeled pthread creation/join and finite dynamic instances; arbitrary creator trees and dynamic handle structures remain limited |
| Synchronization | Supported mutex/atomic/other library contracts; pthread attributes, failures, and ownership must satisfy their modeled premises |
| Objects | Global and supported stack/heap/pointer sources, byte ranges and access widths; general heap graphs, escape and lifetime remain limited |
| Unknown effects | Conservative coverage boundaries or explicit import/execution errors; omitted effects do not prove SAFE |
| Other properties | Original IKOS analyses remain available; deadlock/reachability are not claimed as complete concurrency properties |

AI and BMC have different coverage responsibilities. A fact sufficient to exclude
all conflicts is not necessarily a concrete value source for a BMC execution.

## Resource limits

The local benchmark script defaults to 30 seconds of **wall time** per task.
BMC has an approximately 10-second cooperative search deadline, per-query SMT
limits, and independent path/event/loop bounds. The BMC deadline starts after
abstract analysis; it is not a hard limit on the entire program run. Current
internal limits do not have runtime CLI options. Increasing the benchmark
script's timeout does not increase these internal bounds.

These are engineering defaults, not competition requirements. Competition
acceptance must use the actual BenchExec CPU, memory, core, and task configuration.
Record both the resource configuration and tool identity when comparing results.

## Architecture and code navigation

The pipeline is C/LLVM → AR → thread-modular abstract interpretation → access
records and conservative conflict filtering → candidate-driven BMC → result
and execution evidence. Shared-summary changes trigger thread reanalysis;
bounded search is used to confirm races, not to prove unbounded safety.

| Component | Source |
| --- | --- |
| Concurrent worklist and shared-summary iteration | `analyzer/src/analysis/value/thread_modular.cpp` |
| Concurrent library transfers | `analyzer/include/ikos/analyzer/analysis/execution_engine/concurrent_semantics.hpp` |
| Shared environment and lock/thread facts | `core/include/ikos/core/domain/concurrent_global_env.hpp`, `lockset/lockset_domain.hpp` |
| Access records, conservative filters and dispatch | `analyzer/src/checker/data_race.cpp` |
| Bounded exploration, values, addresses, synchronization and evidence | `analyzer/include/ikos/analyzer/analysis/bmc/` |
| Independent exported AR execution checks | `scripts/validate_bmc_execution.py` |
| Source witness export and BenchExec interface | `svcomp_witness.py`, `benchexec/tools/ikos-conc.py` |

Original IKOS numerical analyses and general parameters are described in
[analyzer/README.md](../analyzer/README.md). Keep property queries separate from
reusable thread and memory semantics.

## Validation and reproducibility

The original soundness regression cases and their controls are in
[review-repros](../docs/review-repros/). For an installed source checkout:

```bash
python3 docs/review-repros/bmc-2026-10-08/run.py --bits 32
python3 docs/review-repros/bmc-2026-10-08/run.py --bits 64
```

For a fixed external benchmark checkout:

```bash
IKOS_BMC_RECOVER=1 python3 svcomp_race_test.py /path/to/sv-benchmarks \
  --ikos "$PWD/install/bin/ikos" --demote --jobs 4 --timeout 30
```

This measures the analyzer under a local wall limit, not the entire competition
wrapper or official CPU budget. Record source revision, binary/frontend hashes,
input revision, ABI, flags, environment, resource limits, and validator version.
Finite zero FP/FN observations do not prove soundness for all programs.

For FM-Tools registration changes, use the official checks through
`svcomp/check-fm-entry.py --fm-tools /path/to/fm-tools`. It checks LF, formatting,
sorting, and 2027 registration rules. Verify uploaded bytes and archive behavior
separately; a local pass is not an official CI or witness acceptance.
