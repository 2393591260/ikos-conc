# Testing and reproducible evidence

[Concurrent analysis documentation](../../CONCURRENCY.md)

<a id="validation-and-reproducibility"></a>
<details>
<summary>Validation and reproducibility</summary>

To save analyzer details, add `--format=json --report-file=report.json` to an
`ikos` command. The report contains per-check information and structured BMC
outcomes. `IKOS_BMC_DIAGNOSTICS=1` enables more detailed candidate diagnostics;
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

The original soundness regression cases and their controls are in
[review-repros](../../../docs/review-repros/). For an installed source checkout:

```bash
python3 docs/review-repros/bmc-2026-10-08/run.py --bits 32
python3 docs/review-repros/bmc-2026-10-08/run.py --bits 64
```

For a fixed external benchmark checkout:

```bash
IKOS_BMC_RECOVER=1 python3 svcomp_race_test.py /path/to/sv-benchmarks \
  --ikos "$PWD/install/bin/ikos" --demote --partitioning guards --jobs 4 --timeout 30
```

This measures the analyzer under a local wall limit, not the entire competition
wrapper or official CPU budget. Record source revision, binary/frontend hashes,
input revision, ABI, flags, environment, resource limits, and validator version.
Finite zero FP/FN observations do not prove soundness for all programs.

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

The BMC also supports a narrow GNU/Linux ILP32 semaphore prefix. It reuses the
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
field in one live allocation of the initial main frame, on GNU/Linux ILP32.
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
