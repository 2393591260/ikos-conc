# Results and witness evidence

[Concurrent analysis documentation](../../CONCURRENCY.md)

## Results and evidence

| Result | Meaning |
| --- | --- |
| **SAFE / TRUE** | Abstract coverage and conservative proofs exclude all conflicts under the supported contract. Missing accesses or incomplete effects cannot justify SAFE. |
| **Confirmed race / FALSE** | The registered coverage obligations pass the strict gate and an encoded finite execution reaches conflicting accesses with the required values, objects, thread instances and synchronization. Direct `ikos` prints `definitely UNSAFE`. |
| **Potential race / UNKNOWN** | The analysis is inconclusive: an abstract candidate, unsupported effect, bounded UNSAT, truncation or solver unknown proves neither safety nor a race. |
| **ERROR** | A nonzero exit, a terminating signal, missing legal result output, or contradictory result metadata indicates execution failure. BenchExec handles external resource termination separately. |

The default `strict-coverage-v1` policy blocks both SAFE and FALSE when a
registered thread/effect obligation remains unresolved, a required entry is
skipped, shared summaries remain dirty, or analysis/checking has not completed.
A surviving mixed atomic/non-atomic boundary also blocks both verdicts. Ordinary
MAY aliases and input uncertainty retain candidates without automatically
becoming coverage gaps. Bounded search failure remains UNKNOWN.

Readonly-heap and bucket-ownership coverage replacements retain the original
obligation and its exact contract identifier. They authorize safety reasoning
only: if candidates remain alongside such a replacement, the result is UNKNOWN.
An independent execution check cannot override this permission boundary.

SQLite settings store the startup `race_policy` and final `race_result`.
JSON exports both keys, including completion, coverage state (`Pending`,
`Blocked`, `Checked`), obligations, counts, verdict and search status. These
property-level fields determine the race verdict independently of display
filters and per-statement rows. `Checked` means that the registered inventory
passed; it does not prove that every C/POSIX semantic omission can be detected.
Mixed checks present other property errors separately from the race result.

Historical reports without this protocol remain readable as historical
evidence. The current competition and witness selectors reject them; they do
not inherit permission to publish FALSE under the new policy.

A witness records the reported race for another tool to check. Keep three kinds
of evidence distinct:

| Check | What it checks |
| --- | --- |
| Witness format | The exported file follows its schema. |
| Independent AR execution check | The competition wrapper checks its selected execution before emitting FALSE. Direct `ikos` results and other saved executions need separate checks. The checker retains frontend and library assumptions. |
| Independent source-level validation | A separate validator checks the witness against the source program. Finding a different race does not validate this witness. |

Exporting format 2.2 or passing an AR check does not by itself establish official
competition witness acceptance. See [Validation and reproducibility](../developer-guide/testing.md#validation-and-reproducibility)
for reports, offline checks and regression commands.
