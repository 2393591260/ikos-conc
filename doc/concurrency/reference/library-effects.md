# Values and library effect contracts

[Concurrent analysis documentation](../../CONCURRENCY.md)

### Values and library effects

<a id="numeric-thread-arguments"></a>
### Native numerical thread arguments

On GNU/Linux x86 ILP32, a native-width integer may travel as a `void*` payload
without acquiring the ability to address application memory. The producer uses
the existing AbsoluteZero plus offset representation. A bounded static check in
`numeric_thread_argument.hpp` requires closed uses in both LLVM and AR: pointer
copies/bitcasts, actual declared `pthread_create` argument transport, and a
defined worker whose parameter is discarded or converted to a native integer.
Every source creation must match an AR creation with the same worker. Unknown
uses, pointer stores/returns/comparisons/arithmetic, helper or indirect transport,
non-native conversions and unsupported ABIs retain the ordinary alias obligation.

This checks the entire pointer-use graph, including branches and loops. It does
not select a thread instance or constant argument. The abstract interpreter joins
actual spawn values conservatively; BMC separately binds each value to its
executed definition before that creation, and independent AR validation retains
its own bit-pattern checks. Existing byte/unknown-effect obligations and the
strict result gate still apply. This representation contract grants neither
pointer validity nor completion, uniqueness or a final verdict.

Regression: `scripts/test_numeric_thread_argument.py --ikos install/bin/ikos`.

<a id="heap-pointer-field-initialization"></a>
<details>
<summary>Heap pointer-field initialization — recover field values from all possible writes</summary>

The abstract interpreter can restore an initialized heap pointer field in a
worker when a full pointer store precedes every invocation of that worker.
This requires one concrete allocation, an exact byte offset, a direct main
initializer and closed, same-block create ordering. The restored value joins
all pointer writes, including later writes; it is not the initializer's value
alone. Ambiguous destinations, overlapping byte writes and uncovered effects
revoke the refinement and trigger reanalysis. This subset checks a closed set
of call effects: general memory-copy, vararg and opaque output operations fall
back to the ordinary conservative analysis. It does not prove general list
ownership or require pointer fields to remain immutable.

A second initialization certificate covers repeated calls to a constructor:
a single straight-line body allocates one constant-size object, initializes an
exact pointer field to NULL, and returns the base without exposing it earlier.
The interpreter can then constrain field loads using the union of all typed
writes to every possible source, including the initial NULL. Every source must
have the certificate and an available summary. This also works for allocation
sites that summarize multiple objects; it grants no unique object or lock
identity. Integer payload conversions are supported. Branching constructors,
interior returns, pointer export, unknown calls, byte overlap and missing source
coverage fall back conservatively. Lifetime checks remain separate.

Field coverage can also follow a closed initialization prefix in a larger
function: each malloc result reaches full-field initialization in the same
basic block before any read or escape. A direct, checked helper may initialize
the object through one pointer parameter and scalar payload parameters.
A main-global store/reload relay additionally requires an exact local source,
no return to the allocation block, and initialization before every possible
create. The complete field value still includes all sources and later writes;
byte overlap, uncovered effects and unsupported lifetime operations disable the
refinement. This certificate only refines field values: it grants neither
ownership nor exclusion of initializer-versus-published-access race pairs.

</details>

<a id="native-thread-handle-arrays"></a>
<details>
<summary>Native thread-handle arrays — model private handle storage without inferring completion</summary>

For closed Linux GNU ILP32 programs, the abstract interpreter also supports
repeated creates into private, fixed-size main-stack arrays of native 32-bit
`pthread_t` values. It checks the original LLVM and imported AR declarations,
every producer and array use, and independently proves the full output/load
ranges before exporting create-effect certificates into a fresh analysis.
Handle outputs and reads are unknown initialized native values, rather than
creation-site numbers. Joins through these values add no completion or
happens-before facts: neither joining one array element nor overwriting and
joining a handle proves that all instances of a worker have ended.
Escapes, representation inspection, uncovered calls and unproved ranges retain
the conservative coverage boundary. This subset requires NULL attributes,
worker arguments and join-result outputs, and unused API return values.
It does not establish list ownership, initialization or first-publication safety.

