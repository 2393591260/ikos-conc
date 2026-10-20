# Module contracts and implementation index

[Documentation index](../../CONCURRENCY.md)

Each identifier below locates a maintained contract describing a supported
mechanism. These identifiers preserve the existing public section names; they
are documentation identities, not independently checkable proof certificates.
The detailed sections include premises, unsupported boundaries and regression
commands. The [architecture](../developer-guide/architecture.md) maps their roles
to source files, and [soundness](soundness.md) lists the common open obligations.

## Roles and permitted use

| Role | Implementation examples | Consumer and boundary |
| --- | --- | --- |
| Library and implicit effects | PthreadInitializationFrame, RuntimeStdioFrame, PosixSemaphore, pthread-key models | Transfers and extern access collection; exact byte effects do not grant general synchronization or safety. |
| Stable range premises | AllAccessFacts and the independent range preflight | Specific effect/array consumers; missing or unstable facts cannot certify bounds. |
| Pair exclusions | RegionOwnership, ArraySlices, ThreadPrivateHeap, publication/protocol/lifecycle passes | DataRaceChecker applies each method's full premises to the specified access pair. |
| Coverage replacement | ReadonlyHeapReads and BucketListOwnership | Only supported effect identities and complete replacement contracts; original unresolved effects still block safety. |
| Counterexample search | BMC unroll/encoding/result modules | Remaining candidates; no bounded failure proves unbounded safety. |
| Execution validation | validate_bmc_execution.py and the competition wrapper | Independent checking before official-entry FALSE; source-witness acceptance remains separate. |

Some components serve more than one role. Coverage replacement and pair
exclusion remain distinct permissions even when supplied by the same component.

## Contract locations

