# Extending concurrent analysis

[Documentation index](../../CONCURRENCY.md)

Start with a missing semantic fact and an example that needs it. A benchmark name
or expected verdict is never a semantic premise. Prefer extending the component
that owns the fact over adding another interpreter for the same instructions.

## Define the contract before implementation

State the property, machine widths, memory model and library assumptions. Explain
what the result represents and what its consumers may conclude:

| Result | Permitted use |
| --- | --- |
| MAY behavior | Include possible values, objects, effects or thread instances. |
| MUST fact | Use a guarantee that holds for every represented execution. |
| Pair exclusion | Exclude the specified access pair under its full premises. |
| Effect coverage | Account for the specified otherwise-unmodeled effect, including downstream dependencies. |
| Confirmed execution | Report a race only with supported, independently checked execution evidence. |

A range fact cannot discharge an unknown write. Excluding one pair cannot remove
a possible writer from interference or establish thread completion. A bounded
search without a counterexample cannot produce a safety proof.

Specify the fact's program point, object coordinate system, byte width, thread
root, calling context and dynamic-instance scope. Define its initial state,
transfer, join and loop behavior, and how unknown effects, escape, rebinding or
new shared summaries invalidate it. A final consumer needs applicable stable
facts; a transfer that reads an intermediate summary needs a conservative update
and a dependency that triggers reanalysis when that summary changes.

## Existing extension points

- Ordinary transfer functions and library effects:
  [concurrent_semantics.hpp](../../../analyzer/include/ikos/analyzer/analysis/execution_engine/concurrent_semantics.hpp).
- Shared MAY/MUST summaries and change tracking:
  [concurrent_global_env.hpp](../../../core/include/ikos/core/domain/concurrent_global_env.hpp).
- All-observation range premises and publication:
  [all_access_facts.hpp](../../../analyzer/include/ikos/analyzer/analysis/all_access_facts.hpp).
- Access records and final candidate processing:
  [data_race.hpp](../../../analyzer/include/ikos/analyzer/checker/data_race.hpp) and
  [data_race.cpp](../../../analyzer/src/checker/data_race.cpp).

The current candidate checker invokes several specialized `ordered` and
`excludes` methods. These methods have different contracts; they are not a
general permission to refine the numerical state. Consult the
[module index](../reference/module-contracts.md) before adding a consumer.

## Example: excluding nonoverlapping byte ranges

For a common object, suppose access A starts in `[lA,uA]` and accesses `wA`
bytes, and B starts in `[lB,uB]` and accesses `wB` bytes. If the intervals cover
all actual starts and `uA + wA <= lB`, the byte ranges cannot overlap. Unknown
base displacement, width or offset does not satisfy this proof.

Test disjoint fields, overlapping wide/narrow stores, aliases to the same field,
unknown offsets, and the relevant machine-width conversions. A test that only
checks the preferred safe example cannot establish the contract.

## Complete the change

Bind the result to its consumers and add controls that break its premises. Include
an interaction test when two analyses consume the same information. Check the
affected old regressions, budget-sensitive cases and final paired results using
the candidate's own binary and execution evidence. Follow the
[testing guide](testing.md); document remaining proof obligations in
[soundness.md](../reference/soundness.md).

## Choosing a permission and consumer

Register the mechanism in `checker/race_rules.hpp` with a distinct identity and
role. Implement its predicate in the responsible stage, preserving full object,
context, instance and byte premises. A pair adapter returns `PairDecision` and
uses `consume_pair_proof`; a complete replacement for a recorded effect returns
`CoverageDecision` and uses `consume_coverage_proof`. A library model describes
and records effects during collection through `consume_library_effect`.

The registry is a review aid and permission check, not an automatic plugin
scheduler. Keep the scan's short-circuit order explicit. Document real producer
dependencies, such as binary tokens consumed by arena reservation and once
initialization. Reusing a numerical bound does not grant another role's
permission; the consumer must retain its full semantic checks.

Preflight publications and whole-source certificate handles in `Context` are
const to consumers. Add a narrowly owned producer and explicit query accessor
when a new fact is needed. Do not write another module's certificate, inherit a
preflight's application memory, or share proof-generation decisions with the
independent execution validator.
