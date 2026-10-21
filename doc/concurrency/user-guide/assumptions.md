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

Many additional safety proofs target **GNU/Linux ILP32** and restricted program
shapes. General ILP32/LP64 input support does not make every proof available on
both ABIs. Each panel below states its own requirements and refusal conditions.

The native GNU/Linux entry checks use the target's actual headers: `int` is
32 bits and `long long` is 64 bits on both models; `long`, pointers, `size_t`
and `pthread_t` are 32/64 bits respectively. The tested GNU mutex layouts are
24/40 bytes. These sizes are target contracts, not portable POSIX constants.
The entry propagates the selected model to compilation and witness metadata.
The dual-ABI baseline covers ordinary global mutex protection, native
create/join and a 64-bit data field after a pointer. More specialized numeric
thread arguments, dynamic handles, local locks and safety certificates retain
their individual support gates; unsupported behavior remains UNKNOWN.

| Mechanism family | What it contributes | Details |
| --- | --- | --- |
| Library effects and initialized fields | Conservatively model supported calls and recover field values; these facts alone do not prove ownership or synchronization. | [Values and library effects](../reference/library-effects.md#values-and-library-effects) |
| Private objects and indexed ownership | Exclude conflicts when both endpoints satisfy the required object, owner and lock relations. | [Memory and ownership](../reference/memory-and-ownership.md#memory-and-ownership) |
| Array slices and arena reservations | Prove that complete access byte ranges are disjoint. | [Memory and ownership](../reference/memory-and-ownership.md#memory-and-ownership) |
| Publication and initialization gates | Order initialization before certified later accesses; arbitrary later writes remain subject to checks. | [Synchronization and lifecycle](../reference/synchronization.md#synchronization-and-lifecycle) |
| Tokens, tickets and scalar protocols | Prove mutual exclusion or ordering under each complete protocol contract. | [Synchronization and lifecycle](../reference/synchronization.md#synchronization-and-lifecycle) |
| Serial child lifecycles | Exclude overlap only when the actual child and completion relation are established. | [Synchronization and lifecycle](../reference/synchronization.md#synchronization-and-lifecycle) |
| BMC instances, values and addresses | Build supported finite race executions with their actual effects and synchronization. | [Bounded executions](../reference/bounded-execution.md#bounded-executions) |
