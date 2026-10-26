# Semantic assumptions and supported boundaries

[Concurrent analysis documentation](../../CONCURRENCY.md)

## Supported boundaries

The current target is concurrent C data-race analysis. Support depends on the
implemented operation and library contracts; it is not a blanket guarantee for
all C/POSIX programs.

| Area | Current scope and boundary |
| --- | --- |
| Input and layout | C via LLVM/Clang 14 and AR; explicit ILP32/LP64; competition wrapper accepts `.c`/`.i` |
| Memory model | BMC models its supported sequentially consistent executions; complete C11 weak-memory behavior is not implemented |
| Threads | Modeled pthread creation/join and finite dynamic instances; arbitrary creator trees and dynamic handle structures remain limited |
| Synchronization | Supported mutex/atomic/other library contracts; pthread attributes, failures, and ownership must satisfy their modeled premises |
| Objects | Global and supported stack/heap/pointer sources, byte ranges and access widths; general heap graphs, escape and lifetime remain limited |
| Unknown effects | Registered unresolved coverage obligations block SAFE and FALSE; normal inconclusive analysis is UNKNOWN, while import/execution failures remain errors |
| Other properties | Original IKOS analyses remain available; deadlock/reachability are not claimed as complete concurrency properties |

The maintained mechanism contracts cover restricted GNU/Linux ILP32 and LP64
subsets; the [per-mechanism ABI table](../reference/module-contracts.md#per-mechanism-abi-boundaries)
records their actual scope. Native layout support does not establish every
program shape or API behavior. These are source-checkout contracts, not an
upgrade to the capabilities recorded in an older release archive.

The native GNU/Linux entry checks use the target's actual headers: `int` is
32 bits and `long long` is 64 bits on both models; `long`, pointers, `size_t`
and `pthread_t` are 32/64 bits respectively. The tested GNU mutex layouts are
24/40 bytes. These sizes are target contracts, not portable POSIX constants.
The entry propagates the selected model to compilation and witness metadata.
Native numerical thread arguments, supported dynamic handles, local locks and
safety certificates each retain their own source/AR, byte, lifetime and effect
gates. A 64-bit datum on ILP32 is a separate test from a native LP64 program.
An unresolved registered obligation or unsupported confirmation remains UNKNOWN.

## Native dual-ABI mechanism controls

The maintained [native-mechanism regression](../../../scripts/test_native_mechanisms.py)
expects the following results for
[native-mechanisms.c](../../../analyzer/test/regression/concurrency/native-mechanisms.c),
using real GNU/Linux headers and guarded partitioning. These controls describe
specific programs, not general completeness of an API. UNKNOWN retains unresolved
coverage or proof/search obligations; it does not establish source safety.

| Control | ILP32 | LP64 |
| --- | --- | --- |
| Copy a native thread handle, then join before the next write | SAFE | SAFE |
| Detach with overlapping ordinary accesses | FALSE | FALSE |
| Heap payload after a pointer, including 64-bit data | FALSE | FALSE |
| Overlapping whole/upper-word union accesses | FALSE | FALSE |
| Numeric argument `0x100000001`, worker writes only for value 1 | FALSE | UNKNOWN (source is safe) |
| Numeric argument `-1`, tested against `UINTPTR_MAX` | FALSE | UNKNOWN |
| Same ordinary mutex held across creation and both writes | SAFE | SAFE |
| Same once control, initialized before creation | SAFE | SAFE |
| Distinct once controls writing one shared object | UNKNOWN | UNKNOWN |
| Barrier ordering; writes after a shared barrier | UNKNOWN for both | UNKNOWN for both |
| Recursive mutex initialized via attributes | UNKNOWN | UNKNOWN |

The numeric controls exercise GNU target integer/pointer conversions without
using the numeric value as a memory address. Native numerical-payload and detach
contracts support both ABIs, but that does not guarantee confirmation for every
conversion/control shape in this panel. Once, barrier and attributed-mutex
examples also expose semantic limits; changing pointer width alone does not
implement those operations. Do not reinterpret an ILP32-preprocessed input
as LP64: use the original C and target headers when comparing models.
Each confirmed execution is checked independently at AR level. This does not
validate its source witness or all possible executions.

| Mechanism family | What it contributes | Details |
| --- | --- | --- |
| Library effects and initialized fields | Conservatively model supported calls and recover field values; these facts alone do not prove ownership or synchronization. | [Values and library effects](../reference/library-effects.md#values-and-library-effects) |
| Private objects and indexed ownership | Exclude conflicts when both endpoints satisfy the required object, owner and lock relations. | [Memory and ownership](../reference/memory-and-ownership.md#memory-and-ownership) |
| Array slices and arena reservations | Prove that complete access byte ranges are disjoint. | [Memory and ownership](../reference/memory-and-ownership.md#memory-and-ownership) |
| Publication and initialization gates | Order initialization before certified later accesses; arbitrary later writes remain subject to checks. | [Synchronization and lifecycle](../reference/synchronization.md#synchronization-and-lifecycle) |
| Tokens, tickets and scalar protocols | Prove mutual exclusion or ordering under each complete protocol contract. | [Synchronization and lifecycle](../reference/synchronization.md#synchronization-and-lifecycle) |
| Serial child lifecycles | Exclude overlap only when the actual child and completion relation are established. | [Synchronization and lifecycle](../reference/synchronization.md#synchronization-and-lifecycle) |
| BMC instances, values and addresses | Build supported finite race executions with their actual effects and synchronization. | [Bounded executions](../reference/bounded-execution.md#bounded-executions) |
