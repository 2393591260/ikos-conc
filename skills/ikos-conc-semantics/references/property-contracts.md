# Semantic checks by change and property

Read only the sections implicated by the task. These are proof obligations to
resolve against actual semantics, not claims that IKOS currently implements them.

## Shared semantic foundation

- **Identity:** distinguish static function/instruction/allocation site from dynamic
  thread, call, loop iteration, and object instance. SSA pointers alone cannot key
  all dynamic values. Separate independent nondet events, preserve reuse of one
  event's result, and propagate parameters/returns in their actual contexts.
- **Memory:** establish object identity, alias set, lifetime, publication/escape,
  byte offset and width. Unknown points-to, width, or escape is not disjointness or
  privacy. Local allocation does not imply thread locality after publication.
  Use target layout and cover synthetic/bulk/RMW accesses where relevant. “Two
  offsets differ” does not establish disjoint ranges or valid object provenance.
- **Values:** free symbols are justified by a verified source contract, not by
  inability to encode a value. Shared loads require correlated initial/write values
  and a memory-model-consistent source. Loads of immutable data or proven local
  objects can be modeled without full interthread rf/co if their premises suffice.
  Machine arithmetic, casts, comparisons, overflow, poison/undef and signedness
  must match the selected language/IR contract; range constraints alone do not
  model every machine operation.
- **Synchronization:** verify object identity, lock mode/recursion, ownership,
  return codes, try/timed failures, spawn/join target and completion, once-control
  identity, condition wait's release/reacquire and wakeups, barriers/semaphores,
  atomic orderings, and relevant library summaries. An attempted operation is not
  automatically a successful operation. MAY facts cannot establish MUST protection.
- **Fixpoint:** check lattice polarity, top/bottom, join/widening, monotonic shared
  summaries and dirty notification, context-sensitive cache keys, invalidation,
  and convergence. Truncation/iteration limits need a conservative fallback.
- **Calls:** resolve bitcasts/indirect calls only with justified targets. Omitting
  calls can hide writes, blocking, synchronization, thread creation or termination.
  A precise summary may still include memory effects; do not blindly double-count
  them through unknown-call fallback or remove that fallback without coverage.

## Slicing, BMC, and witness preservation

For a candidate-driven slice, account for the control/value/address dependencies
that determine reachability and conflict, initial state, creators and main prefix,
relevant writes/threads, synchronization and library effects. Two individually
feasible thread paths need not compose into one feasible program execution.

A partial closure is allowed when the omitted behavior has a justified summary or
is irrelevant to the queried property. For unproved omissions, decline confirmation
or validate a concrete extension; do not guess the nearest syntactic store as rf.
Prove enough dependencies for the accepted witness, rather than demanding an
unbounded whole-program expansion for every candidate. Candidate ordering is a
search heuristic; candidate removal is a semantic decision.

Under SC, a load's source depends on a compatible execution order and intervening
writes. Under weaker models, rf/co and the model's other relations must jointly
satisfy its axioms. Do not invent `hb = po + communication + mutex + assume`:
hb and which edges generate it are defined by the selected specification.
An assume restricts feasible states; it is not intrinsically a synchronization edge.

## Data race

Establish conflicting byte ranges on a jointly reachable object, with at least one
write, relevant dynamic thread distinction, and the property's synchronization/
atomicity requirements. A possible alias or empty lockset is a candidate, not an
existential witness. Total-order adjacency is not automatically semantic concurrency;
use an equivalence theorem only under its actual encoding and model premises.

For SAFE pruning, prove disjointness, definite exclusion/order, or another sufficient
property-specific condition. If privacy is used, establish non-escape/non-publication
at the access, not merely later same-thread locking. Points-to cardinality or creator
metadata alone need not prove it. Preserve uncertain candidates or an inconclusive
verdict; do not let an unsupported candidate disappear into an empty SAFE report.

## Reachability

Specify the target and initial states. FALSE for `unreach-call` requires a feasible
execution reaching its designated error call; TRUE requires exclusion of all such
executions. Keep this polarity explicit when discussing a reachability query.
Finite-prefix witnesses can suffice; bounded UNSAT cannot establish global
unreachability without a completeness argument. Check call/return, guards, shared
values, spawn context, and abnormal exits. Do not reuse race-specific result or
witness adapters without property-aware mapping.

## Deadlock and progress

Define whether the property is global deadlock, partial/thread deadlock, or a
progress/liveness property. A lock-order cycle identifies risk, not necessarily a
reachable deadlock. A witness needs a jointly reachable state with the required
blocked threads and no enabled transition relevant to that definition. Account
for recursion, lock modes, trylock, timeout, condition wakeups, joins, termination,
other runnable threads, and environment steps. An open system may require explicit
environment assumptions.

Safety-style deadlock states and starvation/livelock are different obligations.
Liveness generally needs infinite-behavior/fairness reasoning, not just a finite
blocked prefix. Do not claim support based solely on the current race event model.
Extend reusable semantics as needed; avoid building an unused generic framework.

## Research for algorithmic changes

Find the original specification/paper and a versioned reference implementation.
For BMC, abstract interpretation, partial-order reduction or weak memory, identify
the theorem/algorithm assumptions actually needed by the proposed change. For
Dartagnan, inspect the relevant event/value/memory-model/property encoding at the
chosen revision, not just its top-level race predicate or reported score.

Record: source title/version and exact relevant section or symbol; its property
and execution model; how it treats bounds, initial state, calls, memory and
synchronization; which premises IKOS already satisfies; missing obligations and
the fallback. If primary sources are unavailable, mark the transfer as unverified
and limit conclusions accordingly. Routine API/build fixes do not require a fresh
literature survey.
