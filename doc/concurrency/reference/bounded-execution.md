# Bounded execution contracts

[Concurrent analysis documentation](../../CONCURRENCY.md)

### Bounded executions

AI and BMC have different coverage responsibilities. A fact sufficient to exclude
all conflicts is not necessarily a concrete value source for a BMC execution.
The strict coverage gate must pass before BMC may confirm a race. Registered
unresolved obligations and safety-only replacements with remaining candidates
return UNKNOWN even when an older execution model could confirm a trace.
These contracts describe the source checkout; an archived release retains its
own recorded source and execution-checker versions.

### Search sequence and shared budget

The scheduler in [data_race_search.cpp](../../../analyzer/src/checker/data_race_search.cpp)
uses up to thirteen ordered passes. Later passes visit only deferred, applicable groups;
they are not independent complete analyses or independent time allowances.

| Pass | Work considered |
| --- | --- |
| 0 | Initial short searches for the collected candidate groups. |
| 1 | Deferred initial groups, including newly established short activations. |
| 2 | Deeper loop/startup alternatives, retaining earlier short startup prefixes. |
| 3 | Selected dynamic create occurrences and supported finite allocation-size samples. |
| 4 | Larger startup bounds suggested by literal loop structure. |
| 5 | Main/worker searches requiring actual auxiliary-child completion. |
| 6 | Supported finite semaphore startup prefixes. |
| 7 | Exact later access ordinals across the supported retained execution profiles. |
| 8 | Timed main with zero or one completed provider and two selected workers. |
| 9 | Exact later access ordinals in the timed main/provider profile. |
| 10 | Timed main/provider prefixes ending at an actual creator mutex release. |
| 11 | Later target access ordinals in those creator-release prefixes. |
| 12 | Sealed once callback executions on their actual physical caller threads. |

