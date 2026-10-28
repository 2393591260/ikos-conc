# Budgets and benchmarking

[Concurrent analysis documentation](../../CONCURRENCY.md)

## Resource limits

| Limit | Default | Where it applies |
| --- | --- | --- |
| Local benchmark wall timeout | 30 s per task | The local benchmark script; independent of internal search limits. |
| BMC cooperative deadline | 20,000 ms | Confirmation search after abstract analysis; `--bmc-deadline-ms`. |
| SMT timeout | 1,000 ms per query direction | `--bmc-solver-timeout-ms`; positive values only. |
| Structural and proof limits | Mechanism-specific | Path, event, loop, state and other bounds; separate from time limits. |

These are engineering defaults, **not competition requirements**. Use the actual
BenchExec CPU, memory, core and task configuration for competition acceptance.
A zero BMC deadline disables confirmation; exhaustion cannot establish safety.
The cooperative deadline is not a hard timeout for the whole tool invocation.
Expand [Budget behavior and diagnostics](#budget-behavior-and-diagnostics) for details.

<a id="budget-behavior-and-diagnostics"></a>
<details>
<summary>Budget behavior and diagnostics</summary>

The local benchmark script defaults to 30 seconds of **wall time** per task.
BMC defaults to a 20-second cooperative search deadline and a 1-second SMT
timeout per race-query direction. The `ikos` and `ikos-analyzer` options
`--bmc-deadline-ms` and `--bmc-solver-timeout-ms` override these independently.
For example, `--bmc-deadline-ms=20000 --bmc-solver-timeout-ms=2000` changes only
those time limits. A zero deadline disables confirmation searches; unresolved
candidates remain UNKNOWN. The solver timeout must be positive: zero must not
silently request unlimited Z3 solving.

The competition wrapper `svcomp_witness.py` accepts these same two options and
forwards explicit values to IKOS. Omitting them preserves the analyzer defaults.

The deadline starts after abstract analysis and is checked cooperatively, so it
does not hard-limit the whole invocation. The legacy search checks the deadline
before each path-pair query; auxiliary completion queries also cap their solver
timeout by the remaining deadline. Path/event/loop bounds and initial candidate/
query tokens remain separate, unchanged limits. Some legacy retry passes reset
those tokens without resetting the deadline. Increasing the benchmark script's
wall timeout does not change any of these internal limits.

The effective time limits are saved in the analysis database. With
`IKOS_BMC_DIAGNOSTICS=1`, reports also record budget settings and observed
`search_deadline`, `candidate_budget`, `query_budget`, and `smt_timeout` reasons.
Several causes can coexist; exhaustion never establishes unbounded safety.

These are engineering defaults, not competition requirements. Competition
acceptance must use the actual BenchExec CPU, memory, core, and task configuration.
Record both the resource configuration and tool identity when comparing results.

Ordinary and auxiliary BMC queries pass the same file-level deadline to path
enumeration and encoding. Exhaustion remains inconclusive, and each solver
direction receives a timeout bounded by the remaining budget. This is cooperative cancellation;
abstract interpretation, indivisible internal steps and frontend work are not
a hard wall-time guarantee. The outer runner timeout remains separate.

</details>

## Local benchmark sweep

Run from a source checkout with an installed tool and a fixed benchmark checkout:

```bash
IKOS_BMC_RECOVER=1 python3 svcomp_race_test.py /path/to/sv-benchmarks \
  --ikos /path/to/install/bin/ikos --demote --partitioning guards --jobs 4 --timeout 30 \
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
official CPU limits.

## Focused cooperation diagnostics

The following environment variables are opt-in diagnostics and ablations. Their
presence enables them, including a value of `0`; clear inherited variables when
measuring the default configuration.

| Variable | Effect |
| --- | --- |
| `IKOS_RACE_PAIR_OBSERVATIONS=<path>` | Append frozen access-pair records, the first exclusion rule, owner attempt status, and three independent read-only predicates. |
| `IKOS_RACE_DISABLE_OWNER_CONSUMER=1` | Suppress only the final region-ownership exclusion, then try later rules and confirmation normally. Range, ownership, library-effect and coverage producers remain enabled. |
| `IKOS_BMC_DISABLE_SEMAPHORE_FACTS=1` | Withhold the semaphore-effect callback from BMC. Abstract transfer and effect coverage remain enabled; unsupported concrete operations cannot confirm a race. |

Pair records use AR function/block/statement coordinates and the complete calling
context. They must be bound externally to the input, ABI, frozen AR configuration,
binary and resource identity. Two access roles preserve repeated-thread self-pairs.
Cross-run alignment is supported only when producers and their access sequence are
unchanged. Points-to and numerical snapshots describe abstract accesses; numeric
lock and create/join identities are run-local and are not cross-run keys.

The independent overlap fields cover read/read, unique-thread and atomic/atomic
predicates only. Other modules retain their original first-hit evaluation order;
`not_attempted` is distinct from `not_proved`. Coverage can stop pair processing
before any observation is emitted, so an absent file does not establish zero
candidates. Use the result protocol and coverage records to interpret it.

Keep diagnostic runs separate from cost measurements. The output may be quadratic
in the number of accesses and deliberately fails on write errors. First-hit counts
are order-dependent, and a faster UNKNOWN after disconnecting a required fact is
a capability change, not an equivalent verification speedup. Run
`scripts/test_race_cooperation.py --ikos <install>/bin/ikos` for focused contracts.

### Local benchmark snapshots

The local race runner defaults to `~/sv-benchmarks-current`. Point it to a
verified snapshot of the upstream SV-Benchmarks repository's `main` branch.
Before each new acceptance stage, check the upstream revision and refresh the
pointer if needed; freeze the resolved checkout and task manifest for the whole
run. Select active `no-data-race` tasks from `c/Concurrency.set` and verify that
the runner's directory list still covers that set. Record the commit, task and
input hashes, data models and property identity. A task count alone does not
identify a benchmark set.

Pass a checkout explicitly when reproducing historical results. Updating the
current pointer does not change the identity of saved results or authorize
reusing old scores for changed inputs. Benchmark updates do not change analysis
budgets.
