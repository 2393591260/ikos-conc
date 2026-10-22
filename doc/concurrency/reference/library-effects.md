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

<a id="numeric-pointer-null-tests"></a>
### Native numerical pointers used only in null tests

On GNU/Linux x86 ILP32, `numeric_pointer_null.hpp` checks the complete pointer-use
closure in both LLVM and AR. A native-width integer-to-pointer conversion may
only be copied or bitcast and compared for equality/inequality with literal NULL.
Dereferences, publication, calls, returns, pointer arithmetic, other comparisons,
mixed source PHIs and unsupported layouts retain the alias coverage obligation.
Explicit integer truncation before the native conversion keeps its ordinary
machine semantics, including a 64-bit value whose low 32 bits are zero.

The existing abstract conversion still executes: it preserves the numeric offset
and conservative nullity. This contract supplies no object, lifetime, nonnull,
synchronization or pair-exclusion fact. Unrelated effect obligations remain.
BMC null guards demand the integer's actual executed definition, using existing
machine arithmetic and per-occurrence nondet/read-from bindings. Integer copies,
sign casts, extensions and truncations reduce to a low-bit zero predicate;
truncation tests divisibility by the corresponding power of two, including signed
inputs. This preserves nullness without inventing a numeric object address. Failed encoding
remains unsupported; no free pointer symbol replaces an unknown origin. Independent
AR replay computes numeric bits itself and accepts numeric equality only against
literal NULL. It still refuses numeric object access and general pointer equality.

Regression: `scripts/test_numeric_pointer_null.py --ikos install/bin/ikos`.
The source/import contract remains a trusted dependency; finite controls and
AR ACCEPT are not a source-witness validation or a whole-tool proof.

<a id="heap-pointer-field-initialization"></a>
<details>
<summary>Heap pointer-field initialization — recover field values from all possible writes</summary>

The abstract interpreter can restore an initialized heap pointer field in a
worker when a full pointer store precedes every invocation of that worker.
This requires one concrete allocation, an exact byte offset, a direct main
initializer that dominates every closed worker creation site and cannot repeat. The restored value joins
all pointer writes, including later writes; it is not the initializer's value
alone. Ambiguous destinations, overlapping byte writes and uncovered effects
revoke the refinement and trigger reanalysis. This subset checks a closed set
of call effects: general memory-copy, vararg and opaque output operations fall
back to the ordinary conservative analysis. It does not prove general list
ownership or require pointer fields to remain immutable.

A second initialization certificate covers repeated calls to a constructor:
a body allocates one constant-size object, initializes an exact pointer field to
NULL on every continuing path, and returns the base without exposing it earlier.
The interpreter can then constrain field loads using the union of all typed
writes to every possible source, including the initial NULL. Every source must
have the certificate and an available summary. This also works for allocation
sites that summarize multiple objects; it grants no unique object or lock
identity. Integer payload conversions are supported. General branching,
interior returns, pointer export, unknown calls, byte overlap and missing source
coverage fall back conservatively. Lifetime checks remain separate.

Field coverage can also follow a closed initialization prefix in a larger
function: each malloc result reaches full-field initialization before any read or
escape on a certified continuing CFG path. A direct, checked helper may initialize
the object through one pointer parameter and scalar payload parameters.
A main-global store/reload relay additionally requires an exact local source,
no return to the allocation block, and initialization before every possible
create. The complete field value still includes all sources and later writes;
byte overlap, uncovered effects and unsupported lifetime operations disable the
refinement. This certificate only refines field values: it grants neither
ownership nor exclusion of initializer-versus-published-access race pairs.

Both certificates can cross a complementary NULL/non-NULL check of a genuine
malloc result when the NULL edge contains only a genuine libc abort followed by
unreachable. Unconditional edges are also followed; loops and other branches are
refused. The failure edge is retained in the program and cannot contain writes,
reads, calls with other effects, or a continuing return. This proof grants no
pointer validity or non-NULL fact and cannot authorize counterexample confirmation.
The first-publication pass consumes a complete prefix atomically across these
continuations only when it is finite and every continuing block has exactly its
preceding block as predecessor. It clears all skipped result bindings before
installing completed aliases and resumes after the real initialization statement.
Alternate entries and prefix cycles refuse this summary.