All passes share the file deadline. Candidate/query counters and structural
limits are separate: the existing early retries reset their counters, while the
later exact-target/provider searches use the remaining allowances. An earlier
confirmation can stop later retries. Exhaustion or an unsupported combination
leaves the search inconclusive; neither proves SAFE. This bounded schedule makes
no claim to exhaust every candidate occurrence, path or interleaving. The
[budget guide](../user-guide/benchmarking.md#resource-limits) distinguishes these
limits from process CPU and wall limits.

<a id="bmc-thread-instances-arguments-and-heap-handles"></a>
<details>
<summary>BMC thread instances, arguments and heap handles</summary>

For supported main/worker candidates, BMC can retain finite auxiliary child
executions through their actual returns before a successful join. Those children
contribute their reads, writes and synchronization to the same concrete execution;
a completion edge alone cannot replace their effects. The fallback requires
supported handle generations and rejects nested child creation/join, unsupported
handle storage and atomic targets whose completion suffixes it cannot retain.
The existing complete-path search for atomic targets remains available.

The inert-child fast path traces an integer join handle back to its actual load
and checks earlier uses of that slot. An unresolved handle or a previously
consumed slot cannot establish another successful join. The concrete AR checker
also tracks whether each created instance has been joined or detached. BMC can
model a successful detach of a still-joinable instance identified through private,
native-width main storage. Reusing the slot for a new create gives a new instance;
it does not make the old instance joinable again. Unresolved or escaped handles
remain unsupported.

BMC also supports a native-pointer-width integer passed through a thread's
`void*` argument and converted back to an integer. The value comes from the
definition executed before that particular create, including repeated loop
occurrences; signedness changes preserve the bit pattern. The concrete AR
checker treats this as a numeric pointer and grants no permission to dereference
it or perform pointer arithmetic. Object-pointer conversions and non-native
width round trips remain unsupported. This is a contract for the imported AR
and configured LLVM target, not a portable C pointer-representation guarantee.

For an uncalled C `main(int, char**)`, BMC can choose the legal input `argc=1`
and retain its actual guards; an incompatible explicit `--argc` setting prevents
this choice. This does not restrict abstract analysis or justify a SAFE result.
A supported variable-length stack array is allocated once in main before thread
creation, with a positive path-constant count and at most 1,000,000 bytes. Counts
may use private scalar globals only when their complete address-use closure
excludes worker writes and earlier executed calls have not received the address.
The SMT encoding checks the actual allocation operand, every intermediate
inbounds pointer shift, and typed, aligned, in-range thread-handle outputs.
The independent AR checker recomputes the count and repeated element bounds.
Only a genuine LLVM stack-save intrinsic is admitted, as an opaque token;
any executed stack restore, repeated allocation or unresolved extent keeps the
execution unconfirmed. These checks reuse the existing path and object model.

BMC can search a single main allocation whose size depends on one genuine
integer nondeterministic source. After the established path search fails, it
tries source values 1 through 4, retaining the real loop guards and binding the
same value to the allocation extent in SMT. These are search samples, not a
restriction on the program's possible inputs or evidence of safety.
The supported heap handle storage remains private to main: native-width,
aligned create outputs must fit the allocation. A supported `free` consumes
that allocation's base exactly once; publication, integer address export,
unknown pointer derivation, interior or repeated free, and subsequent use
leave the execution unconfirmed. Actual intermediate inbounds pointer shifts
are checked as well. The AR checker independently checks allocation size,
create output identity/alignment, free identity and lifetime. Supported heap
join loads bind the actual load occurrence to the real create output and its
joinable instance; copying an opaque handle does not establish a new creation.
The [repeated-allocation profile](#repeated-main-heap-allocations) below retains
separate identities when the same allocation site executes more than once.

</details>

<a id="atomic-targets-and-communication"></a>
<details>
<summary>Atomic targets and communication — retain the required execution suffixes and ordering</summary>

When either selected access is inside a verifier atomic region, the exported
execution retains both target roots through completion. The independent AR
checker also requires both roots to finish, so an atomic-end marker alone
cannot hide a later assumption by truncating the trace.

Absence of pthread synchronization does not exclude communication between
verifier-atomic sections. BMC first tries its conservative adjacency
conditions for mixed atomic/plain targets. A final narrow query can instead
prove that the actual target occurrences are unordered in a complete event
graph: program order, actual create-to-entry, returned-child-to-consuming-join,
and the schedule order of every complete verifier atomic section. Ordinary
read-from constrains values without adding a synchronization edge.

This query requires real completion of every retained timed root and helper.
It refuses native atomics, mutexes, rwlocks, conditions, semaphores, barriers,
once, stdout, other opaque external effects, and the startup-provider profile.
The existing source coverage, value, memory and indivisible-section checks still
apply. Two Boolean sets closed under all graph edges witness mutual
unreachability; no graph certificate is exported or trusted by replay.
The old queries run first; a structurally impossible interior adjacency pair
can skip directly to the additional query. It shares the existing deadline and
solver timeout. `IKOS_BMC_DISABLE_ATOMIC_UNORDERED=1` disables only this query;
4096 retained events and 64 complete atomic sections are structural refusal
limits. No unsuccessful query establishes SAFE.

The AR checker also tests ordering
under a stronger relation that serializes complete atomic sections; a pair
ordered only by this additional relation is `UNSUPPORTED`, not certified as a
race. This is a conservative acceptance condition, not an implementation of the
complete CAT memory model. External calls inside an atomic section are limited
to supported nondeterministic inputs, assumptions, and `ffs`; ordinary helpers
must be expanded. Other library effects require further communication modeling.

</details>

<a id="exact-ffs-values-in-bmc"></a>
<details>
<summary>Exact ffs values in BMC — compute the lowest set bit of the actual argument</summary>

BMC models the POSIX `ffs(int)` library declaration with its exact 32-bit
argument: zero returns zero, otherwise the result is the 1-based index of the
least significant set bit. The independent AR checker computes that result
from the replayed argument. Definitions with the same name execute their
actual bodies; incompatible signatures and calling conventions are unsupported.

</details>

<a id="startup-heap-pointer-fields-in-bounded-executions"></a>
<details>
<summary>Startup heap pointer fields in bounded executions — bind loads to actual initialization stores</summary>

The BMC address resolver can follow a full-width object-pointer Store into a
fixed heap field before the first thread creation. It uses the last actual Store
occurrence, including a prior NULL initialization, and resolves its value in that
call's dynamic context. Every retained trace is then checked: a later overlapping
write, symbolic alias, partial write, or library output makes this route
unsupported. The rule does not model mutable pointer read-from or allocations in
worker threads, and keeps the existing activation, lifetime and atomic contracts.

A certified pointer Load exports its allocation instance, extent and byte offset.
The independent AR checker compares them with the pointer obtained from its own
byte-memory replay and also verifies the actual read-from source. This validates
the selected AR prefix, not a C-source witness. Run
`python3 scripts/test_initialized_pointer_fields.py --bits 32` or `--bits 64`
for native-layout int/long-long controls, alias/output corruption and damaged
execution evidence.

</details>

### Later dynamic access targets

A static candidate can select a later execution of the same memory instruction.
Access ordinals count real occurrences in the complete root trace, including
expanded helper invocations; they are separate from thread-create generations.
The exact-target pass follows the earlier activation/completion/semaphore
searches; a later provider-specific pass composes exact targets with timed main
execution. Both use the remaining file deadline and candidate/query budgets.
They preserve the existing helper alternatives rather than changing their
interpretation.

The same requested occurrences pass through loop retries, finite input samples,
atomic-root completion and main/worker auxiliary completion. Exported targets
identify actual trace steps; independent replay checks the executed definitions,
byte addresses, read-from sources, synchronization and lifetimes. Exhausted or
unsupported searches stay inconclusive and grant no SAFE permission.

Run `python3 scripts/test_bmc_later_accesses.py --bits 32` and `--bits 64`
for later direct/helper targets, distinct create generations, required completion
suffixes, boundary controls and damaged selected executions.

### Timed main and a completed provider

The supported profile retains one bounded main timeline, zero or one provider,
and two selected target workers. Actual creates, the provider's memory effects
and Return, exact handle-generation joins, main reads and guards, and normal
mutex operations participate in the same execution order and read-from encoding.
A join-completion edge cannot replace the provider body. Later target occurrences
use the same exact-access frontier as the other supported profiles.

Execution export and independent AR replay retain the timed roles and canonical
event identities. Source mapping preserves the selected schedule and reports
UNKNOWN for its declared registration-order projection limit; damaged source or
execution evidence remains an error. The legacy GraphML adapter rejects this
profile. Provider plus heap is currently unsupported. The
[provider and combination controls](../developer-guide/testing.md#timed-main-and-provider-execution-controls)
do not substitute for independent source-witness validation.

### Repeated main heap allocations

Before spawning workers, the supported profile permits at most four main malloc
events in total, including repeated execution of the same site. Allocation identity includes the actual
canonical event and site occurrence. Distinct live allocations, a freed early
generation followed by another allocation, and passing a later generation through
supported arguments or handle storage retain separate byte contents, extents,
alignment and lifetimes. `free` consumes only its actual allocation instance.

Every trace is normalized before allocation/create indices become persistent;
read-from and exported targets keep those identities through independent replay.
Exact later access targets can compose with this profile. Concurrent free,
realloc, arbitrary mutable heap graphs, observations of raw-address reuse and
provider-plus-heap execution remain unsupported. See the
[allocation and canonical-event controls](../developer-guide/testing.md#allocation-generation-and-canonical-event-controls).

### Executed constant read addresses

For supported shared main-stack and heap reads, an address index can become a
constant at its actual use when encoding its executed value simplifies to an
integer literal. No solver model is sampled to choose that constant. The original
access remains in the execution evidence and retains its object, lifetime,
alignment, initialization and SC read-from checks. Mutable or unresolved indices
keep the execution unconfirmed under this route.

All retained global, heap and stack GEPs, including fixed stack objects and intermediate
unused definitions, must satisfy the supported native inbounds contract. BMC
checks each intermediate offset and rejects signed index/offset truncation or
overflow. Native LLVM strides and struct field offsets must match the imported AR
terms exactly; the original sequential index widths must match as well. The AR
snapshot exports these original terms, and the independent checker compares them
before performing its own concrete pointer arithmetic. Missing native metadata is
unsupported. One-past formation is allowed; dereference still requires in-range
bytes. These conservative checks do not establish SAFE or source-level witness
validity.

Run `python3 scripts/test_bmc_executed_offset.py` (and `--bits 64`) for native
ILP32/LP64 C int/long-long controls,
raw LLVM intermediate/large-stride/field-offset cases and damaged native evidence.


## Startup cancellation and phase cost

Ordinary and finite-sample startup enumeration use the same file-level BMC
deadline. Reaching a construction deadline, path, event or work limit records
BUDGET_EXCEEDED even when detailed diagnostics are disabled. Fully constructed
prefixes remain eligible for their ordinary guard, value, instance and execution
checks; an interrupted branch never becomes a witness. This status grants no
SAFE permission. Process failures and external resource termination retain their
separate ERROR/resource classifications.

SQLite times include ikos-analyzer.race.finalize, ikos-analyzer.race.pairs and
ikos-analyzer.race.bmc-search. The first includes the latter two and effect
preparation; these nested durations must not be added. Existing per-function
value/check timers measure main AI work; BMC solver_us/evidence_us remain
substage measurements. No per-statement instrumentation or extra proof query is
performed for these timers. Rule counts are first successful consumers, not
exclusive solved-task contributions; field-import logs count attempts rather
than measured abstract-state strengthening.

SymbolicArena uses its own fixed query/state limits, separate from BMC options.
Debug logs distinguish query-budget exhaustion from a solver unknown and include
the solver's supplied reason. They do not raise the existing 100ms query limit
or turn an inconclusive query into a certificate.


### Independent join API checks

Selected AR replay checks the GNU ILP32/LP64 join declaration and call types
before consuming a handle or merging clocks: native integer handle, native
pointer for the NULL output, signed i32 return, nonvariadic C call, matching
argument/result types. Actual generation, joinability and completed child
execution remain separate requirements. Rehashing an inconsistent program
snapshot cannot bypass these API checks. This is selected AR validation, not a
proof of source-to-AR translation or a source witness.

### Creator release endpoints

The timed main/provider profile can end at an actual normal-mutex Unlock in
main after both selected creates. This retains the real release, its preceding
guards and memory effects in the common execution timeline; it does not infer a
release from a lock summary or from a worker's blocked acquisition. A provider,
when present, still executes through its actual Return before a successful join.
The existing dynamic create, handle, read-from and mutex-ownership checks apply.

The release searches run after the earlier ten passes with the same remaining
deadline and counters. At most the first three occurrences of each eligible
main-body release are considered. Helper-only releases and unsupported numeric
pointer coverage remain outside this extension. Disabling the added searches
with `IKOS_BMC_DISABLE_CREATOR_RELEASE=1` preserves the earlier searches and
returns an inconclusive result when they cannot find a supported execution.
Neither unsuccessful search nor an AR-accepted execution grants SAFE.

### Once callbacks on their physical caller

A supported `pthread_once` executes the callback in the actual calling pthread.
The AI callback identity is not a new physical thread or a synthetic create.
The bounded profile retains one actual main, one actual worker create, the API
call, callback entry/body/Return and successful API return. Each exact control
has one active initializer. Passive returns acquire only that initializer's
completed clock; a later passive caller's accesses cannot flow into another
passive return through a mutable completion clock.

The profile requires a static zero-initialized GNU ILP32/LP64 control with closed
uses and a direct native C `void(void)` callback. Native and AR API types, calling
conventions, initialization, callback identity and control uses are exported
and independently checked. Ordinary source definitions named `pthread_once`
execute their real bodies. Unsupported reentry, escaping controls, callback
thread creation, multiple physical creates or verifier-atomic combinations
remain inconclusive. Main's configured argument assumptions are preserved.

A separate finite-control form accepts up to eight whole static controls selected
through pointer-preserving SSA merges and plain main-only pointer slots. Every
LLVM/AR definition, PHI incoming edge, slot initializer/store/load and control use
must close. Slots have one live `main(void)` frame; a local slot must have a
proven dominating initialization. Cyclic origins, pointer arithmetic, NULL or
integer origins, volatile/atomic slots, escaping slots, opaque external writers
and unknown-address writes refuse this form. The selected execution keeps the
actual branches and pointer stores/loads. Each API occurrence resolves its
executed control; its static MAY set cannot choose that control. The independent
checker reconstructs the full AR closure and actual pointer bytes, including
exact static-global address initializers. Global object registration precedes
initialization so dictionary order has no semantics.

`IKOS_DISABLE_FINITE_ONCE_CONTROLS=1` disables only this finite-pointer extension;
the earlier direct-control profile remains available.

The optional final search shares the earlier deadline and counters.
`IKOS_BMC_DISABLE_ONCE_EXECUTION=1` disables only this profile. It grants no
SAFE permission. Independent AR replay validates the physical roots and actual
callback completion; source projection registers only real thread creates and
retains callback targets on their caller's thread. Neither replay nor export
success constitutes external source-witness validation.

### One-shot capacity-two barriers

Supported barrier execution reuses the closed native barrier-object and
whole-program effect checks, then establishes its own successful initialization
and physical participant execution. A real main initializes one static GNU
ILP32/LP64 barrier with NULL attributes and literal capacity two before creating
one worker. Each physical participant waits once on that object. Every arrival
precedes each synthetic departure; both departures acquire an immutable join of
the arrival clocks. Accesses after one departure do not enter the other
participant's release clock and are not mutually exclusive.

The first profile rejects repeated waits on one object, reinitialization, opaque
storage access, missing participants, used API results and unsupported profile
combinations. The unobserved serial-thread result is not exported as zero for
every participant. A departure marker has no API result of its own. Multiple
objects are permitted only when each retained round has its actual two arrivals.
A prefix between two barriers can retain the first round without claiming
completion of the later one. Main Return terminates the process; replay rejects
later worker events.

`IKOS_BMC_DISABLE_BARRIER_EXECUTION=1` disables this execution profile without
authorizing SAFE. Existing barrier safety proofs and their premises are
unchanged. The selected execution, source mapping and external source-witness
validation remain distinct evidence layers.

### Complete shallow alternatives after interrupted enumeration

When a deferred large-loop enumeration is interrupted, the existing complete
bound-two retry remains first. An optional bound-three retry follows it with
the same remaining deadline, query counters and structural limits. It calls
the ordinary confirmation path: real loop guards, dynamic definitions, atomic
root completion, read-from and independent replay are unchanged. An emitted
path must still reach its selected target and any required real Return; an
interrupted branch is never treated as completion.

`IKOS_BMC_DISABLE_FINITE_LADDER=1` disables only the added bound-three choice.
This is a bounded search policy, not a new program abstraction, a completeness
claim or a proof of safety. A zero-gain comparison is a valid observation; no
resource limit is increased to obtain a result.

### Actual calloc allocation generations

The bounded heap profile accepts exact native `calloc(size_t, size_t)` calls in
pre-spawn main. Both actual unsigned operands must have the target ABI width;
their positive mathematical product must fit size_t and the existing 1 MiB
object limit. Closed allocation helpers are expanded through LLVM's ordinary
inliner, preserving their allocation, NULL checks, abort and return behavior.

Each executed allocation occurrence owns fresh all-zero bytes. These supply
integer load sources only for that live object and byte range; actual later
stores retain the ordinary SC latest-write constraints. `malloc` remains
uninitialized. The independent execution checker recomputes operand binding,
native layout, product, generation and bytes. Zero-sized or overflowing
allocations, worker allocations, unsupported publication or lifetime effects
remain inconclusive. This profile adds no SAFE permission.

`IKOS_BMC_DISABLE_CALLOC=1` disables the new allocation execution support.
Independent AR replay and source projection are separate from official
source-witness validation.
