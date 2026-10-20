# Extending library semantics

[Documentation index](../../CONCURRENCY.md)

A library model interprets an operation whose body is unavailable or handled
specially. Its purpose is to include the operation's actual effects in the
analysis. Precision comes from a justified specification of those effects.

## Identify the actual operation

Check the resolved declaration or definition, parameter types, return type,
calling convention and target layout. A similarly named user function is not
automatically a pthread or libc operation. Existing components reuse
[source_call_binding.hpp](../../../analyzer/include/ikos/analyzer/analysis/source_call_binding.hpp)
for some source/AR identity checks; its use does not establish the rest of a model.

## Describe all relevant effects

Record argument evaluation, memory read/write byte ranges, reachable objects,
escape, return values, success/failure behavior and synchronization. A call can
affect future pointer values or branch conditions even if it does not itself
touch the eventual race location. Such dependencies must be covered before an
effect can be omitted.

For example, a modeled input routine writes through an output pointer. The
transfer must update or conservatively forget the affected value, and the race
checker must account for the write. Treating it only as a return-value computation
would miss a possible conflict.

For a mutex operation, matching the mutex address is insufficient. A MUST-held
fact also depends on the supported acquisition, type, lifetime and failure
contract. Unknown release targets must not leave a possibly released mutex in
the guaranteed-held set.

## Keep fallback conservative

When the model cannot establish its premises, use the existing conservative
effect handling or an explicit coverage gap. Do not silently substitute a no-op,
and do not infer success from a function name. A source-only proof that covers a
specific effect does not extend BMC's supported representation of that call.

The main implementation locations are
[concurrent_semantics.hpp](../../../analyzer/include/ikos/analyzer/analysis/execution_engine/concurrent_semantics.hpp),
the [execution engine](../../../analyzer/include/ikos/analyzer/analysis/execution_engine/numerical.hpp),
and [extern access collection](../../../analyzer/src/checker/data_race.cpp).
Specialized contracts are described in
[library-effects.md](../reference/library-effects.md).

## Verify both transfer and access coverage

Use a supported positive example, a real conflicting-access control, a failed or
unsupported call, wrong prototype or user-defined lookalike, aliases, byte-range
overlap and relevant width conversions. Check that changing a shared value
invalidates consumers that previously relied on it. If BMC supports the new
operation, update and test its independent execution interpretation separately.

List the normative library source and the supported platform. Keep language
rules, platform assumptions, code checks and finite test observations distinct.
