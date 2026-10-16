// Copyright and license: see data_race.hpp.
#pragma once

#include <memory>
#include <vector>
#include <ikos/analyzer/json/json.hpp>

namespace ikos {
namespace analyzer {
class Context;
struct AccessRecord;

// Closed named-atomic binary transitions plus per-thread MUST ownership.
// A verified atomic function body does not make its caller atomic.
class BinaryTokenOwnership {
  class Impl;
  std::unique_ptr< Impl > _impl;
public:
  BinaryTokenOwnership(Context&, const std::vector< AccessRecord >&);
  ~BinaryTokenOwnership();
  bool excludes(const AccessRecord&, const AccessRecord&) const;
  JsonDict info(const AccessRecord&, const AccessRecord&) const;
};
} // namespace analyzer
} // namespace ikos
