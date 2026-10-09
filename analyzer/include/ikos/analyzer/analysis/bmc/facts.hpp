#pragma once

#include <map>
#include "unroll.hpp"

namespace ikos { namespace analyzer { namespace bmc {

// AI supplies may-bounds for the candidate, not values for unconstrained loads.
// Creation prefixes are concrete structural evidence, retained by the checker.
struct CandidateFacts {
  std::vector< BmcEvent > startup;
  bool bounded_a = false, bounded_b = false;
  core::ZNumber lo_a{0}, hi_a{0}, lo_b{0}, hi_b{0};
  const std::map< ar::BinaryOperation*, bool >* no_wrap = nullptr;
};

} } }
