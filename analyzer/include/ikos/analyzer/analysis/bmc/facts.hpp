#pragma once

#include <map>
#include <functional>
#include "unroll.hpp"

namespace ikos { namespace analyzer { namespace bmc {

// startup: actual main CFG/guards/creates; no unordered AI initialization values.
// bounds: the AccessRecord's may-offset hull, never an equality for shared loads.
// no_wrap: all observed contexts agree on the interval proof, with pure local
// SSA sources and shared-independent control (producer in data_race.cpp).
// Missing bounds/proofs only remove these optimizations; source/alias/lifetime
// coverage is still checked from the complete chosen prefixes in encode.hpp.
struct CandidateFacts {
  std::vector< BmcEvent > startup;
  bool bounded_a = false, bounded_b = false;
  core::ZNumber lo_a{0}, hi_a{0}, lo_b{0}, hi_b{0};
  const std::map< ar::BinaryOperation*, bool >* no_wrap = nullptr;
  // Direct, acyclic invocation chains rooted at implicit main; missing proof
  // never licenses an external helper publication.
  std::function< bool(ar::Function*) > single_execution;
};

} } }