The closed bucket/list certificate reuses this guard proof before any worker
creation. It can evaluate the exact pending allocation base against NULL only
when the native comparison matches that proof. The object remains pending until
the existing real zero-base payload Store establishes its normal continuation;
the guard does not grant memory access, graph ownership or FALSE confirmation.
Other nullable comparisons and failure-side effects still refuse the certificate.
This bucket/list subset retains its ILP32 layout boundary.

The private main-to-worker heap handoff certificate accepts this same exact
pure-abort NULL comparison as a nonescaping use. Fresh allocation before each
create attempt, a single transfer, closed uses, access bounds and worker lifetime
remain required. Other pointer comparisons still refuse this ILP32 certificate.

Regression: `scripts/test_allocation_continuations.py --ikos install/bin/ikos`
uses native headers in both ABIs and includes long-long payloads, partial
initialization, early escape/publication, overlapping writes, failure-side effects
and shared fallback objects. The dynamic-list helper control remains UNKNOWN in
LP64 because its separate handle/object coverage is unsupported.

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

BMC retains the actual intrinsic call. For the same closed array structure,
its executed destination must resolve to the current stack allocation at offset
zero. Complete zeroing contributes a zero-valued write source to every element;
later stores and repeated zeroing retain their actual order. A prior invocation
or another worker instance cannot initialize this stack object. No new
synchronization, completion or safety-exclusion permission is introduced.

The independent AR checker checks the exported intrinsic operands and native
layout against a live allocation owned by the current frame/thread. It writes
the actual zero bytes and updates their writer occurrences; subsequent loads
read those bytes and must match encoded values. It does not reuse the producer's
privacy verdict. Missing/mismatched intrinsic, length, byte, volatile, allocation
or layout evidence cannot be accepted. AR acceptance remains narrower than
source-program or source-witness validation.

