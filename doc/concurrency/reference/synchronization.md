# Synchronization and lifecycle contracts

[Concurrent analysis documentation](../../CONCURRENCY.md)

### Synchronization and lifecycle

<a id="native-cmpxchg-values"></a>
### Native compare-and-exchange values

LLVM `cmpxchg` imports as separate success and failure value paths. Success
requires the observed old value to equal the expected value, writes the desired
value and returns a true flag. Failure preserves memory and returns a false
flag; weak failure also permits equal values. Both outcomes return the same old
value used by their comparison. Volatility and the legal read/write components
of success ordering, and the actual failure-load ordering, are retained.

The split AR accesses conservatively model values and control flow. They grant
no native atomic indivisibility or new cross-thread synchronization proof.
Existing exact BMC and protocol consumers continue to require their own native
operation support. `scripts/test_cmpxchg_values.py` checks failure, success,
spurious weak failure and impossible outcomes on both ABIs, including pointer
and 64-bit cells.

<a id="self-publication-one-write-repeated-reads"></a>
<details>
<summary>Self-publication: one write, repeated reads</summary>

A separate self-publication certificate handles a scalar which is both the
state and the published value. Under one closed default-normal mutex, every
normal critical-section exit must leave the final value: an initial entry
executes exactly one Store, while an already-final entry executes no Store.
Even a same-value later Store refuses this rule. Induction over mutex ownership
proves a unique dynamic write across arbitrarily many worker instances. A finite
per-instance phase proof then orders that write before each certified post-gate
read, including repeated reads. It does not order arbitrary later writes.

The initial subset requires a closed uniform-zero integer array and scalar
body arguments. The complete memory-free entry prefix and guard helpers produce
argument constraints; every feasible argument tuple is enumerated, with a final
UNSAT check proving exhaustion. A model or enumeration limit is inconclusive.
The actual body is explored to a finite fixed point for every tuple, with
parallel PHIs, captured loads, exact array byte bounds and checked arithmetic.
Unprotected state loads remain TOP; their values do not justify their own race
exclusion. Unknown effects, escaping addresses, missing normal-return coverage,
weakening the write invariant, solver unknown or incomplete traversal refuse the
certificate. The parent is limited to private scalar effects and checked native
creates; no thread-count bound, successful completion or join fact is inferred.

</details>

<a id="locked-thread-creation"></a>
<details>
<summary>Creation inside a normal mutex interval — actual ordering without inherited locks</summary>

