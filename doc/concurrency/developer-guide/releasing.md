# Building and releasing

[Documentation index](../../CONCURRENCY.md)

A source commit, an installed binary and an uploaded archive have separate
identities. Verifying one does not automatically verify the others.

## Build the candidate

Follow the [installation instructions](../../../README.md#install-and-run).
For evaluation, check the actual CMake cache and compiler flags: a `BUILD_MODE`
label alone does not establish a Release build. Changes to shared headers must
rebuild every dependent object. Restoring patches with old timestamps can leave
incompatible objects in one executable.

Preserve an accepted install and freeze a separate candidate before evaluation.
Do not replace an entry used by an active benchmark run. Record the analyzer,
preprocessor, Python entry/modules and required libraries along with the inputs,
options and validator version.

## Verify the delivery path

Run the relevant regressions and complete paired acceptance for the final
candidate. Check the actual wrapper, result protocol and witness output in a
clean supported container using the declared dependencies. Exercise non-root,
read-only and relocated installations where the archive promises support.

Check the package layout, executable permissions, dependency and license files,
entry-point links and bundled examples. Packaging is implemented in
[svcomp/package.py](../../../svcomp/package.py); its documentation and dependency
declarations must match the built archive.

Witness format checks, independent AR checks and external source-witness
validation must be recorded separately. A successful smoke test exercises a
small sample and does not replace full evaluation.

## Publish an identifiable version

Archive filenames, checksums, source revision and build information must identify
the tested runtime. A repository push does not update an external archive.
Registration metadata must point to the published artifact and use the accepted
competition-year configuration. Verify the public file and actual CI execution
after publication.

Use the competition's current submission rules for deadlines and registration;
the [benchmarking guide](../user-guide/benchmarking.md) describes the distinction
between local checks and official resource evaluation.
