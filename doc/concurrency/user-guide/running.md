# Running concurrent analysis

[Concurrent analysis documentation](../../CONCURRENCY.md)

## Install the release archive

The [current release](../../../README.md#install-and-run) targets Linux x86-64
with Ubuntu 26.04 system packages. Windows users can use Ubuntu 26.04 under WSL2.
Install the runtime dependencies before extracting and running the archive:

```bash
sudo apt-get update
sudo apt-get install --yes \
  unzip python3 libc6-dev libc6-dev-i386 lib32gcc-15-dev \
  libgmp10 libgmpxx4ldbl libmpfr6 \
  libsqlite3-0 libtinfo6 zlib1g libffi8 \
  libedit2 libbsd0 liblzma5 libmd0
```

Download `ikos-conc.zip` from the linked Zenodo record, extract it, and keep the
whole directory together. Run `./smoketest.sh` inside it before analyzing inputs.
The smoke test uses the actual competition entry on race and mutex-protected
examples in ILP32 and LP64. It checks witness output, not independent
source-witness validity.

The archive includes LLVM/Clang 14, APRON, Z3, Boost Filesystem/Thread, PPL,
oneTBB and its allocator, and the ICU 74/libxml2 ABIs needed by its LLVM build.
`BUNDLED-LIBRARIES.json` records the added runtime libraries' versions and hashes;
third-party notices and PPL's corresponding source are in `licenses/`.
System Python and the declared system libraries are still required. Check
`BUILD-INFO.json` for the source revision and binary hashes; a repository update
does not change a published archive.

Native macOS is not a validated release target. Its manual exploratory build
workflow has an unresolved compilation failure; Linux is the supported build
and acceptance target.

## Supported analysis engine

Concurrent analysis requires `--proc=inter` and `--jobs=1` in the
underlying `ikos` process. Requests for the race checker or explicit concurrency
with other engines fail with a nonzero exit. A benchmark harness can run several
independent tasks, and its core limit is separate from this analyzer option.

## Analysis modes

Use the witness wrapper for the competition configuration. It enables abstract
analysis, Boolean partitions and bounded race confirmation together.

### Competition entry and source witnesses

In an unpacked archive:

```bash
./bin/svcomp_witness.py --data-model ILP32 --out witness.yml input.c
```

From a source checkout after installation:

```bash
./install/bin/svcomp_witness.py \
  --data-model ILP32 --out witness.yml input.c
```

The wrapper fixes the analysis to demote plus BMC and explicit Boolean
partitions (`--partitioning=guards`), accepts `.c`/`.i` inputs,
and emits a witness in SV-Witnesses format 2.2 for a confirmed race. It defaults
to ILP32; direct `ikos` defaults to LP64, so specify the model explicitly.
Use `--property FILE.prp` for a competition task. The supported property is:

```text
CHECK(init(main()), LTL(G ! data-race))
```

Other properties are rejected. The wrapper locates `ikos` next to itself unless
`--ikos` is specified. It prints a confirmed verdict only after writing the
witness. Output files are per invocation: the requested old witness is removed
before analysis, including unsuccessful runs. Parallel tasks need distinct
witness paths and analyzer databases.

For direct analyzer commands, abstract-analysis-only runs and partition options,
expand [Direct analyzer options](#direct-analyzer-options) below. These instructions
describe the current source checkout; an older archive may behave differently.
Check its `BUILD-INFO.json` before interpreting its results.

<a id="direct-analyzer-options"></a>
<details>
<summary>Direct analyzer options</summary>

All commands below use the installed frontend, not a binary in the build tree.
Select `-m 32` for ILP32 or `-m 64` for LP64 to match the input's target.
The analyzer verdict behavior here describes the current source checkout.
Older archives may label standalone abstract candidates as definite races;
use their confirmation wrapper and check `BUILD-INFO.json` for the version.

### Abstract safety analysis

```bash
env -u IKOS_BMC_RECOVER ./install/bin/ikos \
  --analyses=race --concurrency=auto --demote-race-to-unknown \
  -m 64 -o output.db input.c
```

The abstract interpreter tries to prove safety. Unresolved race candidates and
coverage gaps remain inconclusive. It does not construct a concrete race.

### Abstract analysis with bounded confirmation

```bash
IKOS_BMC_RECOVER=1 ./install/bin/ikos \
  --analyses=race --concurrency=auto --demote-race-to-unknown \
  -m 64 -o output.db input.c
```

This is the recommended analyzer mode. The abstract interpreter first excludes
conflicts where its conservative proofs apply; BMC searches remaining candidates
for actual finite executions. The environment variable is enabled by presence:
`IKOS_BMC_RECOVER=0` still enables it. Remove it to disable confirmation.
The demote option is also required to enter the current confirmation path.
An abstract warning from another mode is not evidence of an actual race.
Without confirmation, unresolved candidates are reported as warnings rather
than definite races. This applies to the ordinary non-demote mode as well.

### Explicit Boolean partitions

Add `--partitioning=guards` to keep Boolean alternatives separate through scalar
copies, integer conversions, bitcasts, and helper parameters. A guard helper's
actual normal-return paths can then retain index bounds lost by interval joins.
The policy selects partition keys; it adds no assumptions and does not recognize
helpers by name. Redefinition and non-injective conversion use the underlying
machine-integer transfers and conservative partition merging.

Ordinary `ikos` still defaults to `--partitioning=no`. The existing `manual` and
`return` strategies retain their previous behavior. In `guards` mode one
partition key is managed automatically; this is not a relational integer domain.
For example, when a helper receives the integer index and computes its condition
internally, bounds on that formal do not necessarily refine the caller's copy.
Unsupported cases remain inconclusive. The initial end-to-end ownership tests
use ILP32, including programs with 64-bit data.

</details>
