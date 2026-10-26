// Copyright and license: see data_race.hpp.
#pragma once

#include <cstdint>
#include <string>
#include <ikos/core/domain/concurrent_global_env.hpp>
#include <ikos/core/domain/machine_int/operator.hpp>
#include <ikos/core/number/z_number.hpp>

namespace ikos {
namespace analyzer {

// Apply only to an explicitly copied native query state. Production and
// regression controls share this exact mathematical residual query; no new
// symbolic evaluator or transfer domain is introduced.
template < typename NativeDomain, typename VariableRef >
core::machine_int::Interval native_thread_argument_window(
    NativeDomain& query,
    VariableRef pointer_offset,
    VariableRef anchor,
    VariableRef wide_offset,
    VariableRef wide_anchor,
    VariableRef delta) {
  using Unary = core::machine_int::UnaryOperator;
  using Binary = core::machine_int::BinaryOperator;
  query.int_apply(Unary::Cast, wide_offset, pointer_offset);
  query.int_apply(Unary::Cast, wide_anchor, anchor);
  query.int_apply(Binary::Sub, delta, wide_offset, wide_anchor);
  return query.int_to_interval(delta);
}

// Immutable views of the native observations. Opaque identity keys are compared
// only for equality; no pointer validity or allocation permission is inferred.
struct ThreadArgumentWindowAccessView {
  const void* family;
  const void* object;
  const std::string& thread;
  const core::ZNumber& lower;
  const core::ZNumber& upper;
  std::uint64_t width;
};

// This is the production final-pair theorem, separate from family discovery and
// native numeric queries. References keep pair queries allocation-free.
struct ThreadArgumentWindowConsumerState {
  const void* family;
  const std::string& family_name;
  const core::ZNumber& stride;
  const void* object;
  bool valid;
  bool create_seen;
  bool step_seen;

  bool disjoint(const ThreadArgumentWindowAccessView& a,
                const ThreadArgumentWindowAccessView& b,
                const core::ConcurrentGlobalEnv& shared,
                bool analysis_complete) const {
    if (!family || !valid || !create_seen || !step_seen || !object ||
        !analysis_complete || shared.is_dirty() ||
        !shared.unmodeled_thread_creations().empty() ||
        a.width == 0 || b.width == 0 ||
        a.family != family || b.family != family ||
        a.thread != family_name || b.thread != family_name ||
        a.object != object || b.object != object ||
        a.lower > a.upper || b.lower > b.upper) return false;
    return a.upper + a.width <= stride + b.lower &&
           b.upper + b.width <= stride + a.lower;
  }
};

} // namespace analyzer
} // namespace ikos