`locked-creation-v1` is an independent final access-pair proof for closed roots
in an acyclic creation graph from main. Every root CFG is acyclic, and each root has at most one
lock and one unlock per object; any unlock must be dominated by its own lock.
GNU Linux i386 24-byte/4-byte and LP64 40-byte/8-byte mutex objects must have
matching whole-zero source/AR initialization and closed actual lock/unlock uses.
This is the ordinary non-robust NORMAL contract of
[glibc 2.39](https://github.com/bminor/glibc/blob/glibc-2.39/nptl/pthread_mutex_lock.c),
not an assumption about arbitrary POSIX DEFAULT or recursive mutexes.

Two separate relations may exclude a pair. First, an access by main in the
same held interval as **all** creates of a child entry precedes a child access
dominated by that child's own acquisition of the same mutex. The child access
may follow its unlock. This proved order may continue through further actual
creates: a descendant starts after main's release only when every possible
creation source is already ordered after that release. Facts are intersected
over all creators in creation-graph order. Creating before the acquisition,
another unproved creator or a creation cycle refuses this extension. No lock
ownership transfers to a child. Second, if main holds that mutex at all those creates
and has no reachable later unlock, a different root's access while actually
holding the mutex precedes the created child's accesses. That other root must
not be main: main's own accesses can still race with its child. A previously
released lock cannot supply the second relation.

Instruction order and every creation source are checked. Parents transfer no
lock ownership to children, and two unprotected children remain concurrent.
Whole scalar memory effects, source/AR call and endpoint bindings, native ABI,
and the absence of helpers, callbacks, unknown effects and unclosed creators
close this subset. Without joins the relations above apply. A separate narrow
mode accepts exactly one join in the entire closed program, issued by main
for its uniquely created direct child. Every root then has one static creation
and at most one activation. The native-width global handle has only its actual
create and one plain load; source/AR identity, unique load definition and all
address uses are checked. Create dominates that load, and the join dominates
main's unlock while both remain inside its actual held interval. The return
output is NULL and the join result is unused.

This mode excludes only pairs between that child and another root actually
holding the containing mutex. Such a holder may precede or follow the child
scope; no fixed direction is reported. A real child access implies its create
succeeded, so its unconsumed joinable handle is valid. Under the checked
[glibc 2.39 untimed join](https://github.com/bminor/glibc/blob/glibc-2.39/nptl/pthread_join_common.c)
contract, returning then requires completion. Failed creates are not assumed to
leave a harmless handle: their outputs can contain stale TCB addresses. Exactly
one join, and consumption only for the actual target child, avoid granting
completion from another failed creation. Multiple or nested joins, detach,
handle copies/overwrites/extra reads and unjoined descendants receive no scope
certificate. Main's own accesses receive no child-lock privilege. In this mode
the earlier creation relations are not additionally consumed. Missing premises retain the candidate. This rule
grants no library effects, coverage, completion or FALSE confirmation rights.

Run `python3 scripts/test_locked_creation.py` for dual-ABI wide-payload controls.

</details>

<a id="finite-sequentially-consistent-protocols"></a>
<details>
<summary>Finite sequentially consistent protocols — exhaust the supported protocol state space</summary>

The race checker can independently saturate a finite SC protocol state space.
Its initial subset closes two distinct, once-created worker roots, direct whole
1–64-bit integer scalar effects, and all LLVM/AR call and memory effects on
the checked GNU/Linux ILP32 and LP64 layouts. Native handle storage is checked
separately from the protocol integers. Each protocol global has an exact constant initializer and only actual
system-scope sequentially consistent loads and literal stores. RMW, fences,
weak or mixed accesses, escaped addresses, heap/array effects and unknown calls
refuse this certificate. The parent cannot write globals; its private scalar
handle effects are checked, and ignoring its joins grants no completion fact.
Memory-free helper calls may return in the projection, even when a concrete
call aborts; this does not prove assertions, progress or successful termination.

For each pair of unique worker access events, the pass retains possible PCs,
dynamic load/parallel-PHI snapshots, atomic last values and release causal tags.
It explores all scheduler prefixes to a reachable-state fixed point, including
progress by one worker after the other returns. Release/acquire propagation,
not the SC total order itself, must order the two events. Alternative histories
remain distinct; their causal tags are never unioned into a purported MUST fact.
Protocol loops may be unbounded, while a repeated target instruction or worker
instance refuses this first certificate. Unsupported scalar values become TOP;
branch alternatives are retained. Exhausted graph/state/transition budgets
refuse the proof. No bounded UNSAT or lack of a found race establishes SAFE.

</details>

<a id="two-mutex-handover"></a>
<details>
<summary>Two-mutex handover — prove ordering through a checked lock history</summary>

The race checker can also certify a closed two-mutex handover. An access inside
a continuous `D` interval after acquiring `A` is ordered against an access that
still holds `A` acquired under `D` after releasing `D`. Whichever `D` interval
comes first establishes the order, directly or through `A`. The finite analysis
joins possible lock-history states and requires each endpoint's role in every
reaching state and helper context. Releasing/reacquiring a mutex resets the
corresponding history; returns do not release mutexes. Unmatched unlocks,
recursive acquisitions, unknown calls or proof exhaustion refuse the certificate.
The initial scope requires exactly two closed default-normal global mutexes on
GNU/Linux x86 ILP32 or x86-64 LP64, direct scalar memory effects, direct helpers
and native thread creation with a private native-width output and NULL argument. It adds neither a join
completion fact nor a BMC synchronization shortcut.

</details>

<a id="closed-first-publication-ordering"></a>
<details>
<summary>Closed first-publication ordering — order initialization before acquired accesses</summary>

The race checker can independently certify a finite publication protocol for
heap records with one pointer link and scalar payload. A closed allocation
prefix binds initialization stores to the actual helper call frame and installs
an initialized-pointer fact only at completion. An unused native definition is
excluded from birth inventory only under the sole main entry and closed
function/callback-use contract. A prefix may cross the certified pure-abort
allocation check on a finite, single-entry continuation; all intermediate
definitions are cleared before importing its completed aliases. Other branches,
alternate ingress and loops refuse that summary. Every subsequent root/link
publication must store NULL or an initialized pointer under the same concrete
normal mutex; every link acquisition requires that mutex. Immutable startup
roots may be acquired without it only after all possible root writers have been
checked. Exact copies, all-valid control-flow joins and helper returns preserve
the initialization ordering after unlock. Indexed root storage additionally
requires complete, aligned byte ranges from every final abstract access fact;
independent mutex-array effect certificates grant no publication lock.

The resulting rule excludes only initialization/initialization or
initialization/acquired-access pairs on covered live heap objects. It proves
neither pointer validity nor allocation-site uniqueness, and does not exclude
ordinary post-publication data races. Unknown effects, representation changes,
missing sources, unmatched ABI/callback uses, partial graph accesses or an
unproved protocol path disable the rule. This is separate from the disabled
legacy `private_init` heuristic and from the indexed ownership proof.

The ordering uses program order, thread creation and the POSIX mutex release /
acquisition memory synchronization contract. Both checked GNU/Linux native
layouts are supported: graph pointer slots and padded fields match LLVM/AR,
and the normal mutex has complete zero storage, native alignment and the actual
typed pthread API. The [ABI table](module-contracts.md#per-mechanism-abi-boundaries)
records the sizes and remaining premises. This is not a claim of arbitrary
weak-memory publication support.

</details>

<a id="sealed-runtime-initialization-gates"></a>
<details>
<summary>Sealed runtime initialization gates — check one-time initialization before later accesses</summary>

A separate `OnceInitialization` certificate orders a full-width scalar runtime
initialization against accesses that must pass a concrete default-normal mutex
gate. The sealed i32 state starts at zero. Exact critical-prefix execution for
entry states zero and one proves that the former performs one initialization,
publishes state one, and unlocks; the latter only unlocks. All state and gate
uses are closed, so this is an inductive invariant for arbitrarily many gate
entries, rather than a bounded-thread safety argument. The syntactic default
branch of a lowered switch need not be reachable from this invariant.

A finite interprocedural phase pass starts every actual callback without a
post-gate fact. It joins possible phases by union, intersects post-gate facts
over all observations of an access, follows helper returns, and iterates CFG
loops. A helper used both before and after the gate receives no universal
post-gate certificate. An initializer source reached outside its designated
critical prefix invalidates the gate proof. Unreachable definitions are ignored
only after closing the executable root/callback graph and function-address uses.

Only initialization/post-gate pairs for the exact global and full byte interval
are excluded. Later/later races, including equal-value writes, remain subject to
ordinary checks. Resets, address escapes, partial writes, unsupported effects,
incomplete prefixes or proof exhaustion refuse certification. The implementation
uses the checked GNU/Linux x86 ILP32 or x86-64 LP64 normal-mutex representation;
the gate state remains i32 and 64-bit scalar payloads are supported on both ABIs.
It changes no
points-to, interference, coverage or BMC semantics.

Arena success guards also recognize a narrowly proved terminating zero branch
when raw CFG dominance retains a continuation after a defined helper. The guard
must dominate the selected access. Its zero-successor block must encounter an
actual libc `abort` call, or an actual `void(i32)` helper called with literal
zero whose exact scalar comparison/branch path reaches that abort. Only unused,
verified freeze metadata calls are transparent; an earlier ordinary call, an
unknown condition, a return, a loop or a bare `unreachable` refuses this extension.
This does not infer termination from a helper name or `noreturn` attribute, and
changes no LLVM CFG, numerical invariant, coverage or BMC result directly.

</details>

<a id="monotone-publication-through-a-mutex-protected-flag"></a>
<details>
<summary>Monotone publication through a mutex-protected flag — order earlier writes before observed reads</summary>

`GuardedPublication` proves a pair-specific ordering from main's scalar writes
to reads by any number of workers. A closed, non-TLS global flag starts at zero
and has one noncyclic constant-one Store in nonreentrant main. All its accesses
use the same actual default-normal GNU/Linux x86 ILP32 or x86-64 LP64 mutex. Watched payload globals have
only full-width direct accesses; every writer is main, and no CFG path from the
publication Store reaches another payload write. Helpers cannot hide watched
accesses or mutex/condition operations. Calls, function addresses, foreign runtime
data and pointer representations are closed independently of numerical values.

A finite CFG worklist tracks possible held states (union), observed publication
(AND), and continuously protected flag-Load identities (intersection). Only an
exact Boolean guard edge implying one from a real protected Load establishes
observation. Other branches are both explored. Unlock and condition wait clear
cached Loads. Under the sealed zero-initialized 48-byte condition variable
(aligned to 4/8 bytes) and 24/40-byte default-normal mutex contract for ILP32/LP64,
a normal cond-wait return retains mutex
ownership; spurious wakeups and failure statuses grant no observation. Robust,
priority-protect, custom initialization, cancellation, destruction and unknown
object uses are outside this contract. Source evidence is the [glibc 2.39
condition-wait implementation](https://github.com/bminor/glibc/blob/ef321e23c20eebc6d6fb4044425c00e6df27b05f/nptl/pthread_cond_wait.c).

Because only main can publish one, a worker observing one while owning that
mutex follows the publisher's actual release, transitively through intervening
owners. Earlier main writes therefore precede its later payload read, including
reads after unlock. This finite MUST proof covers loops and arbitrary worker
multiplicity; it does not enumerate bounded schedules or infer termination.
Only matching actual source/AR write/read endpoints consume it. Unsupported
aliases, effects, control origins and exhausted budgets grant no certificate.
Real API termination is checked by declaration and intrinsic identity; a
returning user-defined `abort` or bare `unreachable` cannot remove a predecessor.

The analysis leaves numerical/MAY state, public lock/HB/join facts and BMC
unchanged. It inherits the existing object-based pthread API effect envelope,
without proving arbitrary invalid-handle libc behavior. Allocation calls must use
the actual native size_t prototype. Run
`python3 scripts/test_guarded_publication.py --bits=32` and `--bits=64` for int/long-long payloads,
8/32-bit flags, loops, guard polarity, spurious-wakeup, late-write, alias and
termination controls. Implementation: `checker/guarded_publication.hpp` and
`src/checker/guarded_publication.cpp`.

</details>

<a id="closed-binary-atomic-tokens"></a>
<details>
<summary>Closed binary atomic tokens — prove exclusive access through checked atomic transitions</summary>

BinaryTokenOwnership supports GNU/Linux x86 ILP32 and x86-64 LP64 layouts
and can independently certify a global token initialized at zero. This does not
change the token's own integer width: a supported uint32 ticket retains its
32-bit nonwrapping contract on LP64. GNU semaphore effects have their own ABI
contract and do not become supported merely by this token layout check.
 Supported operations take an i32 pointer or use an exact direct global
with 8-, 32- or 64-bit integer storage and no arguments. Actual defined
`__VERIFIER_atomic_*` bodies must implement checked
zero-to-one acquisition and one-to-zero release. Exact execution at both entry
values includes their scalar guard helpers: the wrong entry must terminate via
actual libc abort, rather than return. Full-width token uses, function-address
uses, callback roots and external effects must be closed. The rule relies on the
SV-COMP complete named-atomic-body convention; it grants no mutex semantics to
an arbitrary function name or C11 relaxed flag.

A finite per-thread ownership pass starts every root unheld. MAY ownership joins
by union; acquisition requires certainly unheld and release certainly held.
Helpers return their actual possible ownership states, loops reach a fixed point,
and every source access intersects its held fact over all reachable contexts.
A root that returns while held leaves the token occupied; thread exit does not
release it. Unknown effects, foreign release, ambiguous ownership or incomplete
proofs refuse the affected certificate. Two exact direct-global accesses
that must own the same certified token can be excluded. A separate rule orders
an exact atomic transition's token store with a full-width ordinary token read
that must hold that same token: another thread cannot perform a successful
acquisition or release during this ownership interval. Transition bodies are
not marked held, and unheld observations or ordinary token writes receive no
exemption. Volatile storage supplies no synchronization; the proof still needs
the actual atomic body and ownership. Integer extension/truncation inside the
transition is evaluated at both entry values, including byte-stored Boolean
guards. This neither makes caller
statements atomic nor exempts unprotected accesses before or after the interval.

Finite arena reservations can consume actual events from this completed token
certificate. They still require exactly one acquisition and release around every
counter read, capture and update, and the original complete finite counter
closure and byte-range proof. A balanced release/reacquire protocol is therefore
insufficient for a reservation. These events retain their token identity and are
not represented as native pthread mutexes.

Sealed once-initialization gates can also consume these actual token events.
Their exact state-zero/state-one prefixes and MUST post-gate facts remain
independent obligations. Token gates consume the producer's complete actual
event set, including operations with no pointer argument; the gate still needs
one physical acquisition and a closed release set. A prefix may call a defined
void helper with one actual 32- or 64-bit integer constant. Native and AR actual
bits must match, and an initialization through its formal must store that exact
AR formal. A helper invocation before or after the initializing prefix cannot
reuse its prefix-only binding: the whole-program phase check still checks every
invocation. Volatile state or payload accesses keep these same obligations and
supply no synchronization. A narrow payload helper binding additionally accepts
actual defined `void(D*, D, D, i32*)` bodies: all calls must pass the same exact
32- or 64-bit global as D and a closed caller-private i32 output slot. Every
pointer use, full-width source/AR effect and complete helper body is checked.
The same physical call may repeat in a retry loop; the output is private, not
an immutable ticket. Post-gate facts intersect over all calling contexts,
including pre-gate invocations. This binding grants no atomicity and never
orders ordinary later/later conflicts. Ambiguous aliases, escaped outputs,
partial effects or another actual payload refuse the binding.

</details>

<a id="closed-gnu-binary-semaphores"></a>
<details>
<summary>Closed GNU binary semaphores — prove exclusion under a restricted runtime contract</summary>

A shared `PosixSemaphore` description covers an aligned, zero-initialized,
global `sem_t` in the checked GNU/Linux i386 or x86-64 profile:
16 bytes/alignment 4 on ILP32, 32 bytes/alignment 8 on LP64. LLVM and AR
allocation/store extents must agree. It requires one actual
`sem_init(&object, 0, 1)` in main's entry block before all helper/worker entry,
exact source/AR calls, and closed object uses through `sem_wait`/`sem_post` only.
Numerical transfer forgets the full verified native object and publishes the
same byte extent as interference;
results remain TOP. The checker omits only this sealed opaque control storage,
whose representation cannot be observed or changed by application accesses.

Normal return from untimed wait acquires a permit in the supported modern Linux
no-user-handler profile. This is supported by [glibc 2.39 sem_wait](https://github.com/bminor/glibc/blob/ef321e23c20eebc6d6fb4044425c00e6df27b05f/nptl/sem_wait.c),
its [slow path](https://github.com/bminor/glibc/blob/ef321e23c20eebc6d6fb4044425c00e6df27b05f/nptl/sem_waitcommon.c),
and the Linux v6.6 [untimed futex](https://github.com/torvalds/linux/blob/v6.6/kernel/futex/waitwake.c)
and [signal restart](https://github.com/torvalds/linux/blob/v6.6/arch/x86/kernel/signal.c)
paths. GNU ABI selection is a model selection, not a probe of the executing
kernel. Generic POSIX EINTR behavior is not assumed away: user handlers,
cancellation/setxid APIs, timed/try waits, foreign runtime data observations,
unknown calls and escaping callbacks refuse this profile.

`BinaryTokenOwnership` reuses its existing finite U/H CFG and helper fixed point:
wait requires U and yields H; post requires H and yields U; merges retain both
possibilities and each access must be held in every reaching context. Initial
capacity one and these balanced transitions establish mutual exclusion; an extra
post, omitted wait or mixed-held branch cannot establish the theorem. Returning
while held retains occupancy. Only pairs with both endpoints certified held are
excluded; semaphore events are not exported to arena or once analyses.

A separate malloc-derived handle array can be admitted to this token inventory
when pointers flow only through casts/GEPs, exact NULL-attribute create outputs,
whole handle loads used only by NULL-output joins, and base free. It retains the
existing object-based pthread effect envelope: heap outputs are havoced, unknown
handles grant no HB, and normal checker accesses remain. This does not establish
allocation bounds, nonwrap, nonnull, handle validity or join completion, and does
not certify concrete libc behavior for arbitrary invalid handles. Native handle
range certificates and their checks are unchanged.
A main allocation may follow size guards in earlier blocks only if its block
cannot execute again. Its NULL comparison must match the shared native/AR
pure-abort continuation; the failure path remains present and grants no success
fact. The complete cast/GEP/load/create/join/free use closure remains required.
All function address uses are checked before an unused non-main definition is
omitted from the executable effect inventory.


Implementation: `analysis/posix_semaphore.hpp` and its source, numerical transfer,
and `src/checker/binary_token.cpp`. Run `python3 scripts/test_posix_semaphore.py`
for dual-ABI int/long-long, checked allocation, branching, capacity, lifecycle,
alias, TLS and API controls, including LP64 upper-half storage accesses.

The separate finite BMC successful-prefix contract supports the same two ABIs.
It keeps int results, pshared and permit counts at 32 bits. Concrete waits must
fit the real preexisting credits, with actual initialization and creator events.
The exporter records native API attributes, signature, target and control-object
layout; independent replay checks these against the AR program before consuming
credits. Run scripts/test_bmc_semaphore.py with --bits 32 and --bits 64 for real
executions and premise-damage controls. Neither effect coverage nor bounded
failure establishes general safety.

</details>

<a id="monotone-ticket-ownership"></a>
<details>
<summary>Monotone ticket ownership — track unique reserved and held access rights</summary>

`BinaryTokenOwnership` also checks a separate ticket certificate. It never exports
these permissions through the binary-token event interface used by arena or once
proofs. The current subset uses two zero-initialized unsigned 32-bit globals:
issued tickets `T` and the current service index `S`. A complete actual named-atomic
issuer must reject `T == UINT32_MAX`, write the previous `T` to a private scalar
outcell, then increment `T` once. The guard's actual scalar helper is interpreted
on input zero over its complete returning control flow; its name is irrelevant.
Native/AR addresses, widths, value origins and unsigned additions must match.

The inductive invariant is `0 <= S <= T <= UINT32_MAX`: issued rights have unique
numbers in `[0,T)`, and consumed rights are `[0,S)`. A private reserved right becomes
Held only through the real `ticket == S` exit of a closed atomic spin loop. Its
exit has no bypass predecessor. A release advances `S` exactly once inside a real
atomic section and consumes Held. Since a holder has `S = ticket < T`, its release
also cannot wrap. While that right remains Held, no different right can acquire
or release the same service number. Actual atomic serialization, rather than
volatile accesses or interleaving order alone, orders the protected accesses.

A finite MAY worklist tracks None/Reserved/Held through every branch, loop and
actual helper return. Endpoint observations intersect all main/thread/helper
contexts; normal root exits must have no live right. Repeated issuances require
a consumed previous right and a fresh actual outcell write. Arbitrarily many
successful thread creations and synchronous calls remain separate activations;
no thread count or ticket range is enumerated. Creation failure creates no right.
All ticket-cell/counter uses and native/AR memory/call effects are closed. Unknown
aliases, partial writes, stale snapshots, unmatched atomic scopes, extra writers,
signed or unsupported counter widths, recursion and limits refuse this proof.

Run `python3 scripts/test_ticket_ownership.py` for ILP32 scalar/wide-payload controls,
including early release, cross-context helpers, repeat issuance, missing atomic
sections, native API identities and overflow boundaries.
Implementation: `src/checker/binary_token.cpp`; these are pair-exclusion facts,
not BMC UNSAT, thread-completion, allocation-success or pointer-validity facts.

</details>

<a id="mutex-protected-scalar-protocols"></a>
<details>
<summary>Mutex-protected scalar protocols — explore protected values and synchronization histories</summary>

`ScProtocol` has a separate protected-mutex mode, with fresh inventory and state;
the existing System-SC atomic mode keeps its own rules. The current subset is
GNU/Linux i386, two distinct single-created roots, isolated global normal mutexes,
whole scalar accesses and a closed native/AR effect frame. An independent prepass
explores every root CFG branch without consulting guard values, intersects the
locks at **all** accesses to each cell, and checks balanced normal returns.
Only consistently protected cells with matching initializer bits and exclusively
literal stores become finite exact registers. Other shared reads remain TOP.
Helpers must be memory-, synchronization-, thread- and callback-effect free.

The finite kernel retains saved guard reads, actual mutex owners and causal tags
published by unlock and imported by the next lock. Ordinary value communication
or interleaving order supplies no happens-before edge. A pair is excluded only
after the complete reachable-state worklist contains no unordered target events;
limits or unsupported effects retain the candidate. Endpoint blocks are acyclic
and bound to actual root accesses and whole native/AR widths. This proof does not
use bounded BMC UNSAT, assertions about payload values or allocation assumptions.

The lifetime frame requires default NULL-attribute initialization before both
creates, both actual current-child joins before destruction, private unobserved
handle slots, no reinitialization or later sync uses, and only quiescent main
shared reads. Default initialization/normal synchronization and ordinary join
continuation model GNU glibc 2.39 behavior under the stated isolated-storage,
sole-joiner, no-detach/cancel premises; they are not arbitrary ignored-return
POSIX success guarantees. Create failure retains IKOS's existing no-child,
no-HB/output-only unknown-handle envelope, not a claim that invalid real glibc
handles are safe. Abort projections preserve preceding events and supply no
normal completion. No mutex success is inferred from a wrapper return value.

Run `python3 scripts/test_protected_protocol.py` for protocol, lifecycle,
ALL-access protection, source API and ILP32 wide-integer controls.
Implementation: `checker/sc_protocol.hpp` and `src/checker/sc_protocol.cpp`.

</details>

<a id="serial-dynamic-thread-lifecycles"></a>
<details>
<summary>Serial dynamic thread lifecycles — check when child activations cannot overlap</summary>

`ThreadLifecycle` preserves a finite relation between actual scalar control,
helper returns and an outstanding current child. Creation includes both an
existing joinable child and a failure with no child and unknown handle bits.
An owned handle can be reused only after its prior child was consumed; saved,
partial, escaped and foreign handle flows refuse certification. Each admitted
join consumes the actual immediate full-width slot load. Every non-NULL result
must be a private, live, unread pointer-sized local object; its value stays
unknown. The first subset uses global scalar native handle slots and a closed
acyclic spawn tree with at most one outstanding child per parent activation.

CFG and helper results join by a set of possible states. Loops run to a finite
fixed point; work exhaustion emits no proof. An absence fact intersects every
state and calling context of the actual access. Child exit summaries are checked
bottom-up at every real Return, so a parent's join can discharge descendants
only when its child always drains them. The same argument serializes repeated
activations without changing the ordinary thread-uniqueness or joined digests.
Payload reads stay unknown. Exact ordinary source/AR effects remain in the
checker; implicit call effects do not consume these temporal certificates.

The ignored-return completion rule models ordinary GNU/Linux i386 join as in
[glibc 2.39](https://sourceware.org/git/?p=glibc.git;a=blob_plain;f=nptl/pthread_join_common.c;hb=glibc-2.39):
an existing unconsumed joinable child, one parent joiner, no detach/cancel,
parent-held locks or waits outside the acyclic parent-to-child topology.
This is narrower than a portable POSIX guarantee for every ignored-return join.
A no-child join keeps IKOS's existing no-HB, output-only unknown-handle envelope;
it supplies no successful-join edge. That envelope is an explicit library model,
not a guarantee about real libc behavior for arbitrary invalid handles.
Default NULL-attribute mutex calls are confined to disjoint opaque global storage;
initialization/destruction require quiescent descendants. This proof supplies
no mutex-success or mutual-exclusion fact. Unknown effects, callbacks, aliasing,
incomplete normal exits or additional creators keep the candidate.

A separate `independent-scalar-thread-completion-v1` certificate supports private
main-local scalar handles selected by the opaque storage inventory. Each slot
has one acyclic actual Create, one exact full-width Load and one sole current
Join. Native and imported CFGs independently require Create before Load before
Join. For each actual creation family, a main endpoint must be before its
Create or after its Join in both CFGs. Two worker endpoints require every
distinct pair of their creation families to be ordered by a Join before the
other Create. The check covers all families when an access record merges
several instances of the same entry; it never uses just the first instance.
Several children may be outstanding. Their payload loops, heap accesses and
synchronous helpers do not need bounded termination: a successful existing child
has completed whenever its ordinary GNU join returns. A failed create contributes
no child access and retains the explicit no-child/output-only envelope above.
Opaque numeric values, API outputs, joined digests and ordinary uniqueness facts
remain unchanged.

This mode has a separate closed source/AR call graph and covers all inventoried
thread instances. It rejects nested creators, repeated creation of a slot or repeated joins,
detach/cancel, callbacks, unknown effects, main-held locks or atomic regions, and
non-NULL join outputs. Completion applies to a mapped implicit library memory
access as well as a mapped Load or Store. It does not order parallel children
merely because both were created by main, remove heap MAY aliases, or prove
pointer validity. Missing scope, source correspondence or work budget emits no
certificate. Run `python3 scripts/test_scalar_thread_completion.py` for the
separate controls; `--bits=64` selects the explicit LP64 run.

An additional `indexed-native-handle-completion-v1` certificate covers one closed
heap array of native handles: a complete zero-based unit-step Create loop and a
subsequent complete Join loop use the same immutable count and the same cell
coordinates. Source and AR independently bind the loops, native APIs, allocation,
full-width slots and post-Join endpoint. A real non-NULL continuation must
precede the loops, and final AI replay must prove the same allocated heap object
is live. Escapes, overwrites, extra activations, competing joiners, detach/cancel,
parent-held locks, non-NULL Join outputs and unknown effects refuse the proof.

The count range is established before the actual allocation-size multiplication.
Existing AI invariants supply the range and may supply a MUST-true Boolean bound;
the latter is consumed only after checking its complete source/AR comparison,
actual count binding and value-preserving definitions. No helper name supplies a
range. The reduction is local to this certificate, without changing AI state.
For address width w and handle stride s, the checked range satisfies
0 <= s*N < 2^w. For i,j in [0,N) and cell bytes u,v in [0,s), the magnitude of
s*(i-j)+(u-v) is less than 2^w; equal modular coordinates therefore identify the
same index and byte. All integer adapters and no-wrap steps are checked over
the final aggregate count range. No implicit PTRDIFF_MAX allocation cap or new
undefined-behavior pruning is used.

Only direct main Load/Store endpoints after the complete Join loop can be ordered
against direct endpoints of the child entry. Parallel child instances are never
ordered with one another by this certificate. Ordinary GNU conditional completion
and failed-create/no-child effects retain the explicit library model above.
Stable complete analysis and strict effect coverage remain required; the proof
grants no pointer-validity, numeric-handle, BMC or FALSE permission.
Run `python3 scripts/test_indexed_join_family.py` for positive, negative and broken
premises; `--bits=64` selects LP64. A nonnegative i32 count already fits an LP64
handle extent, so weakening its size guard need not disable the LP64 proof.

Run `python3 scripts/test_thread_lifecycle.py` for ILP32 serial-loop, transitive,
failure, stale-handle, helper-context and wide-payload controls.
Implementation: `checker/thread_lifecycle.hpp` and `src/checker/thread_lifecycle.cpp`.

</details>


<a id="closed-pthread-once-callbacks"></a>
<details>
<summary>Closed pthread_once alternatives — retain all callbacks under one control</summary>

On GNU Linux ILP32 and LP64, a finite closure can resolve a whole global callback
pointer slot whose initializer and all stores name defined ABI-compatible
void(void) functions. LLVM/AR bindings, full loads/stores, all address uses and
the actual pthread_once declaration are checked. Slot reads and writes remain
ordinary accesses; concurrent replacement is still checked for races.

The whole global once control must start at PTHREAD_ONCE_INIT, have no byte
aliases, writes or escapes, and be used only by checked once calls. Every possible
callback and direct helper must have a closed call-effect tree: cancellation,
exit, longjmp, recursion, indirect/foreign calls and nested synchronization refuse
this first closure. All alternatives remain discovered MAY writers. The pass
does not select a callback or transfer one alternative's MUST value summary.

A restricted control-pointer closure also retains a finite MAY set of static
zero-initialized controls through exact SSA pointer merges or main-only plain
pointer slots. Each possible control registers its own real control identity.
A multi-control call adds no MUST join, including when one callsite executes in
a loop. Only a definite singleton receives the existing completion join. Source
closure remains separate from actual path selection and does not transfer a
pointer value or grant a new nonnull or memory-validity fact. Unsupported uses,
unknown writers, pointer cycles and slot escapes retain conservative coverage.

Only two callback roots whose final complete instance sets contain the same
single sealed control instance can use the new pair exclusion. Ordinary calls
keep their actual thread root; another control, ordinary spawn or ambiguous
instance refuses the exclusion. Discovery and sealing invalidate the shared
fixpoint. Existing completion and strict coverage checks remain required.
This SAFE certificate adds no permission to confirm a FALSE execution.

</details>

<a id="one-shot-barrier-phases"></a>
<details>
<summary>One-shot GNU barriers — opposite phases with complete participant bounds</summary>

The first subset uses whole global GNU barrier objects, with the i386 20-byte/4-byte
and LP64 32-byte/8-byte layouts. The source and AR must agree on the actual
init/wait declaration, object, NULL attributes and the actual unsigned i32 capacity.
Capacity is a finite conservative set from closed acyclic SSA: all PHI/select
alternatives and all supported integer-operation combinations are retained.
Unknown inputs, unproved cycles, poison, unsupported operations or finite-work
limits refuse the complete certificate. A two-block unsigned i32 unit-decrement
loop with literal start and positive bound may supply its complete small
`[bound, start]` interval. The actual PHI, strict unsigned comparison, backedge
and checked arithmetic must match; no termination, `mustprogress` or exit-value
assumption narrows the set. Each object keeps its own init-time snapshot.
A count may also load a whole i32/i64 struct field from a static main stack
object with closed native/AR uses and matching allocation, typed field offsets,
values, widths and alignments. The load precedes all creates. A bounded LLVM
query only locates the same-block ordinary Store of the exact same type and
field; no intervening memory operation or call, old-load reuse, byte extraction,
cast alias, escape, volatile/atomic access or lifetime restart is admitted.
The original accesses remain in the checker, and later stores do not change
the loaded SSA snapshot.
The accepted capacity range is 1 through 2147483646, following
[glibc 2.39 initialization](https://github.com/bminor/glibc/blob/glibc-2.39/nptl/pthread_barrier_init.c).
This implementation contract supplies successful initialization; portable POSIX
initialization may fail for additional reasons. A unique init must
dominate every main wait and every create. Copies, byte aliases, escapes,
reinitialization and destruction refuse this subset.

Roots have no helpers, hidden callbacks or additional creators. Every create,
init and wait site must be outside every CFG cycle, including irreducible cycles;
ordinary memory loops remain in the analysis. Every main create site executes
at most once; multiple creates of the
same worker count as separate instances. Each root has at most one static wait
per barrier. Let M be one plus the number of create sites and C the capacity.
Only a minimum possible C >= M can order opposite phases. A post-wait access can occur only if at
least C arrivals have occurred; the closed program supplies at most M arrivals.
Thus every possible instance has arrived before such an access. An access that
dominates its own wait precedes an access dominated by the other wait's return,
including instruction order within one block. Both endpoints must bind actual
source/AR memory instructions and their direct thread roots.

The library-effect certificate covers only opaque barrier storage, retaining
ordinary shared interference and unknown integer return values (including the
serial-thread return). It is separate from `one-shot-barrier-phases-v1` pair
exclusion: capacity below M can have modeled effects without any phase proof.
Capacity above M only licenses the conditional pair proof, never deletion of
threads or a completion claim. Pre/pre and post/post accesses remain candidates.
BMC barrier execution remains unsupported, and neither certificate authorizes
FALSE, nonnull, pointer validity, join or numerical publication facts.

Run `python3 scripts/test_barrier_phases.py` for dual-ABI, wide-payload and
broken-premise controls. The shared narrow integer fold and actual source/AR
value binding also have a direct unit check. Other memory-loaded capacities,
unproved loop capacities, repeated rounds and helper phases need additional checked facts before extending
this subset.

</details>

<a id="protected-integer-values"></a>
### Protected integer values in abstract interpretation

The existing mutex-value channel also tracks finite integer leaves in static
structs and arrays, keyed by global object, byte offset and integer width.
Registration requires a complete access closure: ordinary full-width loads and
stores at exact constant offsets. Pointer escape, dynamic offsets, overlapping
byte accesses, volatile/atomic accesses and unsupported effects retain the
conservative memory behavior. Padding is never a value cell.

Initial values, all intermediate writes and release endpoints remain separate.
An aggregate read uses field values only when one actual mutex is common to the
reader and every discovered writer. A new writer lock partition invalidates the
shared fixpoint even when its value is unchanged. Unsupported writes invalidate
feedback; first acquisition and later acquisition distinguish handoff values
from interference while waiting. The value board carries per-cell intervals;
the finite equality extension below supplies joint handoff facts. Neither
channel authorizes BMC confirmation.
`IKOS_DISABLE_MUTEX_FIELD_VALUES=1` disables aggregate registration for diagnostic
ablation while retaining scalar feedback and the normal conservative fallback.

Run `python3 scripts/test_protected_field_values.py` (also `--bits 64`) for value,
initializer, intermediate-write, lock, alias and byte-effect controls, plus
relational positive/negative controls using the existing Polka polyhedra domain.
These distinguish local reconstruction, same-critical-section relations and
cross-thread joint equality, including broken-premise controls.

The joint extension tries at most 64 exact leaf pairs per global, requiring the
same integer width and signedness. A candidate starts only when the initialized
memory proves equality, or a single complete typed zero initializer directly
establishes it under the selected initialization policy and an executed-global
initializer closure that permits only complete stores to each initializer's own
global. Native memory otherwise loses subcell values of aggregate-zero
storage; no equality is inferred just from matching marginal intervals.
At each actual mutex release, both fields are read from
one copy of the native numerical state; inability to prove equality permanently
revokes that candidate for the released mutex. Loss dirties the shared fixpoint
even if both interval marginals are unchanged. Facts join by intersection, not
by the last observed release. A release of an unrelated nested mutex does not
revoke the protecting mutex's candidate.

Only a proven first-ever acquisition of a precise ordinary mutex can import a candidate.
That same actual mutex must protect every ongoing writer of both fields. Fresh
cell values cover initialization, all writes and releases; eligible equalities
are imposed jointly before writing them back to native memory. This breaks
stale links to values captured before acquisition. Local stores then transfer
the relation normally: equality is never reasserted at each load. Subsequent
acquisitions, including genuine reacquisition and a new mutex nested under an
earlier acquisition, retain conservative behavior: absence from a MUST lockset
does not prove that ERRORCHECK cannot fail on an already-held branch.
Values captured together may keep their
relation after unlock; separate acquisitions cannot equate two private snapshots.

These are finite inductive candidates, not assumptions justified by observed
successes: initialization establishes the base, every covered critical-section
release checks preservation, and all writes must obey the common mutex. Unknown
effects, an unprotected writer, failed preservation or incomplete analysis
remove the permission. Every invalidating change replays all discovered roots;
verdict publication still requires the existing stable, complete analysis gate.
The native numerical domain determines which relations survive local joins.
This is bounded relational mutex privatization, not a new numerical domain or a
projection of relations into BMC. `IKOS_DISABLE_MUTEX_FIELD_EQUALITIES=1` disables
only equality registration for causal comparison, retaining field intervals.

<a id="initialization-publication-pair"></a>
### Initialization before a concrete mutex publication

InitPublication is a separate pair certificate from FirstPublication.
It binds one live, nonrepeated malloc generation and the exact dynamic helper
context. An unlocked initialization must occur after allocation and before the
first escape in the same closed creator prefix. A full pointer publication and
the reader's pointer load must address the same single heap slot while holding
the same concrete mutex; the prefix must release it after publication.
Every possible slot writer is checked, and the initial NULL or earlier seed
must precede all direct creates. Unknown effects, partial slot accesses, pointer
representation changes, free/realloc, repeated allocation, an escaped prefix,
or missing helper provenance retain the candidate. The finite work budget can
only refuse a proof.

Offsets and full pointer widths come from the target layout, supporting the
checked ILP32 and LP64 paths. This rule grants neither pointer validity nor
arbitrary graph ownership and cannot authorize FALSE. Its direct rule-hit
controls, including early-publication and missing-lock variants, are in
scripts/test_init_publication.py; FirstPublication tests are not substitutes.

<a id="constant-main-thread-completion"></a>
### Completion of constant-control main children

ConstantThreadCompletion is independent of the dynamic ThreadLifecycle
protocol. It interprets a closed, finite constant-control main over private
native handles, assigning a separate identity to every actual create occurrence.
A function is inactive before an access only when all its covered occurrences
have joined, or none has yet been created there. Overwritten handles leave the
old occurrences live; joining one member does not complete a whole family.
Unknown branches, handle observers, unsupported effects, cancellation/detach,
nested joins, or exhausted work discard the completion result.

Native handle slots and integer conversions are checked for the chosen
GNU/Linux ILP32/LP64 ABI. This is a sufficient completion certificate, not
bounded-UNSAT safety and not a guarantee that grandchildren completed.
scripts/test_constant_thread_completion.py exercises actual inactive-child hits
on both ABIs and keeps partial, stale, truncated and newly live generations.