Reference: [LLVM 14 memset semantics](https://releases.llvm.org/14.0.0/docs/LangRef.html#llvm-memset-intrinsics).
Implementation: `private_array_zero_effect` in
[`concurrent_semantics.hpp`](../../../analyzer/include/ikos/analyzer/analysis/execution_engine/concurrent_semantics.hpp),
consumed by `exec_memset` in
[`numerical.hpp`](../../../analyzer/include/ikos/analyzer/analysis/execution_engine/numerical.hpp).
Structural checks are shared through
[`private_array_zero.hpp`](../../../analyzer/include/ikos/analyzer/analysis/private_array_zero.hpp);
[`unroll.hpp`](../../../analyzer/include/ikos/analyzer/analysis/bmc/unroll.hpp),
[`encode.hpp`](../../../analyzer/include/ikos/analyzer/analysis/bmc/encode.hpp)
and the independent [AR checker](../../../scripts/validate_bmc_execution.py)
implement their distinct execution obligations.
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

<a id="malloc-helper-normalization"></a>
### Bounded malloc helper normalization

The default frontend may expand a directly called, non-interposable leaf helper
with one native-width size argument and a pointer return. The actual body must
be acyclic and bounded (at most 8 blocks and 64 instructions), with one correctly
typed `malloc` declaration, optional terminating `abort`, pointer copies and
comparisons, branches and returns. Returns must derive from that allocation or
literal NULL. User memory effects, unknown calls, escaping function identities,
special atomic boundaries, `optnone` and explicit call-site `noinline` prevent
expansion. The pass performs at most 256 call-site expansions.

Clang's `-O0` function-level `noinline` is also emitted for ordinary helpers.
This narrowly checked normalization uses LLVM's direct inlining utility despite
that function attribute; it retains the original attributes and does not enable
general inlining. An explicitly written function-level `noinline` is subject to
the same body checks. The transformation preserves the allocation-failure CFG;
it supplies no non-nullness, freshness, coverage or SAFE certificate.

A selected FALSE execution may choose a real, positive-size successful malloc in
the initial main frame before thread creation. NULL comparisons must follow that
executed allocation through current copies, without intervening free. The
independent execution checker validates native calling convention, signature,
address spaces, argument width, live object identity and the actual branch.
General pointer comparisons, zero-size or worker allocations remain unsupported.

Regression: `scripts/test_malloc_helpers.py --bits 32` and `--bits 64`.

<a id="allocation-failure-state"></a>
### Allocation failure preserves the caller state

For malloc-like allocation and calloc, the abstract transfer joins a successful
allocation with a failure state that only assigns the result NULL. The failure
state retains the previous memory, lifetimes and MUST facts, including when a
loop revisits the same abstract allocation site. Internal storage initialization
must not act as a source dereference proving allocation success. Ordinary program
dereferences retain their non-null checks.

A restricted first-occurrence proof may keep the successful object's ghost
initialization while restoring a possibly NULL result: no previous object at
that site can be observed on failure. This requires the sole implicit main
entry, no global constructors, a complete direct calling context rooted there,
a static whole-program single-invocation proof, and an allocation block outside
CFG cycles. Nonlocal-return/context APIs, LLVM SJLJ/coroutine intrinsics,
returns-twice attributes, inline assembly and missing native identity refuse it. Unproved or repeated sites retain the full failure-state join.
For calloc this precision rule additionally requires a provably representable
size product, so its success arithmetic cannot narrow failure inputs.

For calloc, the failure state also covers a possibly overflowing size product;
a definitely overflowing product returns NULL without allocating. The join is
performed even when allocation-size tracking is disabled. These rules preserve
failure continuations; they do not prove that a particular runtime allocation
succeeds.

<a id="reallocation-coverage-boundary"></a>
### Reallocation is outside the concurrent storage contract

Thread-modular analysis records an unresolved coverage obligation for `realloc`
and uses a conservative unknown call, preserving both NULL and non-NULL return
possibilities. It does not execute the legacy internal copy/free transfer, whose
success assumptions could remove failure continuations. The obligation blocks
both SAFE and FALSE; it cannot be discharged by a safety-only certificate.
This includes NULL inputs, zero sizes and ignored results. Supporting reallocation
requires separate success/failure, in-place/moved object, copied extent and
lifetime contracts. Other nonconcurrent property checkers retain their existing
reallocation model; they are not certified by these concurrency tests.

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
<summary>Heap and finite global-array pthread initialization — checked runtime-storage effects</summary>

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
private unread native create outputs. A separate global-array route
admits a deterministic main initialization prefix with bounded direct CFG
enumeration. PHIs commit simultaneously on the actual predecessor; integer
arithmetic checks no-wrap flags, and truncations must preserve the interpreted
signed value. Unknown control ends the prefix. Every mutex in a homogeneous
GNU i386 array must be initialized once with an exact legal index before any
create. Suffix reachability must exclude another initialization activation,
including reentry into a previously covered instruction. Attribute pointers
remain whole globals without byte arithmetic. Calls and ordinary representation
uses undergo the complete storage closure; unknown external effects refuse it.

The global-array route seals initialization effects independently from consumer
ranges. Its non-singleton numerical destination offset is only an abstract-state
compatibility check: the source enumeration establishes the complete actual
occurrences, and memory forgetting conservatively covers the whole offset
interval plus the written bytes. No byte inside that interval is thereby
certified as a mutex leaf. Each mutex consumer must have a unique source/AR
definition for an actual typed array GEP and a matching native 32-bit index.
At every final checker context the index must be initialized and in range, with
a non-NULL singleton root. Failure adds an unresolved coverage obligation and
keeps UNKNOWN; it neither changes convergence state nor fills a premise back
into the earlier frame. A range proof does not grant lock success, HB,
completion, pair exclusion or a FALSE result. Existing BMC and independent
AR execution checks still establish the selected mutex behavior.

Unproved cases retain the old fallback.
Implementation: `analysis/pthread_initialization_frame.hpp` and its source;
regressions: `python3 scripts/test_heap_pthread_initialization.py` (ILP32,
including long-long payloads and invalidation controls), and
`python3 scripts/test_pthread_array_initialization.py` (constant/dynamic
consumers, whole-array initialization, wide counters, truncation and wrapping
refusals, prefix reentry and representation aliases). LP64 is deferred.

</details>
