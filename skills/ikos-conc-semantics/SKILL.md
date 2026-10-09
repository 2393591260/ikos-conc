---
name: ikos-conc-semantics
description: "Develop, debug, review, or evaluate IKOS-ConC concurrent program analysis. Use for changes to abstract domains, thread and memory semantics, candidate filtering, BMC/SMT, property checkers, or SV-COMP evaluation; supports race detection and future deadlock/reachability work."
---

# IKOS-ConC semantics

Improve general concurrent analysis with explicit proof obligations and reproducible
evidence. Current user priority: SV-COMP 2027 no-data-race; use the local 2026 frozen
set for regression until a target version is fixed. Preserve a path to deadlock,
reachability, and other properties without implementing them speculatively.

## Establish the semantic contract

- Identify the property, C/LLVM/AR semantics, memory model, machine widths, library
  contracts, and analysis mode. Do not equate concurrency with SC or assume C11
  semantics automatically. Separate supported behavior from assumptions and gaps.
- TRUE/SAFE needs coverage of all relevant executions and conservative transfer,
  fixpoint, and filtering rules. FALSE needs an actual property-violating execution:
  a witness-preserving encoding, or independent validation of the selected model.
  A may-warning alone proves neither. UNKNOWN is inconclusive; record timeout,
  unsupported semantics, and failure separately in evaluation.
- Bounded search without a counterexample does not prove unbounded safety. Bounded
  paths with unconstrained values can produce spurious SAT. Solver UNSAT/UNKNOWN,
  path truncation, empty candidates, and unsupported operations need explicit
  interpretations; do not silently convert them to SAFE.
- FN=0 is the first triage priority, and FP=0 also constrains competition verdicts.
  Benchmark zeros are finite observations, not a soundness proof. Keep conservative
  warnings separate from confirmed FALSE results.

## Make a semantic change

1. Locate the flow from frontend/AR through transfer functions, domains, shared
   summaries, candidate creation/filtering, confirmation, and verdict/witness
   reporting. Read the relevant callers, not the entire repository by default.
   Consult [project.md](references/project.md) for navigation and dated facts.
2. For a rule that removes behavior or confirms a violation, state its premises,
   approximation direction, evidence establishing those premises, and fallback
   when they cannot be proved. A comment saying “only loses TP” is not evidence.
3. Check interactions that can invalidate those premises: dynamic thread/call/loop
   identity; alias, lifetime, width and escape; read-from/value dependencies;
   synchronization and failure behavior; unknown side effects; join/widening and
   cross-thread fixpoint convergence. Read only the applicable parts of
   [property-contracts.md](references/property-contracts.md).
4. Make the smallest complete, general fix at the owning layer. Keep property
   queries separate from reusable thread/memory semantics. No filename, expected
   label, benchmark family, or score-dependent semantic exceptions. Harness name
   recognition must follow a verified API contract, not a suggestive substring.
5. For nontrivial semantic changes, use a minimal failing case plus a control and
   relevant variants. Verify the executable actually contains the change, then
   perform targeted checks and proportionate benchmark A/B validation as described
   in [validation.md](references/validation.md). Do not run full sweeps for edits
   that cannot affect analyzer behavior.

Conservative gates may be temporary supported boundaries; replace them with proven
modeling when extending capability. Do not freeze a historical implementation limit
into a permanent design requirement. Optimizations may prioritize work without
changing verdicts; pruning, caching, slicing, or merging needs a preservation argument.

## Evidence and research

Treat project docs, supplied model summaries, labels, and prior reviews as claims
to check. Current source establishes implementation, not mathematical correctness;
experiments establish observations, not universal guarantees. Read the original
paper/specification and versioned reference implementation when introducing or
changing an algorithm. Record which premises transfer to IKOS and which do not.
Do not import Dartagnan's conclusions while omitting its supporting encoding.

Keep context small: task-specific `rg` searches, relevant functions, saved repros,
and a concise record of commands and findings. Load supporting references only as
needed; do not automatically load all `docs/` or unrelated skills. During handoff,
report the change, its semantic justification, exact validation scope, verdict
transitions/performance when measured, and remaining uncertainties. Distinguish
implemented capability, proposed design, and unverified claims.