</details>

<a id="pure-ffs-effects"></a>
### Pure ffs application effects

The GNU/Linux x86 ILP32 `int ffs(int)` declaration has no application-memory,
errno or synchronization effects. The ordinary numerical transfer returns TOP,
so it retains every possible result and dependent path. This model introduces
no value range or safety exclusion. The [BMC value model](bounded-execution.md#exact-ffs-values-in-bmc)
and independent AR checker separately compute the actual lowest set bit.

The transfer requires matching LLVM/AR declarations and signed 32-bit types,
the C calling convention and a direct ordinary call. Varargs, incompatible
attributes, operand bundles and module inline assembly retain the ordinary
external-effect fallback. Source definitions execute their real bodies. A
constant function pointer resolved to a direct call by the frontend qualifies;
an unresolved indirect call does not. Existing compiler assumptions, including
unreachable code after a noreturn declaration, retain their original meaning.

The [POSIX value contract](https://pubs.opengroup.org/onlinepubs/9799919799/functions/ffs.html)
and [glibc 2.39 x86 implementation](https://github.com/bminor/glibc/blob/glibc-2.39/sysdeps/i386/ffs.c)
support this restricted library summary. Implementation: `try_exec_ffs` in
[`numerical.hpp`](../../../analyzer/include/ikos/analyzer/analysis/execution_engine/numerical.hpp).
Regression: `scripts/test_bmc_ffs.py --ikos install/bin/ikos`, including wide
data, incorrect results, source definitions, ABI and effect controls.

<a id="private-array-zero-effects"></a>
### Private integer-array zeroing

On ILP32, a nonvolatile LLVM `memset` intrinsic can have complete private
application effects for a fixed, one-dimensional stack integer array. The
LLVM allocation and AR allocation must agree on element width, count, extent
and alignment; the exact call must zero the entire allocation. Each actual
transfer additionally requires a nonnull pointer to that local allocation,
offset zero, exact length and no escape. Unknown points-to never qualifies.

Both representations must close all derived pointer uses: restricted
bitcasts/array indexing and ordinary integer loads/stores only. Address
transport, publication, helper arguments, returned addresses, mixed pointer
sources and unsupported lifetime uses retain the ordinary byte-effect debt.
The owner is an independent entry or a worker created directly by `main`;
ordinary calls and reentry are outside this subset. Separate worker instances
may have private stacks at the same static site; singleton abstract identity
does not prove concrete object uniqueness.

The real memory-domain `mem_set` and subsequent content/control dependencies
remain active. This bypasses no other obligation and adds neither a scalar
publication nor a pair-exclusion rule. New element cells can retain TOP values;
this is not a general array-value precision improvement. LLVM/AR translation
and the existing memory-domain transfer remain trusted dependencies.

There is no new BMC or independent AR `memset` execution model. A selected
prefix executing this operation remains unsupported; a prefix that leaves
this worker unscheduled must independently pass the usual execution checks.
AR acceptance does not validate the source privacy contract.

Reference: [LLVM 14 memset semantics](https://releases.llvm.org/14.0.0/docs/LangRef.html#llvm-memset-intrinsics).
Implementation: `private_array_zero_effect` in
[`concurrent_semantics.hpp`](../../../analyzer/include/ikos/analyzer/analysis/execution_engine/concurrent_semantics.hpp),
consumed only by `exec_memset` in
[`numerical.hpp`](../../../analyzer/include/ikos/analyzer/analysis/execution_engine/numerical.hpp).
Regression: `python3 scripts/test_private_array_zero.py --ikos install/bin/ikos`.

<a id="closed-thread-detach"></a>
### Closed private thread detachment

For GNU/Linux x86 ILP32, an ignored `pthread_detach` can have a complete
application-memory effect summary when its native scalar handle stays private
to direct `main`: one creation, its immediately following load and detach,
and at most one ordinary entry initialization. LLVM and AR must independently
agree on the declarations, C ABI, slot extent/alignment, order and every use.
Initialization still executes its usual transfer and coverage checks. A loop
may reuse this sequence; the summary does not expose native handle bits.

The program must contain no join, self/identity comparison or detach-state
observer, including unresolved calls that could hide one. Return-value uses,
duplicate detaches, slot escape, extra accesses, incompatible parameter
attributes, special calling conventions, module inline assembly and unsupported
lifetimes retain the ordinary uncovered-effect obligation. These are conservative support boundaries,
not claims that every rejected program contains a race.

The [POSIX detach contract](https://pubs.opengroup.org/onlinepubs/9799919799/functions/pthread_detach.html)
does not terminate a running thread. This summary changes no lockset,
live-child, completion or happens-before fact and assumes neither successful
creation nor valid/successful detachment for a SAFE conclusion. It does not
model arbitrary invalid GNU runtime handles. A FALSE still requires an actual
successful creation and one unconsumed dynamic handle in BMC and independent
AR validation. The latter also checks the detach signature and C calling
convention; its ACCEPT is not source-witness validation. Other coverage
obligations and the strict verdict gate remain in force.

Implementation: [`closed_pthread_detach`](../../../analyzer/include/ikos/analyzer/analysis/execution_engine/native_thread_handles.hpp)
and the external-call transfer in [`numerical.hpp`](../../../analyzer/include/ikos/analyzer/analysis/execution_engine/numerical.hpp).
Regression: `scripts/test_detach_coverage.py --ikos install/bin/ikos`.

<a id="pthread-keys-and-destructor-effects"></a>
<details>
<summary>Pthread keys and destructor effects — account for thread-local library effects</summary>

The interprocedural interpreter has conservative POSIX pthread-key effect
summaries for Linux GNU ILP32 programs entered at `main`, with no entry-specific
omissions of global initialization or global constructors/destructors. The
original LLVM declaration, call ABI and imported AR callee must agree.
`pthread_key_create` with a NULL destructor
havocs its output and returns an unknown status; `pthread_setspecific` returns an
unknown status, and `pthread_getspecific` returns an unknown pointer including
NULL. Allocation failures are included. Association does not dereference the
stored pointer or publish it to another thread. Actual publication through a
retrieved value retains ordinary escape handling. These summaries confer no
ownership, lock or happens-before facts and do not prove get/set value equality.

Key-output writes still invalidate numeric, pointer and mutex summaries and
participate in race checking. A standalone global key avoids pointer-exposure
havoc only when its complete address/value use graph stays within the verified
key APIs and the runtime destination matches that exact object and offset.
Constant integer pointer tokens avoid the unsupported-address gate only when
every use is a bitcast or the value argument of verified `pthread_setspecific`.
Dereferencing, storing or passing such a token elsewhere retains the gate;
retrieved pointers remain unknown. A separate effect certificate accepts a
non-NULL destructor only when DataRace is the sole requested analysis. Its
actual defined `void(void*)` body and every direct helper are scanned in LLVM
and AR without argument-based pruning. Loads, stores, allocation, synchronization,
unknown/indirect calls and hidden ABI effects refuse the certificate. Pure
computation and control, verified nondeterminism, real abort and constant
assertion diagnostics may remain; aborting or diverging is not treated as
successful completion. The checker records this memory projection separately.
Mixed analyses keep the old callback fallback, and BMC/execution validation
still cannot omit TLS registration or callback executions. This proves no
callback assertion, successful key operation or TLS value identity. The older
source-only output/selector predicates remain NULL-destructor-only. Other
destructors, key deletion, incompatible APIs and intraprocedural pointer
analysis retain conservative coverage boundaries.
See POSIX.1-2024
[key creation](https://pubs.opengroup.org/onlinepubs/9799919799/functions/pthread_key_create.html)
and [thread-specific values](https://pubs.opengroup.org/onlinepubs/9799919799/functions/pthread_getspecific.html).

</details>

<a id="calloc-size-overflow"></a>
<details>
<summary>Calloc size overflow — retain allocation-failure continuations</summary>

`calloc` size overflow preserves the caller's normal failure continuation.
The interpreter multiplies interval bounds of the converted `size_t` arguments
using mathematical integers. Certain overflow returns NULL before allocation,
zeroing or lifetime updates. Possible overflow retains a NULL branch with the
complete preallocation state and joins it with the existing representable-size
transfer, including MUST facts. It cannot silently remove later shared accesses
by treating library allocation failure as arithmetic undefined behavior.
Caller arithmetic such as `calloc(n * 2, 4)` is evaluated before this check;
unsigned wrapping there is distinct from the API's internal size multiplication.
This correction covers overflow paths. It does not establish a general heap
generation or complete resource-failure model for repeated allocation sites.

</details>

<a id="zero-element-artificial-initialization"></a>
<details>
<summary>Zero-element artificial initialization — avoid inserting a seed into an empty array</summary>

The frontend's integer-allocation freeze pass preserves the first-cell nondet
seed for positive constant counts. A zero constant count has no seed. A dynamic
count branches on `count != 0` in the original LLVM count width; only that edge
contains the adjacent, tagged nondet call and Store. Both edges continue with the
original suffix. The pass adds no assumption, abort or zero-length-path pruning.
Existing scalar seeds and positive first-cell coverage are retained. Moving a
suffix during CFG splitting can prevent promotion of a later scalar allocation;
the seed still remains. Array-memory
read correlation remains the responsibility of the numerical memory domain.

This correction proves neither allocation success nor pointer validity or byte
extent. In particular, a raw wide LLVM allocation count can still exceed the
ILP32 importer's count width; this change does not repair that separate boundary.
It also does not certify stdio effects or general VLA safety.

`pass-freeze-uninit` checks both the isolated pass and basic pipeline, including
zero, dynamic, high-bit and wide counts, multiple allocations, loops and PHIs.
`python3 scripts/test_freeze_array.py` checks ILP32 scalar/array values and
continuations with 32-bit and long-long payloads and BMC disabled.

</details>

<a id="formatted-input-coverage"></a>
<details>
<summary>Formatted-input coverage — keep warnings for unmodeled parser effects</summary>

Formatted input can update observable parser state such as `errno` as well as
conversion destinations. Forgetting only pointer arguments can leave a cached
`__errno_location()` pointer at its old value and incorrectly discard an error
branch containing a race. Until a complete applicable summary covers these
effects, mapped `scanf`, `fscanf` and `sscanf` retain an uncovered-effect warning,
including calls whose conversion outputs are private. Raw extern calls keep
their existing conservative handling. This coverage gate asserts neither a
conversion result nor a particular errno value.

`python3 scripts/test_scanf_errno.py` covers raw and mapped APIs, cached-error
branches, unconditional conflicts and a no-input impossible-branch control,
with both int and long-long payloads under ILP32.

</details>

<a id="exact-scalar-string-input"></a>
<details>
<summary>Exact scalar string input — model a restricted sscanf call</summary>

The thread-modular analyzer supports a narrow source/AR-verified string-input
summary: actual declared `sscanf` or `__isoc99_sscanf`, GNU/Linux i386 ABI, an
immutable literal `%d`, and one complete signed 32-bit global or scalar-local
output object. Both raw extern and mapped libc routes use the same descriptor.
The input remains a possibly unbounded Read; the output is a four-byte possible
Write. The immutable format is separately verified. Return and output values
are TOP, including failed conversion leaving an old or uninitialized output.
Every call also forgets the existing errno memory object, so cached errno
pointers cannot retain a stale value. No parsing success or value range is inferred.

Errno accessor addresses must stay in same-function, whole-int load/store uses.
Escaping those addresses or exposing foreign runtime data refuses this summary
and retains explicit coverage even with a private output. Other unsupported
formats use the formatted-input gate above. Locale and unknown-call coverage
remain in force. This summary does not establish stream isolation or remove
`fprintf` effects, and is not added as a BMC or execution-checker leaf.

`python3 scripts/test_scalar_scan.py` checks raw/mapped calls, failure and cached
errno paths, private-output fallback, input/output conflicts, API/format/width
controls, shifted-input and partial-output overlaps, read-lock versus exclusive
mutex protection, and ILP32 long-long payloads. Implementation: `stdio_effects.hpp/.cpp`
in `analysis/execution_engine` and `src/analysis`, consumed by numerical transfer
and the data-race checker.

</details>

<a id="closed-runtime-stdio-frame"></a>
<details>
<summary>Closed runtime stdio frame — check when synthetic stdio effects can be omitted</summary>

An optional, immutable `RuntimeStdioFrame` is configured in each thread-modular
Context before numerical analysis. It inventories the actual reachable LLVM and
AR effects, entry options, source identities and GNU i386 ABI. Only a complete
frame supplies either of its two capabilities:

* A verified `fprintf(stderr, literal, ...)` with only promoted `%d` and `%%`
  conversions may omit its stream argument's synthetic application Write. All
  ordinary accesses and other argument effects remain.
* An exact startup scalar scan may suppress its own synthetic pointer escape
  only for the certified whole global output, checked again against the current
  singleton destination and zero offset. Pointer-write publication, integer and
  mutex invalidation, errno/output havoc and failure coverage remain unchanged.

The first subset admits whole integer storage, sealed serial argv input and
counted private i32 stack pools. Actual signed upper edges, stable counts,
zero/unit induction, native/AR byte strides, physical nonzero seed guards and
stack lifetime must match. Each pool's creation phase must cover every consumed
slot with a successful actual create; failure cannot reach a join. Its sole join
phase consumes each slot at most once. Native header and AR PHI-commit checks
reject repeated occurrences, irreducible cycles and restarted phases. These are
internal runtime-storage premises, not exported thread-success, handle-value,
completion or happens-before facts.

Unknown effects, pointer reconstruction/escape, FILE aliases or mutation,
unsupported formats, partial/foreign accesses, heap objects, missing range or
lifetime evidence refuse both capabilities. Existing warnings are never cleared.
The conditional seed extent uses the existing interprocedural Allocate normal-
continuation contract; this pass neither proves general pointer validity nor
changes allocation failure or zero-count continuations.

Implementation: `analysis/runtime_stdio_frame.hpp`,
`src/analysis/runtime_stdio_frame.cpp`, numerical transfer and the data-race
checker. `python3 scripts/test_runtime_stdio.py` covers refusal controls, real
neighboring races, renamed objects, raw/mapped scans, changed bounds, zero and
negative counts, repeated consumption and ILP32 long-long payloads.

</details>

<a id="fixed-heap-pthread-initialization-effects"></a>
<details>
<summary>Fixed heap pthread initialization effects — describe checked runtime-storage effects</summary>

A source-only `PthreadInitializationFrame` can seal constant byte effects for
`pthread_mutexattr_init`, recognized NORMAL/ERRORCHECK setters, and heap mutex
initialization before thread creation. The supported GNU i386/glibc route keeps
default private, nonrobust, PRIO_NONE attribute flags; successful valid
initialization is justified by this restricted runtime contract. Results remain
TOP in the numerical analysis. This is not a POSIX-general success assumption.

The frame checks the actual source/AR calls and memory operands, layout and
widths, every direct helper activation, immutable pointer relays, actual
allocation extents, preceding normal Store continuations, and all ordinary
accesses to runtime storage. It refuses repeated/overlapping initialization,
representation aliases, lifetime changes, unsupported CFG/effects and exhausted
budgets. A layout-only NULL-based offsetof expression may contribute an exact
LLVM-folded integer displacement; it cannot become an application address.

Both numerical transfer and the race checker consume the same immutable effect
description. Transfer checks the actual destination root and byte offset,
forgets only the described bytes and publishes interference; the checker keeps
initialization writes and attribute reads. Existing mutex acquisition/release,
NULL handling, MAY escape, thread completion and BMC remain separate. A wrong
ERRORCHECK unlock does not release another mutex. Ordinary data races remain
warnings even when initialization effects are fully covered.

The first subset admits single-block functions, bounded direct helper expansion,
constant main allocations, initialized runtime leaves before all creates and
private unread native create outputs. Unproved cases retain the old fallback.
Implementation: `analysis/pthread_initialization_frame.hpp` and its source;
regressions: `python3 scripts/test_heap_pthread_initialization.py` (ILP32,
including long-long payloads and invalidation controls). LP64 is deferred.

</details>
