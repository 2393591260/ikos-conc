# Memory and ownership contracts

[Concurrent analysis documentation](../../CONCURRENCY.md)

### Memory and ownership

<a id="private-worker-allocations"></a>
<details>
<summary>Private worker allocations — distinguish objects belonging to separate workers</summary>

The race checker can distinguish live allocations belonging to separate dynamic
worker instances even when points-to analysis merges their allocation sites.
The current certificate covers a single, non-repeated `calloc` in an actual
thread entry. It closes all uses of the allocation and, if used, its TLS pointer
slot. Pointer publication, an escaped slot address, unknown call effects, pointer
graphs, ordinary calls to the entry, and a reachable use after `free` prevent the
certificate. It never treats the static allocation site as a unique object.

A checked wrapper may forward the count and element size unchanged to genuine
calloc, abort on NULL without other effects, and return the original allocation
on the continuing edge. The proof checks that complete native body and matching
AR formals, with one direct caller of the wrapper. Privacy is then checked at
that actual worker call using its count, size, uses and lifetime. Consumption
still binds the inner allocation and the exact wrapper child call context.
Changed arguments, foreign returns, memory effects, extra callers or an
unclosed body retain the conservative fallback. This is a pair certificate,
not an allocation transfer model or a new non-NULL/validity fact.

