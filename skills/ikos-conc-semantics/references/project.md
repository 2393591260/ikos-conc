# Project navigation and versioned evidence

Paths are relative to the IKOS-ConC checkout; do not assume the installed skill's
parent is the repository. Local checkout: `/home/ruan/ikos-conc`; upstream:
`/home/ruan/ikos-original`. Verify revisions before attributing differences.

## Read only the document needed for the task

| Need | Maintained entry |
| --- | --- |
| Current baseline, task population and reading order | `docs/README.md#current` |
| End-to-end flow and owning layers | `docs/concurrency-architecture.md` |
| Abstract coverage, interference, SAFE rules, thread/lock facts | `docs/ikos-ai-complete.md` |
| BMC values, addresses, SC sources, execution evidence and limits | `docs/ikos-bmc-complete.md` |
| AI/BMC feedback, research premises and zero-gain prototypes | `docs/ikos-ai-complete.md#feedback` |
| Build/install, probes, regression and incremental commits | `docs/dev-gotchas.md` |
| Competition entry, packaging, runner and witness acceptance | `docs/SVCOMP2027-参赛指南.md` |

Current numbers have one owner (README), not a second baseline in this skill.
Historical reports were consolidated; retrieve old prose from Git at its recorded
revision. Keep small JSON/CSV evidence and repro inputs at their original paths.
Date external status and distinguish implementation, proposal and experiment.
A document or supplied model summary is secondary evidence, not certification.

## Source anchors

| Concern | File and symbols |
| --- | --- |
| Race records and SAFE filters | `analyzer/include/ikos/analyzer/checker/data_race.hpp`: `AccessRecord`; `analyzer/src/checker/data_race.cpp`: destructor, `private_init`, `same_global`, `confirm_race` |
| BMC orchestration and budget | `analyzer/include/ikos/analyzer/analysis/bmc/bmc.hpp`: `confirm_race`; sibling `result.hpp` |
| Events, paths and dynamic definitions | sibling `unroll.hpp`: `BmcEvent`, `classify_event`, `symbolic_byte_offset`, `unique_def_bmc`, `unroll_to`, `unroll_completed` |
| SMT and value sources | sibling `encode.hpp`: `check_race`, `encode_value`, `encode_assume`, `prefix_values_defined`, `unencodable` |
| AI facts, initialization and export | sibling `facts.hpp`, `initial.hpp`, `replay.hpp`, `evidence.hpp`, `function_values.hpp` |
| Independent AR execution check | `scripts/validate_bmc_execution.py`, `scripts/test_bmc_execution.py`, `scripts/sweep_bmc_execution.py` |
| Dispatch and concurrent semantics | `analyzer/include/ikos/analyzer/analysis/execution_engine/numerical.hpp`, `concurrent_semantics.hpp`, `concurrent_inliner.hpp` |
| Abstract domains and summaries | `core/include/ikos/core/domain/lockset/lockset_domain.hpp`, `core/include/ikos/core/domain/concurrent_global_env.hpp` |
| Inter-thread fixpoint | `analyzer/src/analysis/value/thread_modular.cpp`; `core/include/ikos/core/fixpoint/concurrent_fwd_fixpoint_iterator.hpp`; concurrent `analysis.cpp`/`function_fixpoint.cpp` in analyzer value analysis |
| Frontend and byte layout | `frontend/llvm/`, `ar/`; `bundle->data_layout().store_size_in_bytes(...)` |
| Results and adapters | `analyzer/python/ikos/analyzer.py`, `report.py`; `svcomp_witness.py`; `benchexec/tools/ikos-conc.py` |
| Evaluation and release | `svcomp_race_test.py`, `svcomp/bench-exec-sim.py`, `svcomp/package.py` |

Use symbol searches, not historical line numbers. New properties must trace options,
checker registration, semantics, reporting, adapters and witnesses; the race adapter
is not automatically multi-property.

## Contracts that easily get lost in handoffs

- `confirm_race`/`check_race` return structured `Result`. CONFIRMED carries the
  selected execution; bounded UNSAT, unsupported, solver unknown, budget exhaustion
  and unsearched cases cannot establish SAFE. BMC cannot recover deleted candidates.
- Recovery requires demotion and the **presence** of `IKOS_BMC_RECOVER`; `0` still
  enables it. Unset it for AI-only comparisons. Runner demotion is `--demote`.
- `AccessRecord.width` is separate from starting offsets. Unknown widths, structural
  offsets, dynamic object identity and aliasing each require their own premises.
  `private_init` exclusion remains disabled; collection is not proof of privacy.
- Legal nondet needs a verified intrinsic or exact API/type/source contract. The old
  name-substring heuristic is historical. Loads need actual read-from/value sources.
  Dynamic thread/call/loop definitions cannot share static-variable symbols.
- Clock adjacency does not mean absence of semantic happens-before. Do not transplant
  a query while omitting the reference memory model or synchronization premises.
- AR ACCEPT is conditional on frontend/snapshot/API contracts, not final C-source or
  witness2.2 validation; label TP is a separate observation. Negative calibration
  must actually constrain identities, values, addresses and ordering.
- The local frozen scan uses 1029 ILP32 tasks at benchmark revision
  `0a254fdddcfe24949be9edd47f8d609462eb52fb`. An earlier recursive scan found one extra
  `todo` YAML; neither scan proves official 2027 category membership.
- Protected release endpoints, stable pointer-source correlation and true BMC→AI
  inductive refinement remain proof/design work. Default-off feedback prototypes
  added no frozen TN; finite candidate UNSAT is not an unbounded safety certificate.

Operational traps, build/install paths and stage commit requirements have one owner:
`docs/dev-gotchas.md`. Fixed references and actual paper-read scope have one owner:
AI/BMC manuals. Re-establish paired evidence instead of treating past scores as goals.
