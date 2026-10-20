# Concurrent data-race analysis

IKOS-ConC uses thread-modular abstract interpretation to establish safety and
bounded model checking to confirm remaining race candidates. Start with the
[installation and first example](../README.md#install-and-run).

## User guide

- [Running and analysis modes](concurrency/user-guide/running.md)
- [Results and witnesses](concurrency/user-guide/results-and-witnesses.md)
- [Semantic assumptions and supported boundaries](concurrency/user-guide/assumptions.md)
- [Budgets and benchmarking](concurrency/user-guide/benchmarking.md)
- [Troubleshooting](../TROUBLESHOOTING.md)

## Developer guide

- [Architecture and source navigation](concurrency/developer-guide/architecture.md)
- [Extending an analysis](concurrency/developer-guide/extending-analysis.md)
- [Extending library semantics](concurrency/developer-guide/extending-library.md)
- [Testing and reproducible evidence](concurrency/developer-guide/testing.md)
- [Building and releasing](concurrency/developer-guide/releasing.md)

## Technical reference

- [Module contracts and code index](concurrency/reference/module-contracts.md)
- [Soundness obligations and proof status](concurrency/reference/soundness.md)
- [Abstract state inclusion and convergence](concurrency/reference/abstract-state.md)
- [Values and library effects](concurrency/reference/library-effects.md)
- [Memory and ownership](concurrency/reference/memory-and-ownership.md)
- [Synchronization and lifecycle](concurrency/reference/synchronization.md)
- [Bounded executions](concurrency/reference/bounded-execution.md)

A module's contract is a sufficient condition under a stated semantic model.
Its presence in this index is not a whole-tool soundness proof. Witness schema
checks, independent AR execution checks and source-level validation have distinct
scopes. Bounded UNSAT does not establish unbounded safety.

## Previous section links

Existing fragment links remain here. Each section has one maintained body in the
linked page.

<a id="direct-analyzer-options"></a>

[direct analyzer options](concurrency/user-guide/running.md#direct-analyzer-options)

<a id="analysis-modes"></a>

[analysis modes](concurrency/user-guide/running.md#analysis-modes)

<a id="competition-entry-and-source-witnesses"></a>

[competition entry and source witnesses](concurrency/user-guide/running.md#competition-entry-and-source-witnesses)

<a id="abstract-safety-analysis"></a>

[abstract safety analysis](concurrency/user-guide/running.md#abstract-safety-analysis)

<a id="abstract-analysis-with-bounded-confirmation"></a>

[abstract analysis with bounded confirmation](concurrency/user-guide/running.md#abstract-analysis-with-bounded-confirmation)

<a id="explicit-boolean-partitions"></a>

[explicit boolean partitions](concurrency/user-guide/running.md#explicit-boolean-partitions)

<a id="results-and-evidence"></a>

[results and evidence](concurrency/user-guide/results-and-witnesses.md#results-and-evidence)

<a id="supported-boundaries"></a>

[supported boundaries](concurrency/user-guide/assumptions.md#supported-boundaries)

<a id="budget-behavior-and-diagnostics"></a>

[budget behavior and diagnostics](concurrency/user-guide/benchmarking.md#budget-behavior-and-diagnostics)

<a id="resource-limits"></a>

[resource limits](concurrency/user-guide/benchmarking.md#resource-limits)

<a id="architecture-and-code-navigation"></a>

[architecture and code navigation](concurrency/developer-guide/architecture.md#architecture-and-code-navigation)

<a id="validation-and-reproducibility"></a>

[validation and reproducibility](concurrency/developer-guide/testing.md#validation-and-reproducibility)

<a id="partition-inclusion-and-convergence"></a>

[partition inclusion and convergence](concurrency/reference/abstract-state.md#partition-inclusion-and-convergence)

<a id="heap-pointer-field-initialization"></a>

[heap pointer field initialization](concurrency/reference/library-effects.md#heap-pointer-field-initialization)

<a id="native-thread-handle-arrays"></a>

[native thread handle arrays](concurrency/reference/library-effects.md#native-thread-handle-arrays)

<a id="pthread-keys-and-destructor-effects"></a>

[pthread keys and destructor effects](concurrency/reference/library-effects.md#pthread-keys-and-destructor-effects)

<a id="calloc-size-overflow"></a>

[calloc size overflow](concurrency/reference/library-effects.md#calloc-size-overflow)

<a id="zero-element-artificial-initialization"></a>

[zero element artificial initialization](concurrency/reference/library-effects.md#zero-element-artificial-initialization)

<a id="formatted-input-coverage"></a>

[formatted input coverage](concurrency/reference/library-effects.md#formatted-input-coverage)

<a id="exact-scalar-string-input"></a>

[exact scalar string input](concurrency/reference/library-effects.md#exact-scalar-string-input)

<a id="closed-runtime-stdio-frame"></a>

[closed runtime stdio frame](concurrency/reference/library-effects.md#closed-runtime-stdio-frame)

<a id="fixed-heap-pthread-initialization-effects"></a>

[fixed heap pthread initialization effects](concurrency/reference/library-effects.md#fixed-heap-pthread-initialization-effects)

<a id="values-and-library-effects"></a>

[values and library effects](concurrency/reference/library-effects.md#values-and-library-effects)

<a id="private-worker-allocations"></a>

[private worker allocations](concurrency/reference/memory-and-ownership.md#private-worker-allocations)

<a id="indexed-heap-ownership"></a>

[indexed heap ownership](concurrency/reference/memory-and-ownership.md#indexed-heap-ownership)

<a id="stable-flat-array-slices"></a>

[stable flat array slices](concurrency/reference/memory-and-ownership.md#stable-flat-array-slices)

<a id="serialized-arena-reservations"></a>

[serialized arena reservations](concurrency/reference/memory-and-ownership.md#serialized-arena-reservations)

<a id="immutable-heap-array-reads"></a>

[immutable heap array reads](concurrency/reference/memory-and-ownership.md#immutable-heap-array-reads)

<a id="immutable-bucket-membership-with-protected-payloads"></a>

[immutable bucket membership with protected payloads](concurrency/reference/memory-and-ownership.md#immutable-bucket-membership-with-protected-payloads)

<a id="memory-and-ownership"></a>

[memory and ownership](concurrency/reference/memory-and-ownership.md#memory-and-ownership)

<a id="self-publication-one-write-repeated-reads"></a>

[self publication one write repeated reads](concurrency/reference/synchronization.md#self-publication-one-write-repeated-reads)

<a id="finite-sequentially-consistent-protocols"></a>

[finite sequentially consistent protocols](concurrency/reference/synchronization.md#finite-sequentially-consistent-protocols)

<a id="two-mutex-handover"></a>

[two mutex handover](concurrency/reference/synchronization.md#two-mutex-handover)

<a id="closed-first-publication-ordering"></a>

[closed first publication ordering](concurrency/reference/synchronization.md#closed-first-publication-ordering)

<a id="sealed-runtime-initialization-gates"></a>

[sealed runtime initialization gates](concurrency/reference/synchronization.md#sealed-runtime-initialization-gates)

<a id="monotone-publication-through-a-mutex-protected-flag"></a>

[monotone publication through a mutex protected flag](concurrency/reference/synchronization.md#monotone-publication-through-a-mutex-protected-flag)

<a id="closed-binary-atomic-tokens"></a>

[closed binary atomic tokens](concurrency/reference/synchronization.md#closed-binary-atomic-tokens)

<a id="closed-gnu-binary-semaphores"></a>

[closed gnu binary semaphores](concurrency/reference/synchronization.md#closed-gnu-binary-semaphores)

<a id="monotone-ticket-ownership"></a>

[monotone ticket ownership](concurrency/reference/synchronization.md#monotone-ticket-ownership)

<a id="mutex-protected-scalar-protocols"></a>

[mutex protected scalar protocols](concurrency/reference/synchronization.md#mutex-protected-scalar-protocols)

<a id="serial-dynamic-thread-lifecycles"></a>

[serial dynamic thread lifecycles](concurrency/reference/synchronization.md#serial-dynamic-thread-lifecycles)

<a id="synchronization-and-lifecycle"></a>

[synchronization and lifecycle](concurrency/reference/synchronization.md#synchronization-and-lifecycle)

<a id="bmc-thread-instances-arguments-and-heap-handles"></a>

[bmc thread instances arguments and heap handles](concurrency/reference/bounded-execution.md#bmc-thread-instances-arguments-and-heap-handles)

<a id="atomic-targets-and-communication"></a>

[atomic targets and communication](concurrency/reference/bounded-execution.md#atomic-targets-and-communication)

<a id="exact-ffs-values-in-bmc"></a>

[exact ffs values in bmc](concurrency/reference/bounded-execution.md#exact-ffs-values-in-bmc)

<a id="startup-heap-pointer-fields-in-bounded-executions"></a>

[startup heap pointer fields in bounded executions](concurrency/reference/bounded-execution.md#startup-heap-pointer-fields-in-bounded-executions)

<a id="bounded-executions"></a>

[bounded executions](concurrency/reference/bounded-execution.md#bounded-executions)