Every certified access needs its full byte range within the successful
allocation. Constant offsets are checked mathematically. The supported dynamic
case uses the same defined count as [the calloc allocation](https://pubs.opengroup.org/onlinepubs/9799919799/functions/calloc.html), a zero-based loop counter, a sole
`+1` backedge and a strict signed comparison whose true edge dominates both the
access and increment. The induction proves non-negative indices and a
representable increment; LLVM `inbounds` and `nsw` are not proof premises.
Allocation failure does not grant non-nullness or prune the caller's path.

Only pairs whose two recorded accesses match certified LLVM/AR effects and exact
heap provenance are excluded. All existing access records, shared interference,
coverage checks and unrelated race candidates remain active. The calloc
certificate targets the ILP32 Linux/GNU ABI and does not establish general
creator-to-child or per-iteration ownership.

A separate certificate supports a positive constant-size `malloc` in `main`
whose result is passed to exactly one child. The allocation must dominate the
create, and every path between two create attempts must cross a new allocation
event. Pointer uses in the creator are limited to SSA bitcasts and that transfer;
all entry-function uses must identify the same actual create. Failed attempts
also consume the object, so retrying with an old pointer is rejected. No success
or worker-termination premise is added.

The child may use constant-offset integer fields inside the real allocation
extent and optionally free the original base. Exact mathematical offsets cover
the full access width; a use reachable after free invalidates the certificate.
Publication, parent accesses, array-stored aliases, extra creates, unknown
callbacks and joins needed to reclaim an object from the parent require further
proof and remain outside this subset. This rule preserves the existing NULL
failure branches, coverage and shared interference.

</details>

<a id="indexed-heap-ownership"></a>
<details>
<summary>Indexed heap ownership — relate heap owners to their protecting mutexes</summary>

A separate finite proof pass can relate a global array of heap roots to a
corresponding mutex array. It checks all supported writer transfers, including
helpers and loop joins, before supplying any exclusion certificate. Fresh malloc
instances acquire an immutable owner on publication; links must remain NULL or
within that owner. Copies preserve the relation, while conflicting joins forget
it. Allocation sites are not treated as unique runtime objects.

For a pair of heap accesses, overlap in the same live object implies equal
owners. If both accesses hold their owner's actual mutex, that pair cannot race.
This conditional proof does not establish pointer validity or narrow the original
value analysis. Both complete points-to sets must contain only known heap
locations; numeric addresses, TOP and mixed storage classes retain their original
candidates. Missing ownership on either endpoint also retains the candidate.

The initial subset supports ILP32 GNU/Linux fixed root/mutex arrays, one
self-referential pointer field per object, exact object extents and verified
pthread/malloc APIs. Root slots become immutable before worker creation.
Cross-owner links, unknown effects, unsupported escaping, free/realloc, partial
pointer writes or unproved address bounds disable the family certificate.
Coverage warnings remain independent. Unsupported programs use the existing
analysis and may remain UNKNOWN.

A second complete pass can retain distinct root heads while allowing nodes
shared by several lists before thread creation. A fresh object stored in a root
slot becomes a head and cannot be stored in another slot or link. Old link
targets may be shared; they receive no unique-owner certificate. New worker
objects must be constructed under their corresponding actual mutex and may
only be published through a head or new object with the same owner. The pass
closes all pointer writes, helpers and allocation contexts; unknown publication,
free, partial writes or changing an old link concurrently disable this mode.

A one-hop link load carries a conditional owner: only if its actual object is
from a verified worker allocation family does that owner apply. Every birth of
that exact allocation/context family must qualify; a MAY-spawned state alone is
insufficient. The consumer checks every location in the intersection of both
complete pure-heap points-to sets. Old or mixed allocation families retain the
candidate. Transitive link traversal is deliberately unsupported. This mode
reports `indexed-head-publication-owner-v1`; its BMC-disabled positive, negative,
renamed and 64-bit payload controls run with
`python3 scripts/test_head_publication.py`.

For guarded indexes, `--partitioning=guards` can supply the required array and
mutex ranges. The pass accepts verified zero-argument `__VERIFIER_nondet_int`
declarations, including the C old-style declaration representation. It follows
ordinary definitions rather than granting their names special effects. A verified
`abort` terminates that path after checking earlier accesses; it does not make the
whole prefix unreachable. Other return paths remain part of the proof.


Constructor-field recovery can also use a separate conservative analysis to
certify the byte effects of indexed mutex calls. Every observed address must
select a complete mutex inside the actual array, with exact API and layout
checks. Only the resulting effect certificates enter a fresh analysis; memory,
thread summaries, coverage warnings and failed caches are not reused. Missing
or invalid observations retain the original conservative behavior. The first
subset requires private, unread thread-handle outputs and unused create results;
helper-derived indices and consumed handles may remain unsupported. This does
not assert mutex-call success or establish heap-object uniqueness. Controls run
with BMC disabled via `python3 scripts/test_mutex_array_effects.py`.

Root and mutex arrays may also be separate subobjects of the same global
structure, including fixed nested structures and reordered fields. Each retains
its typed field path and absolute byte extent. LLVM `inbounds` alone does not
bound a nested array: the independent analysis must observe every pointer-slot
load/store and mutex call within its own complete subarray. All contexts and
iterations must pass; missing observations disable recovery. The fresh analysis
checks each recovered load's actual address again. Ownership indexes are relative
to the selected subarray; held mutex keys keep their absolute allocation offsets.
The shared pointer board remains a MAY union, and ordinary write invalidation is
unchanged. Escaping addresses, overlapping writes, unsupported siblings, packed
or union containers, and ambiguous families retain conservative behavior.
Run `python3 scripts/test_nested_array_ownership.py` for these ILP32 controls,
including 64-bit payloads and wrapping index conversions.

Implementation: `analysis/region_ownership.hpp` and
`src/analysis/region_ownership.cpp`. The checker consumes certificates only after
the original value analysis has converged. With `--format=json`, add
`--status-filter=safe` to inspect `indexed-live-heap-owner-v1` proof rows; safe
rows are hidden by the default report filter. Local positive, negative and
transformed examples run with BMC disabled via
`python3 scripts/test_region_ownership.py`.

</details>

<a id="stable-flat-array-slices"></a>
<details>
<summary>Stable flat-array slices — prove that workers access disjoint byte ranges</summary>

`ArraySlices` can exclude a pair of heap accesses when complete native/AR
frames establish disjoint inductive ranges over the same stable global pointer.
The current ILP32 subset handles one signed i32 induction loop per worker and
per array constructor, with exact scalar helpers and pre-create initialization.
Each loop uses an arbitrary iteration satisfying its inductive lower bound;
the actual guard, stride, allocation extent and full access width must be
proved. The increment must preserve the invariant without wrapping. This is
an unbounded range proof, not bounded-search UNSAT.

Every supported main, constructor, helper and worker effect is checked. Global
updates after the first create, partial cell writes, unknown calls, pointer
fields, frees, extra cycles and module assembly refuse the certificate.
Heap reads remain fresh values; they never acquire read-from equalities.
All possible startup states and all ranges of both endpoints must satisfy the
disjointness query. Shared translation by the same actual stable base preserves
disjointness, including modulo pointer arithmetic; no allocation success,
nonnullness, pointer validity or thread completion is inferred. Accesses after
the loop receive no loop-body certificate. Unsupported frames and solver
limits retain the existing candidate.

Run `python3 scripts/test_array_slices.py` for renamed positive, overlap,
effect-frame and byte-width controls, including 64-bit payloads on ILP32.
Implementation: `checker/array_slices.hpp` and `src/checker/array_slices.cpp`.

</details>

<a id="serialized-arena-reservations"></a>
<details>
<summary>Serialized arena reservations — prove that successful reservations do not overlap</summary>

An independent symbolic mode handles a closed, immutable heap array initialized
before all worker creation. It interprets every normal startup prefix and proves
the allocation size equals the positive element count times its stride without
32-bit wrap. Under one real default-normal mutex, the shared counter starts at
zero and every successful transaction strictly increases it within that count.
A success at zero is a nonempty reservation; a transaction without a Store must
have a provably empty processing loop. Later workers see an arbitrary counter
in the inductive range, rather than the first worker's startup value.

For a supported natural loop, the checker proves the iterator invariant at
entry, checks an arbitrary iteration and its backedge, and checks every full
element access against the owning half-open slice. It does not enumerate array
sizes, workers or loop iterations. Native GEP and AR PointerShift base, index,
stride and widths must agree. Scalar snapshots are per execution; data values
and memory-free helper returns are overapproximated. Overflow flags are proof
obligations, never assumptions. Both candidate endpoints must refer exclusively
to the checked live-heap allocation. This conditional byte-conflict certificate
does not establish successful malloc, pointer validity, completion or fairness.
Missing effects, aliases, lifetime or loop evidence, incomplete SMT checks and
resource exhaustion retain the ordinary candidate. Static SMT limits are
separate from BMC budgets; the finite global-array mode below is unchanged.

The checker can prove disjoint byte ranges in a flat global integer array when
each access uses a fresh reservation from a sealed counter. The initial subset
uses a real, defined `__VERIFIER_atomic_*` function with exact `void(int*)` ABI;
its complete body is non-interleaved under the
[SV-COMP atomic-function convention](https://sv-comp.sosy-lab.org/2026/rules.php).
Only the reservation body is atomic, not later caller accesses.

A separate finite-state certificate starts from the counter's exact initializer
and interprets each reachable complete transaction. A success returns the old
positive counter and strictly advances it, reserving the half-open slice between
those values. A failure writes zero and leaves the counter unchanged. The proof
must reach this deterministic exhausted self-loop; a partial search, unsupported
operation, overflow or budget exhaustion yields no certificate. Consequently
arbitrarily many later calls cannot reuse a successful slice. Every intermediate
arithmetic operation is checked before later guards, without assuming LLVM
`nsw` or `inbounds`.

All counter writers and actual calls are closed. Each out-parameter call writes to one
activation-private integer stack slot, with no address escape, later writer or
repeated call in that activation. A successful-result guard must dominate each
certified access, and its full byte width must fit both the reserved slice and
the real array. Both endpoints of an excluded pair need these facts for the
same counter and array. A shared-load reuse of a published index, zero result,
counter reset, escaping arena address or unknown effect keeps the ordinary
candidate. Points-to, interference, access records, BMC and coverage are unchanged.
An additional mode supports an integer-returning allocator protected by one
concrete, completely zero-initialized default-normal glibc mutex. Its complete
address inventory allows only the actual lock/unlock APIs. A finite ownership
analysis checks all defined helpers: lock requires certainly unheld, unlock
requires certainly held, and a returning helper that touches this mutex has
net-zero ownership. Possible ownership states join by union; loops reach a
finite fixed point. Reinitialization, escaping addresses, unmatched unlocks,
nested acquisitions and ambiguous ownership refuse the certificate.

The counter transaction itself requires exactly one continuous acquisition and
release. An old-counter read before lock, a release/reacquire between capture
and update, or a counter reload after release is rejected. The interpreter
selects each return phi from its actual predecessor. Only a direct SSA result
with a dominating successful-result guard supplies a fresh ticket; shared
reloads, stale merges and unsupported spills retain candidates.

These subsets target ILP32 Linux/GNU. They do not order an unrelated runtime
initialization against later accesses; general initialization phases still
need separate proofs.

</details>

<a id="immutable-heap-array-reads"></a>
<details>
<summary>Immutable heap-array reads — retain values from completely initialized, stable cells</summary>

`ReadonlyHeapReads` provides a separate, conservative source certificate for
startup-initialized scalar arrays and a depth-two pointer table. It traverses
the reachable main, constructors, workers and scalar helpers in the actual
LLVM/AR program. Mandatory whole-cell stores establish initialized prefixes by
induction; every published table must finish construction before any create.
The table is a fresh main allocation, avoiding reuse of a scalar snapshot through
a retyped alias. Published roots, dimensions and heap cells remain immutable.

Each full-width scalar-array read uses an arbitrary function of its allocation
snapshot and exact byte offset, so repeated reads of the same cell retain guard
bounds. Different cells and row payloads remain unconstrained. Mutable scalar
globals begin arbitrary; a cell owned by one closed, nonrepeated main-created
worker may retain that instance's stores and reloads. Other mutable cells may
retain a snapshot only within one balanced harness atomic region, after every
possible writer has been checked to use that atomic contract. Region boundaries
forget shared cells while retaining already-read SSA values. Main's observations
after creation remain fresh; no worker state is imported into main.
These scalar caches apply only to the arbitrary-header route below; the older
canonical route keeps fresh mutable loads. Before cache-assisted path pruning,
the new route closes all potential stores and calls through its scalar helpers.
Unknown writes, aliased stores and opaque calls refuse it even in a branch that
the later path proof could discard.

The canonical induction route remains available. A separate worker route covers
one noncanonical natural loop by cutting its header edges and checking an
acyclic traversal from an arbitrary header state. That state retains immutable
startup sources, but forgets worker-prefix guards, mutable cells and scalar
live-ins. All bodies, exits, suffixes and scalar helpers are checked; loop-carried
pointers, unsupported storage, residual cycles and unbalanced atomic paths refuse
the frame. This is coverage by an inductive overapproximation, not finite
unrolling or a termination claim. The exact C/AR one-bit nondet-bool declaration
supplies an arbitrary bit to ordinary byte-array initialization.

Each consumed table and row access requires a
complete initialized source and a proved byte range; unknown effects, unsupported
aliases, partial writes, solver uncertainty or exhausted proof budgets refuse
the entire certificate.

The checker can explain only the exact certified read's synthetic handle warning
and exclude its false alias with an actual direct whole-scalar global write.
All other effects and pairs remain. No shared MAY board, numerical pointer value,
nullity, allocation success, lock, join or completion fact is changed. BMC retains
its original handle-representation gate. These are conditional nullable heap
source facts under the existing access model, not memory-safety validation.

Implementation: `checker/readonly_heap_reads.hpp` and
`src/checker/readonly_heap_reads.cpp`. Regression entry:
`python3 scripts/test_readonly_heap_reads.py`; it includes ILP32 long-long data,
incomplete construction, changed indices, publication and alias mutation controls,
and real races remaining after heap reads have been certified.
`python3 scripts/test_readonly_loop_snapshots.py` additionally covers arbitrary
loop headers, duplicate worker instances, extra writers, atomic boundaries,
prefix-only guards, exact bool ABI, long-long row data and real scalar conflicts
remaining after successful heap certification.

The immutable-read, bucket-membership and fixed heap initialization analyses
share the stateless
`analysis/source_call_binding.hpp` check for the actual LLVM/AR callee, C calling
convention, argument agreement and unsupported ABI attributes. Each analysis
keeps its own source inventory and effect/safety proof; a successful binding
alone grants no memory, synchronization or safety fact.

</details>

<a id="immutable-bucket-membership-with-protected-payloads"></a>
<details>
<summary>Immutable bucket membership with protected payloads — check graph membership and bucket locks</summary>

The ownership mode of `BucketListOwnership` checks a closed startup-built graph of embedded two-pointer
heads and individually allocated nodes. It interprets the actual LLVM/AR CFG,
helper calls, machine-width indices and simultaneous PHIs. Successful dynamic
allocations have separate identities; a real full scalar Store at the exact
allocation base establishes only the existing normal-continuation premise.
Every pointer cell must then receive its own complete Store before use.
At first creation, exhaustive next/previous closure assigns each live node to
exactly one head. Concurrent link mutation, cross-bucket membership, unknown
calls, lifetime changes, incomplete cells and unproved byte bounds refuse the
whole certificate.

Worker payloads are TOP. Scalar comparisons and actual helper arguments preserve
TOP; a TOP condition explores both successors without adding a chosen value.
TOP still cannot determine an address, extent or owner. Branch-only nondeterministic
choices may likewise become TOP after a complete scalar-use closure check. Complete
state exploration covers every reaching access, including repeated graph states;
resource exhaustion is refusal, never a bounded safety argument. Main is checked
on all paths before workers are projected onto their immutable graph frame.
Only identical complete frames are shared; no main path condition or payload
sample restricts a worker. Link offsets are proved single-valued before projection. All concurrent
payload accesses must hold the exact mutex of their bucket. This narrow model
uses GNU/Linux i386 default-normal mutex initialization with NULL attributes
(the ordinary sealed-runtime route in
[glibc 2.39](https://github.com/bminor/glibc/blob/ef321e23c20eebc6d6fb4044425c00e6df27b05f/nptl/pthread_mutex_init.c)), with each
aligned 24-byte leaf initialized once before creation and no later mutation.
It does not assume arbitrary POSIX initialization succeeds.

Only exact fully covered source effects and all combinations of certified access
endpoints can discharge warnings. The four-byte private create output has its
own actual native ABI and alignment proof. A real pointer-to-integer observation
may only feed integer casts and the main return value; it never becomes a usable
numeric address. The shared representation inventory checks this complete use
closure for an unreferenced, argument-free `main`; a branch, store, call or
non-main return refuses. Native handle arrays retain their separate closed-use
checks. Numerical/MAY summaries and BMC's original representation gate
remain unchanged. This is a live-byte race certificate, not pointer-safety,
allocation-success, termination, thread-completion or official witness validation.

Closed extra normal mutexes can be treated as runtime stutters; they grant no
bucket ownership. Readonly assertion strings are accepted only through actual
validated terminal assertion calls. At CFG joins, standard source-SSA liveness
includes PHI operands on predecessor edges. Dead current-frame SSA can be
forgotten before exact-state deduplication; caller frames, path constraints,
memory, precise live pointers and held owner locks are retained. A helper's return
is captured before its completed SSA frame is cleared.

Fixed private native `pthread_t` arrays reuse the real abstract interpreter's
whole-plan range observations: every create output and handle Load must have an
existing true observation, accumulated by AND across contexts and iterations.
Dirty fixpoints and missing thread coverage refuse. The native transfers already
use TOP values and grant no join HB independently of this checker. After the
first possible creation freezes the graph, a finite suffix pass checks every
CFG edge and admits only these native operations and pure control computations.
It collects all possible callbacks and enclosing private-array ranges. Those
ranges prove only object disjointness, never one precise slot or thread identity.
Application writes, graph changes and unsupported suffix calls refuse even after
a join. Only a complete source proof can explain its exact synthetic-native-call
coverage warnings; it does not clear the shared environment's coverage markers.

A shared narrow address gate recognizes NULL-root, constant, 32-bit layout
projections with identical computed LLVM/AR offsets. This observes no application
object address and supplies no nonnull, lifetime or inbounds/UB premise. Real
address observations, dynamic offsets and integer-to-pointer casts keep their
previous conservative gates. The native-array route does not extend this
exception to a pointer returned as main's exit code.

A separate typed-graph mode supports complete global nodes and immutable links
shared between roots. It checks every startup graph before merging link targets
by union. Heap nodes of the same validated layout form one summary class;
equal summary tags never imply equal concrete pointers. Heap/heap comparisons
explore both outcomes, and worker payload reads remain TOP. Global sentinels keep
their actual byte offsets. This finite overapproximation permits cyclic graphs
without assuming traversal termination.

The mode provides typed byte footprints, not unique ownership or new lock facts.
Every post-publication payload access must match an ordinary abstract-analysis
access record by statement, thread root, call context, width and kind. This match
alone does not prove that AI pointer summaries cover every source target. To
discharge an unknown-address startup link write, the source graph proof must also
establish that every concurrent payload endpoint holds a common, exact, sealed
GNU/Linux ILP32 NORMAL mutex. Its lock state is tracked across helpers and kept
in loop-state signatures and endpoint alternatives; it is not inherited by child
threads. Conditional acquisition, early release or an unprotected main/worker
access prevents this complete synchronization proof.

This branch independently proves payload synchronization; it does not repair the
ordinary MAY pointer summaries. Without that proof, unknown pointer stores retain
their coverage warnings. Scalar footprint and independent owned-graph rules are
unchanged. Payload/link and private-runtime byte separation can exclude pairs;
overlapping payload pairs still require a valid synchronization proof. Missing
initialization, structural mutation, unknown effects or incomplete closure refuse
the certificate.
Run `python3 scripts/test_shared_node_graph.py` for this mode's controls.

Implementation: `checker/bucket_list_ownership.hpp` and
`src/checker/bucket_list_ownership.cpp`. Regression entry:
`python3 scripts/test_bucket_list_ownership.py`; ILP32 controls include long-long
payloads, a nonterminating traversal, wrong locks, cross-bucket links, incomplete
initialization, early creation, native lifecycle changes, byte corruption and
integer-address observations with application-memory effects.

</details>
