// Copyright and license: see numerical.hpp.
#pragma once
#include <boost/optional.hpp>

namespace ikos {
namespace ar {
class CallBase;
class Variable;
} // namespace ar
namespace analyzer {

// Pure source/AR descriptor. It proves API and output byte shape only.
// It gives no parsing success, value range, stream isolation or HB fact.
struct ScalarScanEffect {
  ar::CallBase* call;
  ar::Variable* output;
  // False retains the shape match so both raw and mapped fallback routes
  // can explicitly record uncovered implicit state before generic handling.
  bool implicit_state_closed;
  static constexpr unsigned input_argument = 0;
  static constexpr unsigned output_argument = 2;
  static constexpr unsigned output_bytes = 4;
};

boost::optional< ScalarScanEffect > scalar_scan_effect(ar::CallBase* call);

} // namespace analyzer
} // namespace ikos