| Stable section identifier | Mechanism | Reference |
| --- | --- | --- |
| `heap-pointer-field-initialization` | Heap pointer-field initialization — recover field values from all possible writes | [Contract](library-effects.md#heap-pointer-field-initialization) |
| `native-thread-handle-arrays` | Native thread-handle arrays — model private handle storage without inferring completion | [Contract](library-effects.md#native-thread-handle-arrays) |
| `pthread-keys-and-destructor-effects` | Pthread keys and destructor effects — account for thread-local library effects | [Contract](library-effects.md#pthread-keys-and-destructor-effects) |
| `calloc-size-overflow` | Calloc size overflow — retain allocation-failure continuations | [Contract](library-effects.md#calloc-size-overflow) |
| `zero-element-artificial-initialization` | Zero-element artificial initialization — avoid inserting a seed into an empty array | [Contract](library-effects.md#zero-element-artificial-initialization) |
| `formatted-input-coverage` | Formatted-input coverage — keep warnings for unmodeled parser effects | [Contract](library-effects.md#formatted-input-coverage) |
| `exact-scalar-string-input` | Exact scalar string input — model a restricted sscanf call | [Contract](library-effects.md#exact-scalar-string-input) |
| `closed-runtime-stdio-frame` | Closed runtime stdio frame — check when synthetic stdio effects can be omitted | [Contract](library-effects.md#closed-runtime-stdio-frame) |
| `fixed-heap-pthread-initialization-effects` | Fixed heap pthread initialization effects — describe checked runtime-storage effects | [Contract](library-effects.md#fixed-heap-pthread-initialization-effects) |
| `private-worker-allocations` | Private worker allocations — distinguish objects belonging to separate workers | [Contract](memory-and-ownership.md#private-worker-allocations) |
| `indexed-heap-ownership` | Indexed heap ownership — relate heap owners to their protecting mutexes | [Contract](memory-and-ownership.md#indexed-heap-ownership) |
| `stable-flat-array-slices` | Stable flat-array slices — prove that workers access disjoint byte ranges | [Contract](memory-and-ownership.md#stable-flat-array-slices) |
| `serialized-arena-reservations` | Serialized arena reservations — prove that successful reservations do not overlap | [Contract](memory-and-ownership.md#serialized-arena-reservations) |
| `immutable-heap-array-reads` | Immutable heap-array reads — retain values from completely initialized, stable cells | [Contract](memory-and-ownership.md#immutable-heap-array-reads) |
| `immutable-bucket-membership-with-protected-payloads` | Immutable bucket membership with protected payloads — check graph membership and bucket locks | [Contract](memory-and-ownership.md#immutable-bucket-membership-with-protected-payloads) |
| `self-publication-one-write-repeated-reads` | Self-publication: one write, repeated reads | [Contract](synchronization.md#self-publication-one-write-repeated-reads) |
| `finite-sequentially-consistent-protocols` | Finite sequentially consistent protocols — exhaust the supported protocol state space | [Contract](synchronization.md#finite-sequentially-consistent-protocols) |
| `two-mutex-handover` | Two-mutex handover — prove ordering through a checked lock history | [Contract](synchronization.md#two-mutex-handover) |
| `closed-first-publication-ordering` | Closed first-publication ordering — order initialization before acquired accesses | [Contract](synchronization.md#closed-first-publication-ordering) |
| `sealed-runtime-initialization-gates` | Sealed runtime initialization gates — check one-time initialization before later accesses | [Contract](synchronization.md#sealed-runtime-initialization-gates) |
| `monotone-publication-through-a-mutex-protected-flag` | Monotone publication through a mutex-protected flag — order earlier writes before observed reads | [Contract](synchronization.md#monotone-publication-through-a-mutex-protected-flag) |
| `closed-binary-atomic-tokens` | Closed binary atomic tokens — prove exclusive access through checked atomic transitions | [Contract](synchronization.md#closed-binary-atomic-tokens) |
| `closed-gnu-binary-semaphores` | Closed GNU binary semaphores — prove exclusion under a restricted runtime contract | [Contract](synchronization.md#closed-gnu-binary-semaphores) |
| `monotone-ticket-ownership` | Monotone ticket ownership — track unique reserved and held access rights | [Contract](synchronization.md#monotone-ticket-ownership) |
| `mutex-protected-scalar-protocols` | Mutex-protected scalar protocols — explore protected values and synchronization histories | [Contract](synchronization.md#mutex-protected-scalar-protocols) |
| `serial-dynamic-thread-lifecycles` | Serial dynamic thread lifecycles — check when child activations cannot overlap | [Contract](synchronization.md#serial-dynamic-thread-lifecycles) |
| `bmc-thread-instances-arguments-and-heap-handles` | BMC thread instances, arguments and heap handles | [Contract](bounded-execution.md#bmc-thread-instances-arguments-and-heap-handles) |
| `atomic-targets-and-communication` | Atomic targets and communication — retain the required execution suffixes and ordering | [Contract](bounded-execution.md#atomic-targets-and-communication) |
| `exact-ffs-values-in-bmc` | Exact ffs values in BMC — compute the lowest set bit of the actual argument | [Contract](bounded-execution.md#exact-ffs-values-in-bmc) |
| `startup-heap-pointer-fields-in-bounded-executions` | Startup heap pointer fields in bounded executions — bind loads to actual initialization stores | [Contract](bounded-execution.md#startup-heap-pointer-fields-in-bounded-executions) |
| `partition-inclusion-and-convergence` | Partition inclusion and convergence — check coverage when abstract states merge | [Contract](abstract-state.md#partition-inclusion-and-convergence) |

## Maintaining a contract

Keep the claim, semantic scope, abstract-state meaning, producer and consumers,
transfer/join/loop behavior, invalidation and complete alternatives explicit.
Bind accesses to their actual statement, thread root, calling context, object
coordinate and byte width. Include code and meaningful controls; distinguish a
model assumption, mathematical proof, reviewed implementation and observed test.

Changes to permissions or dependencies require updating the implementation and
its interaction regressions as well as this index. A new module should follow
[extending-analysis.md](../developer-guide/extending-analysis.md); new external
operations should follow [extending-library.md](../developer-guide/extending-library.md).
