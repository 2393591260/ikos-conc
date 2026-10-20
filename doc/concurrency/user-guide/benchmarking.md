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
