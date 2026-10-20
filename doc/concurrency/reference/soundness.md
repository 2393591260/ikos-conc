# Soundness obligations and evidence

[Documentation index](../../CONCURRENCY.md)

For the safety channel, soundness means: under explicitly stated semantics and
assumptions, a SAFE verdict implies absence of a data race. An inconclusive answer
is permitted. Finite zero-error benchmark results do not prove this statement
for all programs.

## Obligations from source to verdict

| ID | Required argument | Current evidence and remaining obligation |
| --- | --- | --- |
| L1 | Translation and library interpretation preserve the relevant source behavior. | Type/layout/callee checks and regressions exist; no complete C-to-LLVM-to-AR correspondence proof for all extensions. |
| L2 | Initial states, local transfers, joins and widening conservatively cover successors. | Existing IKOS machinery and local extension contracts; a whole-product proof of every added transfer remains open. |
| L3 | Shared interference covers all writers, instances and lifetime changes; changed dependencies trigger reanalysis. | Shared summaries, worklist, dirty tracking and final-replay safeguards; their joint correspondence to the concurrent semantics remains open. |
| L4 | Every actual conflicting pair is represented, including repeated instances and overlapping bytes. | Access records, extern effects and self-pairing controls; complete reachability and special-entry coverage remains open. |
| L5 | Each exclusion and coverage replacement is valid for every represented alternative. | Explicit restricted contracts and premise-breaking regressions; full proofs of all source passes and their transfer/coverage remain open. |
| L6 | Composition and reporting preserve unresolved obligations and reject stale facts. | Stable fact publication, result handling and wrapper checks; no general independent SAFE certificate checker. |

These are proof obligations, not six completed certifications. Correctness fixes
for creator/join identity, unknown-write premises and final-summary changes are
tested. They do not establish every related operation's soundness.

## Common proof structure

Define the concrete state and allowed steps, then explain the concretization of
each abstract state. Prove initial-state inclusion and one-step preservation for
local actions and permitted interference. Establish conservative joins and a
post-fixpoint that covers loops and shared dependencies.

Next show that every actual race maps to a candidate, and that each exclusion is
valid for all events represented by that candidate. Unresolved effects must
remain visible unless a complete alternative proof covers their dependencies.
Faithful reporting then prevents a SAFE verdict for an actual race.

The numerical and lock examples in
[extending-analysis.md](../developer-guide/extending-analysis.md) illustrate local
rules. Their premises—address containment, widths, lock identity, acquisition and
lifetime—need their own arguments.

## Different evidence has different scope

- A mathematical argument establishes its stated theorem under its premises.
- A code correspondence review checks that implementation operations match that
  argument; it is not a machine-checked implementation proof.
- Regressions and paired benchmarks establish observations for identified runs.
- Independent AR checking establishes acceptance of the selected exported
  execution under the checker's semantics, not absence of other races.
- Source-witness validation and witness schema validation are distinct checks.

Bounded UNSAT cannot prove an unbounded SAFE verdict. SMT used over conservative
abstract facts to prove a universal pair exclusion has a different obligation:
the facts must cover all relevant events and the formula must encode that claim.

## Theory references

- Cousot and Cousot, [Abstract Interpretation, POPL 1977](https://www.di.ens.fr/~cousot/COUSOTpapers/POPL77.shtml): conservative abstract semantics and fixpoint approximation.
- Miné, [Static Analysis of Run-Time Errors in Embedded Real-Time Parallel C Programs, LMCS 2012](https://lmcs.episciences.org/799/pdf): local and interference semantics; its restricted core language is not the whole IKOS-ConC input language.
- Cousot, Cousot and Mauborgne, [Reduced Products, FoSSaCS 2011](https://www.di.ens.fr/~cousot/publications.www/CousotCousotMauborgne-FoSSaCS11-LNCS6604-proofs.pdf): conditions under which combined refinements preserve concretization.
- Schwarz and Erhard, [The digest framework, STTT 2024](https://doi.org/10.1007/s10009-024-00773-y): sound base analysis plus admissible digests yields sound refinement.
- Schwarz and Erhard, [Data Race Detection by Digest-Driven Abstract Interpretation, VMCAI 2026](https://doi.org/10.1007/978-3-032-15700-3_15): access stability, compatibility and race exclusion under the paper's semantics.

These papers supply methods and conditional theorems. They do not certify
IKOS-ConC's added C++ implementation or arbitrary C11 weak-memory behavior.

## Composition argument for the refactored checker

Assume access collection conservatively covers the program under the documented
semantic model. Every enumerated pair is either retained or excluded by an
applicable pair contract. A retained pair keeps the analysis inconclusive unless
supported execution search confirms a concrete race. Independently, every raw
coverage obligation remains unresolved unless its exact effect has a complete
replacement proof. Thus neither a range premise nor bounded UNSAT can discharge
an unrelated pair or coverage obligation.

The refactor preserves the original predicate order, object/context bindings,
construction dependencies and conservative failure paths. Its new private
consumers distinguish pair, coverage and library permissions. Preflight outputs
are read-only to consumers and are published only by the existing checked driver
path. These establish the intended composition boundaries. They do not close
all original transfer, source-translation, memory-model or module proof
obligations listed above; each remains separately reviewable.

Regression equivalence and independent selected-execution checks test this
implementation argument. They are finite evidence, not a proof of the assumed
coverage premise or all executions of arbitrary C programs.
