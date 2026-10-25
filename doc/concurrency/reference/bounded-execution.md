# Bounded execution contracts

[Concurrent analysis documentation](../../CONCURRENCY.md)

### Bounded executions

AI and BMC have different coverage responsibilities. A fact sufficient to exclude
all conflicts is not necessarily a concrete value source for a BMC execution.
The strict coverage gate must pass before BMC may confirm a race. Registered
unresolved obligations and safety-only replacements with remaining candidates
return UNKNOWN even when an older execution model could confirm a trace.

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
create output identity/alignment, free identity and lifetime. Heap join loads
still require additional handle-source and generation support.

</details>

<a id="atomic-targets-and-communication"></a>
<details>
<summary>Atomic targets and communication — retain the required execution suffixes and ordering</summary>

When either selected access is inside a verifier atomic region, the exported
execution retains both target roots through completion. The independent AR
checker also requires both roots to finish, so an atomic-end marker alone
cannot hide a later assumption by truncating the trace.

Absence of pthread synchronization does not exclude communication between
verifier-atomic sections. BMC therefore retains its conservative adjacency
conditions for mixed atomic/plain targets. The AR checker also tests ordering
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
`python3 scripts/test_initialized_pointer_fields.py` for ILP32 int/long-long
controls, alias/output corruption and damaged execution evidence.

</details>

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
