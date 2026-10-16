// Copyright and license: see data_race.hpp.
#pragma once

#include <memory>
#include <vector>

namespace ikos {
namespace analyzer {
class Context;
struct AccessRecord;

// Conditional non-aliasing of live, bounded, unexported worker allocations.
// Does not change points-to, nullity, coverage, or the interference fixpoint.
class ThreadPrivateHeap {
  class Impl;
  std::unique_ptr< Impl > _impl;

public:
  ThreadPrivateHeap(Context&, const std::vector< AccessRecord >&);
  ~ThreadPrivateHeap();
  bool excludes(const AccessRecord&, const AccessRecord&) const;
};
} // namespace analyzer
} // namespace ikos
