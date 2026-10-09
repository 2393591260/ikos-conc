# Project navigation and verified snapshot

Paths below are relative to the IKOS-ConC checkout. Locate the checkout rather than
assuming the installed skill's parent is the repository. Local default:
`/home/ruan/ikos-conc`; upstream comparison: `/home/ruan/ikos-original` (verify its
revision before attributing a difference). Do not rewrite upstream to match this fork.

## Source anchors

| Concern | File and search anchors |
| --- | --- |
| Race records and filters | `analyzer/include/ikos/analyzer/checker/data_race.hpp`: `AccessRecord`; `analyzer/src/checker/data_race.cpp`: destructor, `private_init`, `same_global`, `confirm_race` |
| BMC orchestration | `analyzer/include/ikos/analyzer/analysis/bmc/bmc.hpp`: `confirm_race`, `has_unmodeled_sync`, `held_locks` |
| Events and paths | `analyzer/include/ikos/analyzer/analysis/bmc/unroll.hpp`: `BmcEvent`, `classify_event`, `symbolic_byte_offset`, `unique_def_bmc`, `unroll_to` |
| SMT | `analyzer/include/ikos/analyzer/analysis/bmc/encode.hpp`: `check_race`, `encode_value`, `encode_assume`, `unencodable` |
| Call dispatch and threading | `analyzer/include/ikos/analyzer/analysis/execution_engine/numerical.hpp`; sibling `concurrent_semantics.hpp`, `concurrent_inliner.hpp`: `exec_unknown_extern_call`, `exec_pthread_*` |
| Abstract domains | `core/include/ikos/core/domain/lockset/lockset_domain.hpp`; `core/include/ikos/core/domain/concurrent_global_env.hpp` |
| Fixpoint and summaries | `analyzer/src/analysis/value/thread_modular.cpp`; `core/include/ikos/core/fixpoint/concurrent_fwd_fixpoint_iterator.hpp`; concurrent `analysis.cpp`/`function_fixpoint.cpp` under `analyzer/src/analysis/value/` |
| Frontend and layout | `frontend/llvm/`; `ar/`; `bundle->data_layout().store_size_in_bytes(...)` at memory accesses |
| Verdict and competition adapters | `analyzer/python/ikos/analyzer.py`, `report.py`; `svcomp_witness.py`; `benchexec/tools/ikos-conc.py` |
| Evaluation | `svcomp_race_test.py`, `svcomp/bench-exec-sim.py`, `svcomp/package.py` |

Use symbol searches instead of historical line numbers. New property support must
trace options, checker registration, semantics, reporting, adapters, and witnesses;
the race-specific adapter is not automatically a multi-property adapter.

## Checked on 2026-10-08

Source inspected at `51b795c4b09f36170a69be4e5ce0a10345db2de6`. These are dated
implementation observations, not certification of the current or entire analyzer.

- `df63329`: `AccessRecord.width` is separate from starting-offset intervals.
  Loads/stores use layout store size; some synthetic accesses carry width zero.
  The interval filter uses widths. Another structural-offset filter still skips
  unequal offsets; verify that filter's own premises when working on disjointness.
  `kEnablePrivateInit=false` disables the private-init skip, not its record collection.
- `4910339`: value/address gates reject some unsupported operands, variable indices,
  and failed base extraction before BMC confirmation.
- `249e004` historically introduced local nondet encoding. As checked at
  `8a27daf`/`2e29048`, `encode_value` now uses `IkosNonDet` or an exact list of
  declared integer `__VERIFIER_nondet_*` APIs, checks call/return types, and rejects
  synthetic initialization or missing source provenance. `source_nondet` also
  requires `supported_leaf_call`, whose named APIs use an exact list. The earlier
  `name().find("nondet")` warning is historical, not the current implementation.
  Caches and dynamic definition identities still require per-instance review;
  bitcast peeling is bounded. API recognition alone is not a soundness proof.
- `confirm_race` returns `false` for both failure to confirm and unsupported cases;
  it is not a proof of no race. The checker invokes BMC only on selected candidates.
  Removed candidates cannot be recovered by improving BMC.
- The BMC query tests adjacent terminal events in a clock ordering. Do not infer
  that clock adjacency means absence of semantic happens-before.
- Lockset joins contain MUST intersection and MAY union components; inspect each
  component and its top/bottom representation. Collection names alone do not
  establish that a spawn, signal, or wait creates a definite ordering edge.
- BMC recovery requires the demotion option and **presence** of `IKOS_BMC_RECOVER`
  (`getenv` check). Setting it to `0` still enables the gate; unset it for A/B off.

The supplied `TP=52 FP=0 FN=0 TN=709 err=1 unknown=267` is a historical reported
1029-task baseline. This skill creation did not rerun or independently certify it.
Re-establish paired evidence for changes instead of treating these numbers as targets.

Local frozen checkout observed at `0a254fdddcfe24949be9edd47f8d609462eb52fb`.
The runner's nonrecursive directory scan accepted 1029 tasks. Recursive inspection
using the same parser accepted 1030; the extra YAML is
`c/ldv-linux-3.14-races/todo/linux-3.14--drivers--usb--misc--iowarrior.ko.cil.yml`.
This does not establish official inclusion of that `todo` task. Pin an official
category/task manifest before claiming competition-wide coverage.

## Operational traps to check when applicable

- `install/bin/ikos` is a shell/Python wrapper, not the analyzer binary. The local
  wrapper exports bundled library paths and Python 3.12 site-packages, then runs
  `ikos-frontend.py`. Verify these paths if the installation changes.
- Rebuilding C++ does not update `install/bin/ikos-analyzer`; Python changes also
  need installation into the module path the wrapper actually imports.
- `log::debug/info/warning(StringRef)` return void; use the logging API's `msg()`
  stream where appropriate. Keep expensive diagnostic probes gated; remove
  temporary probes after diagnosis unless they provide maintained observability.
- Worklists and identities should use stable semantic keys, not ASLR-dependent
  pointer order. Do not blindly sort away an ordering dependency: fixpoint results
  must remain conservative under schedule changes. Shared-summary growth must
  notify the outer fixpoint and invalidate dependent caches.

## Documents: read by task, then verify

| Need | Starting document |
| --- | --- |
| Navigation | `docs/README.md` |
| API/build traps | `docs/dev-gotchas.md` |
| Architecture/theory claims | `docs/ikos-race-theory-mapping.md`, `docs/concurrency-architecture.md` |
| BMC design/limits | `docs/ikos-bmc-complete.md` |
| Historical counterexamples | `docs/ikos-bmc-review-2026-10-08.md`, `docs/review-repros/bmc-2026-10-08/` |
| Reference analyzer | `docs/dartagnan-svcomp2026-reference.md`, `docs/ikos-vs-dartagnan-gap.md` |
| Competition integration | `svcomp/category-structure-mr.md`, `svcomp/reference/README.md`, relevant `docs/SVCOMP2027-*.md` |

Historical reviews describe pre-fix behavior. Navigation docs still contain older
TP/score claims. A document called “authoritative” is still secondary evidence:
verify source revision, paper premises, and actual scripts for the question at hand.
