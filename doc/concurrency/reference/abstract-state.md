# Abstract state inclusion and convergence

[Concurrent analysis documentation](../../CONCURRENCY.md)

<a id="partition-inclusion-and-convergence"></a>
<details>
<summary>Partition inclusion and convergence — check coverage when abstract states merge</summary>

Partitioned value domains preserve a union of correlated states. Inclusion and
fixpoint convergence must compare with each actual right-hand partition, including
uncovered key ranges; comparing only convex hulls can incorrectly prove safety.
`core/domain/memory/partitioning.hpp` performs this check without initializing
undefined keys. Its regression suite covers distinct partition keys, gaps, signed
and unsigned one-bit/64-bit boundaries, and finite concrete-state inclusion:

```bash
cmake --build build --target test-core-domain-memory-partitioning
ctest --test-dir build -R '^core-domain-memory-partitioning$' --output-on-failure
```

The existing `--partitioning=manual` and `--partitioning=return` modes are optional;
this inclusion fix does not automatically enable partitioning or add assumptions.

</details>
