// Copyright and license: see context.hpp.
// Source/AR and relevant options must stay fixed for this Context.
#pragma once
#include <ikos/analyzer/json/json.hpp>
#include <memory>
#include <string>

namespace ikos {
namespace ar {
class CallBase;
class GlobalVariable;
} // namespace ar
namespace analyzer {
class Context;
class CallContext;

// Constructed before numerical analysis from source/AR/options only. Both
// capabilities are sealed together, then exposed read-only for this Context.
// No concurrent_env, numerical invariant, or checker-owned fact is a premise.
class RuntimeStdioFrame {
  class Impl;
  std::unique_ptr< Impl > _impl;

public:
  explicit RuntimeStdioFrame(Context&);
  ~RuntimeStdioFrame();
  bool omits_stderr_write(ar::CallBase*, CallContext*) const;
  // nullptr is the conservative fallback. This identity still requires the
  // consumer to verify its current destination points-to and zero offset.
  ar::GlobalVariable* non_exposing_scan_output(ar::CallBase*) const;
  JsonDict info(ar::CallBase*) const;
  const std::string& refusal_reason() const;
};

// Call once at ThreadModularAnalysis::run entry, after driver options settle,
// before any fixpoint/preflight. No lazy construction from numerical/checker.
void configure_runtime_stdio_frame(Context&);
} // namespace analyzer
} // namespace ikos
