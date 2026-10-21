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
| Coverage replacement | ReadonlyHeapReads and BucketListOwnership | Exact original effect identities and complete safety-only replacement contracts; unresolved obligations block SAFE/FALSE, and safety-only replacements cannot authorize execution confirmation. |
| Counterexample search | BMC unroll/encoding/result modules | Remaining candidates; no bounded failure proves unbounded safety. |
| Execution validation | validate_bmc_execution.py and the competition wrapper | Independent checking before official-entry FALSE; source-witness acceptance remains separate. |

Some components serve more than one role. Coverage replacement and pair
exclusion remain distinct permissions even when supplied by the same component.

## Contract locations

| Stable section identifier | Mechanism | Reference |
| --- | --- | --- |
| `numeric-thread-arguments` | Native numerical thread payloads with closed pointer uses | [Contract](library-effects.md#numeric-thread-arguments) |
| `heap-pointer-field-initialization` | Heap pointer-field initialization — recover field values from all possible writes | [Contract](library-effects.md#heap-pointer-field-initialization) |
| `native-thread-handle-arrays` | Native thread-handle arrays — model private handle storage without inferring completion | [Contract](library-effects.md#native-thread-handle-arrays) |
| `closed-thread-detach` | Closed private thread detachment without a completion edge | [Contract](library-effects.md#closed-thread-detach) |
| `pure-ffs-effects` | Pure GNU ffs effects with an unconstrained numerical return | [Contract](library-effects.md#pure-ffs-effects) |
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

## Production rule adapters

The implementation registry is [race_rules.hpp](../../../analyzer/include/ikos/analyzer/checker/race_rules.hpp). Its identifiers name
permissions, rather than machine-checkable proofs. An adapter must first check
the existing mechanism's premises; merely possessing an identifier grants
nothing.

Pair adapters in [data_race_pairs.cpp](../../../analyzer/src/checker/data_race_pairs.cpp) return a pair-only decision for the two
final access indices. The checker consumes that result in `consume_pair_proof`.
The adapter does not write a final verdict or clear an uncovered effect.
Readonly heap and bucket graph coverage return separate decisions bound to an
actual `(statement, reason)` in the raw effect inventory; these pass through
`consume_coverage_proof`. The inventory retains each obligation and its resolving
contract (`coverage-readonly-heap-v1` or `coverage-bucket-ownership-v1`). If
candidates remain with either safety-only replacement, the strict gate returns
UNKNOWN without BMC. Pair exclusions cannot clear this inventory.
Early library models pass through
`consume_library_effect` while the checker is collecting accesses.

The rule scan preserves the existing short-circuit order. Basic read/read,
thread, points-to, atomic, byte and lock filters are registered alongside the
specialized publication and ownership proofs. The unsafe legacy private-init
rule stays disabled. Numerical pair exclusion keeps its existing explicit
opt-in and query budget.

`IKOS_RACE_RULE_REPORT` optionally writes per-rule exclusion counts and the
remaining candidate count. It does not enable a proof. Reporting failure is a
warning and does not change the verdict. Like other diagnostic output, its
extra I/O can change wall-clock timings.

### Pair rule groups and proof premises

| Rule group | Required premise | Failure behavior |
| --- | --- | --- |
| Read/read and atomic/atomic | Actual recorded access kinds | Otherwise retain the pair. |
| Same unique thread | Proven single represented thread instance | Multiplicity retains self-instance candidates. |
| Inactive/joined/create-before | Complete instances, actual creator and access-point lifetime facts | Missing instances or TOP history retains the pair. |
| Points-to and byte separation | Conservative complete target sets or disjoint full byte intervals | Unknown target, offset or width retains the pair. |
| Concrete or corresponding mutex | Both accesses protected; actual lock equality or overlap-implies-lock-equality proof | Shared read/read locks or unresolved identities retain the pair. |
| Publication, token, ownership, graph and slice passes | The full mechanism-specific contract linked above | An unsupported effect, missing bound or invalidated premise retains the pair. |
| Optional AI offset exclusion | Stable all-offset interval/congruence facts; unbounded overlap query is UNSAT | SAT, invalid facts, timeout or exhausted query budget retains the pair. |

These rules rely on the recorded accesses conservatively covering program
behavior. They do not prove frontend translation or every library transfer;
those remain separate soundness obligations. Registry metadata prevents
permission mixing in the new consumers, but does not prove each original
algorithm correct.

### Fact publication path

The independent preflight's three all-access inventories are consumed through
`Context::range_premise` by the driver. This checks the range role and the
existing stable `AllAccessFacts::proves` predicate. Publication still requires
the full preflight coverage checks; the fresh run's effect obligations are
unchanged. Only the driver writes the private published stores, and every
ordinary consumer uses a const accessor plus its original object/layout checks.

### Early library effect consumers

The ordinary interpreter queries immutable runtime stdio, pthread-initialization
and semaphore certificates through the const `Context` accessors. The producer
configuration functions retain their original complete source scans. Their
numerical consumers retain exact destination/offset/width and memory-havoc
checks; their access-collection consumers use the library-effect gateway.
Pthread TLS slots, immutable arguments and exact constant output retain their
existing limited effect omissions. The legacy free/stack lifetime boundary is
named explicitly and remains a separately documented modeling limitation.

Closed private detachment is checked at the numerical external-call transfer,
using the LLVM/AR contract in `native_thread_handles.hpp`. It summarizes only
application-memory effects and changes no lifecycle or synchronization fact.

These paths do not enter the pair proof gateway. The numerical transfer still
publishes possible writes and invalidates affected memory/value summaries;
proof metadata is not a replacement for interference. Unsupported models fall
back to the original conservative external-effect handling.

## Registry identities and verification locations

The following identities are distinct from the long-standing documentation
section identifiers above. Each locates an adapter permission in
[race_rules.hpp](../../../analyzer/include/ikos/analyzer/checker/race_rules.hpp).
They are conditional implementation contracts, not proof-assistant theorems.
The original detailed contracts and the shared [soundness obligations](soundness.md)
remain applicable. Disabled mechanisms grant no permission; optional offset
rules still require their per-run configuration.

| Registry identity | Implementation / evidence group |
| --- | --- |
| `same-unique-statement-v1` | [Pair adapters](../../../analyzer/src/checker/data_race_pairs.cpp); thread lifecycle, modes and P0 controls. |
| `read-read-v1` | [Pair adapters](../../../analyzer/src/checker/data_race_pairs.cpp); correctness, addresses, arrays and protocol controls. |
| `same-unique-thread-v1` | [Pair adapters](../../../analyzer/src/checker/data_race_pairs.cpp); thread lifecycle, modes and P0 controls. |
| `inactive-child-v1` | [Pair adapters](../../../analyzer/src/checker/data_race_pairs.cpp); thread lifecycle, modes and P0 controls. |
| `joined-right-v1` | [Pair adapters](../../../analyzer/src/checker/data_race_pairs.cpp); thread lifecycle, modes and P0 controls. |
| `joined-left-v1` | [Pair adapters](../../../analyzer/src/checker/data_race_pairs.cpp); thread lifecycle, modes and P0 controls. |
| `create-before-v1` | [Pair adapters](../../../analyzer/src/checker/data_race_pairs.cpp); thread lifecycle, modes and P0 controls. |
| `unreachable-points-v1` | [Pair adapters](../../../analyzer/src/checker/data_race_pairs.cpp); correctness, addresses, arrays and protocol controls. |
| `disjoint-points-v1` | [Pair adapters](../../../analyzer/src/checker/data_race_pairs.cpp); correctness, addresses, arrays and protocol controls. |
| `atomic-atomic-v1` | [Pair adapters](../../../analyzer/src/checker/data_race_pairs.cpp); correctness, addresses, arrays and protocol controls. |
| `init-publication-v1` | [Pair adapters](../../../analyzer/src/checker/data_race_pairs.cpp); the corresponding ownership/publication/token regression group. |
| `disabled-private-init-v1` | [Pair adapters](../../../analyzer/src/checker/data_race_pairs.cpp); disabled; published-before-init regression remains. |
| `disjoint-bytes-v1` | [Pair adapters](../../../analyzer/src/checker/data_race_pairs.cpp); correctness, addresses, arrays and protocol controls. |
| `common-mutex-v1` | [Pair adapters](../../../analyzer/src/checker/data_race_pairs.cpp); correctness, addresses, arrays and protocol controls. |
| `corresponding-mutex-exact-v1` | [Pair adapters](../../../analyzer/src/checker/data_race_pairs.cpp); correctness, addresses, arrays and protocol controls. |
| `corresponding-mutex-congruence-v1` | [Pair adapters](../../../analyzer/src/checker/data_race_pairs.cpp); correctness, addresses, arrays and protocol controls. |
| `offset-congruence-v1` | [Pair adapters](../../../analyzer/src/checker/data_race_pairs.cpp); correctness, addresses, arrays and protocol controls. |
| `offset-smt-v1` | [Pair adapters](../../../analyzer/src/checker/data_race_pairs.cpp); correctness, addresses, arrays and protocol controls. |
| `private-heap-v1` | [Pair adapters](../../../analyzer/src/checker/data_race_pairs.cpp); the corresponding ownership/publication/token regression group. |
| `binary-token-v1` | [Pair adapters](../../../analyzer/src/checker/data_race_pairs.cpp); the corresponding ownership/publication/token regression group. |
| `arena-reservation-v1` | [Pair adapters](../../../analyzer/src/checker/data_race_pairs.cpp); the corresponding ownership/publication/token regression group. |
| `once-initialization-v1` | [Pair adapters](../../../analyzer/src/checker/data_race_pairs.cpp); the corresponding ownership/publication/token regression group. |
| `mutex-handover-v1` | [Pair adapters](../../../analyzer/src/checker/data_race_pairs.cpp); the corresponding ownership/publication/token regression group. |
| `thread-lifecycle-v1` | [Pair adapters](../../../analyzer/src/checker/data_race_pairs.cpp); thread lifecycle, modes and P0 controls. |
| `guarded-publication-v1` | [Pair adapters](../../../analyzer/src/checker/data_race_pairs.cpp); the corresponding ownership/publication/token regression group. |
| `sc-protocol-v1` | [Pair adapters](../../../analyzer/src/checker/data_race_pairs.cpp); the corresponding ownership/publication/token regression group. |
| `self-publication-v1` | [Pair adapters](../../../analyzer/src/checker/data_race_pairs.cpp); the corresponding ownership/publication/token regression group. |
| `first-publication-v1` | [Pair adapters](../../../analyzer/src/checker/data_race_pairs.cpp); the corresponding ownership/publication/token regression group. |
| `region-ownership-v1` | [Pair adapters](../../../analyzer/src/checker/data_race_pairs.cpp); the corresponding ownership/publication/token regression group. |
| `bucket-ownership-v1` | [Pair adapters](../../../analyzer/src/checker/data_race_pairs.cpp); the corresponding ownership/publication/token regression group. |
| `readonly-heap-v1` | [Pair adapters](../../../analyzer/src/checker/data_race_pairs.cpp); the corresponding ownership/publication/token regression group. |
| `array-slices-v1` | [Pair adapters](../../../analyzer/src/checker/data_race_pairs.cpp); the corresponding ownership/publication/token regression group. |
| `coverage-readonly-heap-v1` | [Coverage consumer](../../../analyzer/src/checker/data_race.cpp); readonly-heap and bucket graph positive/negative effect controls. |
| `coverage-bucket-ownership-v1` | [Coverage consumer](../../../analyzer/src/checker/data_race.cpp); readonly-heap and bucket graph positive/negative effect controls. |
| `effect-semaphore-v1` | [Effect consumer](../../../analyzer/src/checker/data_race.cpp); pthread-key, runtime-stdio, initialization, semaphore and correctness controls. |
| `effect-constant-output-v1` | [Effect consumer](../../../analyzer/src/checker/data_race.cpp); pthread-key, runtime-stdio, initialization, semaphore and correctness controls. |
| `effect-scalar-scan-v1` | [Effect consumer](../../../analyzer/src/checker/data_race.cpp); pthread-key, runtime-stdio, initialization, semaphore and correctness controls. |
| `effect-tls-slot-v1` | [Effect consumer](../../../analyzer/src/checker/data_race.cpp); pthread-key, runtime-stdio, initialization, semaphore and correctness controls. |
| `effect-tls-destructor-v1` | [Effect consumer](../../../analyzer/src/checker/data_race.cpp); pthread-key, runtime-stdio, initialization, semaphore and correctness controls. |
| `effect-pthread-initialization-v1` | [Effect consumer](../../../analyzer/src/checker/data_race.cpp); pthread-key, runtime-stdio, initialization, semaphore and correctness controls. |
| `effect-sync-arguments-v1` | [Effect consumer](../../../analyzer/src/checker/data_race.cpp); pthread-key, runtime-stdio, initialization, semaphore and correctness controls. |
| `effect-runtime-stdio-v1` | [Effect consumer](../../../analyzer/src/checker/data_race.cpp); pthread-key, runtime-stdio, initialization, semaphore and correctness controls. |
| `effect-immutable-arguments-v1` | [Effect consumer](../../../analyzer/src/checker/data_race.cpp); pthread-key, runtime-stdio, initialization, semaphore and correctness controls. |
| `effect-lifetime-boundary-v1` | [Effect consumer](../../../analyzer/src/checker/data_race.cpp); pthread-key, runtime-stdio, initialization, semaphore and correctness controls. |
| `effect-initialization-arguments-v1` | [Effect consumer](../../../analyzer/src/checker/data_race.cpp); pthread-key, runtime-stdio, initialization, semaphore and correctness controls. |
| `effect-tls-create-output-v1` | [Effect consumer](../../../analyzer/src/checker/data_race.cpp); pthread-key, runtime-stdio, initialization, semaphore and correctness controls. |
| `range-mutex-effects-v1` | [Preflight driver](../../../analyzer/src/analysis/value/thread_modular.cpp); all-access-facts and preflight-memory-ranges. |
| `range-subarray-v1` | [Preflight driver](../../../analyzer/src/analysis/value/thread_modular.cpp); all-access-facts and preflight-memory-ranges. |
| `range-native-handles-v1` | [Preflight driver](../../../analyzer/src/analysis/value/thread_modular.cpp); all-access-facts and preflight-memory-ranges. |
| `bmc-local-integer-v1` | [Pure-local producer](../../../analyzer/src/checker/data_race.cpp) and [execution consumer](../../../analyzer/src/checker/data_race_search.cpp); values, correctness, auxiliary and independent execution checks. |
| `bmc-local-control-v1` | [Shared-independent control producer](../../../analyzer/src/checker/data_race.cpp) and [execution consumer](../../../analyzer/src/checker/data_race_search.cpp); values, correctness, auxiliary and independent execution checks. |
| `bmc-selected-execution-v1` | [Execution consumer](../../../analyzer/src/checker/data_race_search.cpp); values, correctness, auxiliary and independent execution checks. |

The executable regression groups are registered in
[regression/CMakeLists.txt](../../../analyzer/test/regression/CMakeLists.txt).
A successful regression is finite evidence. The identity table does not assert
that the original source coverage or each mechanism's full theorem is proved.
