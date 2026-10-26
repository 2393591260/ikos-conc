# Testing and reproducible evidence

[Concurrent analysis documentation](../../CONCURRENCY.md)

## Check a source installation

After installation, developers can run the focused concurrency regressions:

```bash
./install/libexec/bin/python -m pip install benchexec
ctest --test-dir build -L concurrency --output-on-failure
```

These checks cover verdict modes, supported value/address semantics, exported
execution checks and witness/adapter failure controls. CTest includes explicit
native ILP32 and LP64 groups. Individual Python suites with an ABI selector
accept `--bits 32` or `--bits 64`, as documented below. These regressions are
separate from the full external benchmark sweep and official validation.

The standalone harness checks need no analyzer build:

```bash
python3 scripts/test_ikos_runtime.py
python3 scripts/test_svcomp_race_runner.py
python3 scripts/test_svcomp_package.py
python3 scripts/check_concurrency_docs.py
```

For same-invocation SQLite evidence and offline execution checks, see the
[local benchmark sweep](../user-guide/benchmarking.md#local-benchmark-sweep).

<a id="validation-and-reproducibility"></a>
<details>
<summary>Validation and reproducibility</summary>

To save analyzer details, add `--format=json --report-file=report.json` to an
`ikos` command. The report contains per-check information and structured BMC
outcomes and the property-level `race_policy` / `race_result` protocol.
`IKOS_BMC_DIAGNOSTICS=1` enables more detailed candidate diagnostics;
these may be large. A group may have several unsuccessful search reasons.

The competition entry `svcomp_witness.py` independently checks the selected
execution with `validate_bmc_execution.py` before writing a witness and emitting
FALSE. ACCEPT permits that result; UNSUPPORTED returns UNKNOWN without a witness;
REJECT, missing evidence, or a checker failure returns ERROR. The installed entry
and release archive include the checker. Direct `ikos` does not apply this gate.

Other saved executions can be checked offline with
[scripts/validate_bmc_execution.py](../../../scripts/validate_bmc_execution.py). The batch checker
[scripts/sweep_bmc_execution.py](../../../scripts/sweep_bmc_execution.py) requires nonempty selected evidence, ACCEPT for
every execution, and evidence for every non-SAFE task; unsupported or missing
evidence fails acceptance. These checks interpret exported AR under explicit
frontend and library assumptions. Witness format validity, AR acceptance, and
acceptance by an independent source-level validator must be reported separately.
An independent tool finding another race does not validate this tool's witness.

The batch checker can replay historical AR evidence, but separately marks
`strict_false_publishable`: an old AR ACCEPT does not grant current publication
permission. New evidence must retain the global protocol even when only selected
rows are compressed. SQLite export uses the candidate's own Python reporter.
Missing or contradictory new metadata is an error; normal blocked coverage is
UNKNOWN, and external time limits remain separate.
The competition entry preserves the final attempt's `race_result` in stderr
for task-level offline attribution after its temporary database is removed.

The original soundness regression cases and their controls are in
[review-repros](../../../docs/review-repros/). For an installed source checkout:

```bash
python3 docs/review-repros/bmc-2026-10-08/run.py --bits 32
python3 docs/review-repros/bmc-2026-10-08/run.py --bits 64
```

The [benchmarking guide](../user-guide/benchmarking.md#local-benchmark-sweep)
provides the local sweep command and evidence options. It measures the analyzer
under a local wall limit, rather than the entire competition wrapper or official
CPU budget. Record source revision, binary/frontend hashes, input revision, ABI,
flags, environment, resource limits, and validator version. Finite zero FP/FN
observations do not prove soundness for all programs.

The optional `--inline-allocators` setting is available in `ikos`, the benchmark
runner, and the competition wrapper. It expands direct calls to bounded,
straight-line allocation constructors, giving each expanded call its own LLVM
allocation site. The checked subset has a fixed positive allocation size,
non-overlapping field initialization, and an initialization store that preserves
every integer argument. It leaves ordinary helpers and source-definedness checks
in place. It is disabled by default and cannot be combined with `--inline-all`.
This frontend transformation does not itself prove freshness, non-nullness,
thread completion, or race freedom; subsequent analyses retain those duties.

For the benchmark runner and competition wrapper, `--try-inline-allocators`
instead keeps the original conclusive result and retries only a valid UNKNOWN.
It first requests a read-only frontend inventory and skips the retry when no
eligible direct constructor call exists. The inventory reuses the inlining
predicate, does not change the LLVM module, and grants no safety or race proof.
Missing or malformed inventory for UNKNOWN is an execution error.
The second analysis uses its own report and must pass the same checks. Missing
reports, crashes, and inconsistent verdicts remain errors. This mode avoids
replacing existing proofs when a different LLVM representation loses precision.
The runner records both attempts. Its wall timeout and BMC limits apply to each
attempt (at most two); an external BenchExec CPU limit still covers the whole
tool invocation. Both refinement and forced inlining are disabled by default.

The BMC also supports a narrow GNU/Linux x86 ILP32 and x86-64 LP64 semaphore
prefix. It reuses the
closed library-effect proof, requires a real initialization with a constant
permit count from zero through the GNU semaphore maximum,
and retains actual main-thread posts before starting the selected workers.
Every worker wait before the two ordinary targets must fit those existing
credits; worker posts before the targets, used API results, atomic targets and
interleaved main/completion traces remain unsupported. Replay executes the
semaphore calls, checks opaque storage and joins all preceding providers'
clocks on acquisition. This does not add a safety rule or a general semaphore
model; unsupported cases remain inconclusive.
Only initialization with one permit supplies a token to the separate SAFE
ownership proof. General library-effect coverage does not grant mutual exclusion.

Explicit mutex initialization in a selected BMC execution must occur in the
main prefix before thread creation. Reinitializing overlapping live storage,
including after an unlock, remains unsupported; an empty owner set does not
end a mutex lifetime. Distinct, disjoint mutex fields keep separate lifetimes.

Selected ordinary executions may also use an explicitly initialized mutex
field in one live allocation of the initial main frame, on GNU/Linux x86
ILP32 or x86-64 LP64. The native mutex leaf is respectively 24 bytes aligned
to 4 bytes or 40 bytes aligned to 8 bytes.
The encoder checks the actual LLVM-declared mutex subobject, its byte bounds
and alignment, and binds worker arguments to their actual creation and helper
invocations. The independent AR checker resolves the raw native layout and
replays initialization, ownership and object lifetime. A same-sized cast,
missing initialization, ordinary byte access overlapping the mutex, or a
returned helper frame cannot establish this lock identity. Disjoint payload
fields remain ordinary memory. Local rwlocks, heap locks and combinations with
condition variables, atomic targets or completion traces are outside this
extension; it adds no SAFE rule.

For FM-Tools registration changes, use the official checks through
`svcomp/check-fm-entry.py --fm-tools /path/to/fm-tools`. It checks LF, formatting,
sorting, and 2027 registration rules. Verify uploaded bytes and archive behavior
separately; a local pass is not an official CI or witness acceptance.

</details>

## Refactor permission and pipeline checks

`python3 scripts/test_native_abi.py` checks both native GNU/Linux data models
through the competition entry. `--bits 32` or `--bits 64` selects one model;
CTest registers `concurrency-native-abi-32` and `concurrency-native-abi-64`.
It compiles real target headers and layout assertions, checks ordinary mutex
SAFE and native-handle/64-bit-field FALSE, disables confirmation for an UNKNOWN
control, and checks compilation and invalid-model errors. FALSE must pass the
entry's independent execution check and retain the source hash and data model
in its witness. This is a baseline, not dual-ABI acceptance of every mechanism.

`python3 scripts/test_native_mechanisms.py` runs the dual-ABI capability and
refusal controls listed in [supported boundaries](../user-guide/assumptions.md#native-dual-abi-mechanism-controls).
`--bits 32` or `--bits 64` selects one ABI; both are registered in CTest.
The tests prohibit wrong SAFE/FALSE independently of their current expected
verdicts and independently check every confirmed AR execution. They intentionally
retain UNKNOWN controls for unsupported synchronization and numeric arguments.
A newly supported verdict requires reviewing the corresponding semantic contract
and updating the documented observation; it must not bypass the coverage gate.

Malloc helper regressions run on both ABIs with
`python3 scripts/test_malloc_helpers.py --bits 32` and `--bits 64`.
They cover preserved failure branches, zero and native-width sizes, direct
allocation failure, repeated-site failure preserving old bytes and lifetimes,
unsupported effects, actual successful executions and corrupted native allocation
evidence. Reallocation failure, NULL inputs, zero sizes and ignored results must
retain an unresolved obligation and UNKNOWN before any counterexample search.
CTest registers both models.

After installing the build, run:

```bash
ctest --test-dir build -L concurrency --output-on-failure
```
 `concurrency-race-rules` checks distinct permissions,
identities and the disabled legacy rule. `concurrency-all-access-facts` checks
MUST conjunction, missing observations and late-replay invalidation. The
end-to-end groups retain positive and negative controls for coverage, ownership,
publication, byte overlap, thread multiplicity and selected executions.
`concurrency-strict-coverage` checks common-lock safety, supported races,
uncovered effects/entries, global initialization, mixed atomic boundaries,
late destructor changes, no-checks, unanchored required entries, display filters,
mixed properties and SQLite/JSON agreement on native ILP32 and LP64, including
long-long data.
`concurrency-race-result` checks protocol corruption and safety-only permissions.

For a compact pair-exclusion summary, set `IKOS_RACE_RULE_REPORT` to a writable
file path. Each run appends a JSON line containing rule counts, access count and
remaining pairs. It does not export a formal certificate and does not change
which rules are enabled. Use ordinary reports and BMC diagnostics to inspect
remaining candidate targets and execution outcomes. Disable diagnostic I/O for
performance comparisons.

Protected field relations have two distinct diagnostic controls.
`IKOS_DISABLE_MUTEX_FIELD_EQUALITIES=1` suppresses initial equality templates;
`IKOS_DISABLE_MUTEX_FIELD_EQUALITY_IMPORT=1` suppresses only their import at
mutex acquisition. The latter retains template generation, release checks,
invalidation, and scalar value handoffs. Subsequent analysis can still produce
weaker release facts when the imported relation is unavailable. Neither control
disables coverage checks or grants a safety proof. With `--log=debug`, relation
messages distinguish initialization, release checks, import attempts, and
suppressed joint imports. The import control skips the whole joint snapshot
import block, including its fresh field variables, rather than only the equality
constraint. A successful release check alone does not establish that every writer
is covered. `scripts/test_protected_field_values.py --bits 32` and `--bits 64`
exercise both normal analysis and import-only positive and negative controls.


## Maintained cooperation controls

`concurrency-bmc-fact-switches` and its `-lp64` counterpart exercise the
production numeric-bound, no-wrap and single-execution consumers. Each control
requires an actual metric hit or its withdrawal under the corresponding switch;
numeric fact switches must preserve the AI candidate population. Confirmed
executions require nonempty strict AR acceptance.

`concurrency-pair-exclusion` and `-lp64` test native and solver-assisted
pair exclusion with their respective ablations. They run with BMC disabled:
a pair exclusion proves only the supported separation query and cannot authorize
a concrete counterexample. `concurrency-pair-exclusion-unit` independently
enumerates tiny address spaces to check the range/congruence abstraction.

`concurrency-results` compares selected execution models with detailed BMC
diagnostics disabled and enabled, and independently checks every exported
execution. The lightweight `ablate-bmc`, `reference-unknowns` and `witness-export`
groups check offline evidence classification and export contracts without running
an external source validator.

Maintained end-to-end runners select the installed runtime's own isolated Python
through `ikos_runtime.runtime_command`. This prevents a copied entry's absolute
shebang or an ambient Python path from silently selecting a different install.
Record the actual runtime before and after a frozen acceptance round.

## Local acceptance and remote CI

A development stage passes its relevant local controls, regression suites,
budget sentinels, frozen task comparison and selected-execution checks before
commit and push. Record the remote commit and CI status separately. A pending
remote run does not block the next locally accepted stage.

Every push runs the lightweight Quick checks workflow: documentation links,
runtime identity, benchmark runner and packaging contract tests. It does not
build the analyzer or repeat the benchmark sweep. Superseded quick runs are
cancelled automatically on the same branch.

The Build tool on Linux workflow remains available through GitHub Actions'
manual Run workflow entry. Run this clean build and complete concurrency
regression for build/dependency changes or a release candidate, and record the
tested commit. Ordinary mechanism stages do not require a full remote rerun.
Investigate concrete CI failures before treating the affected checks as passed;
neither local acceptance nor a successful push implies remote CI success.

### Independent finite BMC coverage comparison

`python3 scripts/test_bmc_finite_coverage.py --bits 32` and `--bits 64`
compare 84 fixed program/input instances with an independent exhaustive oracle.
The language has two workers, three global int objects, ordinary outer mutex
sections, immutable Boolean guards and one to three payload accesses per worker.
Its own vector-clock interpreter enumerates all 404 retained-event schedules;
a separate closed-form conflict calculation cross-checks the event-pair sets.

All 29 oracle-racy inputs must produce FALSE with nonempty strict AR evidence.
Actual source locations, static instruction identities, dynamic access ordinals
and objects must map back to oracle-racy pairs. Oracle-safe inputs may be SAFE
or UNKNOWN and must never be FALSE. Missing candidates, missing targets, wrong
ordinals/objects, errors and resource termination cannot pass as coverage.
Comparator self-checks inject these failures, including mutations of real
observations. This establishes the stated finite comparison, not completeness
for arbitrary C or a new SAFE rule. `--self-test` runs the oracle alone.

### Native thread-argument windows

`test_thread_argument_windows.py` runs both ABI modes with explicit
VarPack Polka and guard partitioning. Positive cases require actual native
creator/window queries and the final pair rule; independent producer and
consumer ablations must withdraw that proof. Default-domain, non-race,
checks-off and other partitioning modes must not prepare the helper.

The thread driver prepares one closed creation family before constructing
checkers. A shadow copies each native entry argument offset; existing native
transfers propagate the relation. The final consumer combines the observed
relative access windows with a proved positive creation stride in both
orientations. It requires actual create/step observations, matching family
and object, a clean shared board and completed analysis. It grants only pair
exclusion; pointer validity, effect coverage and overall SAFE remain separate.

Three native units check machine-width arithmetic/join/widen/forget/wrap,
full original-state projection after forgetting the shadow, and the actual
production consumer. The last unit includes a real shared-board write and a
fresh native query of its changed value: dirty first withdraws the old proof,
and the recomputed broader window continues to block it. A separate independent
round checks recovery; this does not claim automatic whole-worklist reconvergence.

### Timed main and provider execution controls

The test_bmc_startup_provider.py runner supports --bits 32 and --bits 64.
It exercises a bounded actual main timeline, zero or one completed provider,
and two selected workers. Creates, provider writes and Return, exact
handle-generation joins, main reads and guards, normal mutex operations and
read-from share the existing BMC encoding. A failed or unsupported search does
not grant SAFE.

Positive controls require the actual timeline and strict independent AR replay;
negative controls damage creation, join, completion, read-from and source
identity. The source witness mapper accepts the timed roles, keeps the selected
schedule, and reports UNKNOWN for its declared registration-order projection
limit. Corrupt source or execution evidence remains an error. The legacy
GraphML adapter explicitly rejects timed main/provider traces.
