# Build, regression, and benchmark evidence

Run from the repository root unless specified. Commands below describe the current
checkout; verify executable paths, options, installed modules and toolchain when
they change. Do not overwrite unrelated user artifacts.

## Build and synchronize the code under test

```bash
cmake --build build --target ikos-analyzer -j"$(nproc)"
```

Only after a successful build, from the **repository root**:

```bash
cp build/analyzer/ikos-analyzer install/bin/ikos-analyzer
cmp build/analyzer/ikos-analyzer install/bin/ikos-analyzer
```

The alternate build command is
`cd /home/ruan/ikos-conc/build && cmake --build . --target ikos-analyzer -j"$(nproc)"`.
Return to the repository root before the relative `cp`; otherwise it addresses
`build/build/...`. Build/installation failures invalidate subsequent fix claims.

For Python changes, identify the module the wrapper actually imports and synchronize
the changed source files into it. The observed destination is
`install/libexec/lib/python3.12/site-packages/ikos/`, source `analyzer/python/ikos/`.
Generated Python settings/scripts need their generation/install mechanism rather
than a blanket copy. Do not casually replace the local relocatable wrapper with
an upstream CMake install wrapper; verify the installation recipe first.

## Modes and minimal regressions

Diagnostic/default race mode (may report unconfirmed abstract warnings):

```bash
env -u IKOS_BMC_RECOVER install/bin/ikos --analyses=race --concurrency=auto <file.c>
```

Demotion only:

```bash
env -u IKOS_BMC_RECOVER install/bin/ikos --analyses=race --concurrency=auto --demote-race-to-unknown <file.c>
```

BMC confirmation:

```bash
IKOS_BMC_RECOVER=1 install/bin/ikos --analyses=race --concurrency=auto --demote-race-to-unknown <file.c>
```

Placeholders must be replaced with a real input. Supply `-m 32`/`-m 64` from the
task's data model and isolated `-o` output paths when running several tests. Use a
temporary working directory for commands that create `output.db` or `witness.yml`.
Environment **presence**, not its numerical value, currently enables recovery.

For changes to the race filters, threading, memory, values or BMC relevant to these
cases, run both architectures:

```bash
python3 docs/review-repros/bmc-2026-10-08/run.py --bits 32
python3 docs/review-repros/bmc-2026-10-08/run.py --bits 64
```

This runner prints observations and JSON; **it does not assert expected verdicts**.
Exit zero does not establish success. Check each input, mode, verdict and exit code
against the property's semantics, including the control variants:

- `exclusive-flag.c` is race-free: BMC must not confirm FALSE; a guard gate may
  yield UNKNOWN and a stronger justified model may prove SAFE.
- `published-before-init.c` and `overlapping-widths.c` contain races: neither may
  produce SAFE. A confirmed FALSE is useful; UNKNOWN is a precision loss, not FN.
  In particular, demoted heap candidates may remain UNKNOWN.
- Controls and new metamorphic variants need independently understood labels.
  Choose relevant transformations: names, helper calls, thread instances, control
  flow, layout/width, equivalent constants and paths. Not every edit needs every
  transformation. Reject tests that merely mirror implementation structure.

## Sweep commands and honest coverage

Default-mode scans of frozen and rolling sets, respectively:

```bash
env -u IKOS_BMC_RECOVER python3 svcomp_race_test.py
env -u IKOS_BMC_RECOVER python3 svcomp_race_test.py /home/ruan/sv-benchmarks
```

BMC scans need **both** controls:

```bash
IKOS_BMC_RECOVER=1 python3 svcomp_race_test.py --demote
IKOS_BMC_RECOVER=1 python3 svcomp_race_test.py /home/ruan/sv-benchmarks --demote
```

Current defaults are frozen `/home/ruan/sv-benchmarks-2026`, 30-second wall timeout
and 8 jobs. These are local harness settings, not verified 2027 competition limits.
Record any overrides. `--limit` selects a sorted prefix, not a representative sample.

Before calling a run “full”, audit the task manifest against category definitions:
YAML path, active property and expected verdict, input file(s), language and data
model, includes/excludes and dataset revision. Merely having a property in a YAML
does not establish official category inclusion. Conversely, unsupported YAML forms
must not silently vanish from coverage. The current parser is line/regex based,
accepts a single quoted input path, and scanning uses a fixed directory list and
nonrecursive `glob('*.yml')`; examine omissions before changing the task population.

Use parsed YAML/official manifests when validating new task formats; do not infer
false from a malformed/missing verdict. Unknown expected labels are unscored cases,
not negative examples. Expected verdicts belong in evaluation, never semantic rules.

## A/B comparison and experiment record

For behavioral changes, reproduce the old failure, test the fix and relevant
neighbors, then sweep the frozen reference for material changes to verdicts.
Use a small relevant subset during iteration; run the full established manifest
before claiming aggregate improvement. Broaden checks after new failures/changes,
not by repeating an unchanged successful suite. Do not require full sweeps for
skill/documentation-only or other nonbehavioral changes.

Pin both analyzer versions/builds and identical task identities/configuration.
Use isolated checkouts/builds if needed; never reset the user's working tree.
Preserve paired per-task verdict transitions, not just TP totals. Record:

- Analyzer commit plus dirty diff/build fingerprint; executed binary/wrapper and
  installed module identities; compiler/frontend/LLVM/Z3 versions as relevant.
- Dataset revision and actual task manifest, property, machine model, command,
  relevant environment/gates, bounds, timeout/memory/jobs and platform.
- TP/TN/FP/FN, unknown by reason where available, errors/timeouts, and runtime/
  memory if measured; raw logs for changed or failed tasks. State missing metrics.

Current `svcomp_race_test.py` emits `RESULT` lines but does not retain complete
per-task raw output or assert FP/FN zero. Its rows use source paths rather than
YAML/property identities. Retain logs and rerun changed tasks individually; do not
confuse its exit zero with an accepted semantic regression or conflate distinct
tasks sharing inputs. Capture results outside existing user artifacts.

New FP/FN invalidate correctness claims and take priority over score gains. Track
UNKNOWN increases and TP/TN losses as precision regressions; fix, justify or report
them explicitly. Do not claim verified official scoring from an outdated local
formula or report recall that hides UNKNOWN as full negative coverage.

## Competition path and future properties

When changing verdict/witness output or preparing submission, compare direct
analysis with `benchexec/tools/ikos-conc.py` and bundled `svcomp_witness.py` on
affected tasks, then use the actual packaged tool/BenchExec configuration. Run in
isolated working directories. `svcomp/bench-exec-sim.py` reuses the local task scan
and maps some errors/timeouts to UNKNOWN; it is not independent coverage evidence
or equivalent to competition resource enforcement.

A generated witness file alone does not prove a feasible violation or successful
validation. Check its selected property, source mapping, dynamic identities and
data model; use the competition-compatible validator when required and available.
Record unvalidated witnesses as such. Future deadlock/reachability experiments
need their own property manifests, labels and result/witness mapping; do not apply
the current no-data-race adapter or confusion matrix unchanged.
