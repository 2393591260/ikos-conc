# Results and witness evidence

[Concurrent analysis documentation](../../CONCURRENCY.md)

## Results and evidence

| Result | Meaning |
| --- | --- |
| **SAFE / TRUE** | Abstract coverage and conservative proofs exclude all conflicts under the supported contract. Missing accesses or incomplete effects cannot justify SAFE. |
| **Confirmed race / FALSE** | An encoded finite execution reaches conflicting accesses with the required values, objects, thread instances and synchronization. Direct `ikos` prints `definitely UNSAFE` in the recommended mode. |
| **Potential race / UNKNOWN** | The analysis is inconclusive: an abstract candidate, unsupported effect, bounded UNSAT, truncation or solver unknown proves neither safety nor a race. |
| **ERROR** | A nonzero exit, a terminating signal, or missing legal result output indicates execution failure. BenchExec handles external resource termination separately. |

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
