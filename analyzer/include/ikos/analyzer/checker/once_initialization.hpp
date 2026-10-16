// Copyright and license: see data_race.hpp.
#pragma once

#include <memory>
#include <vector>
#include <ikos/analyzer/json/json.hpp>

namespace ikos {
namespace analyzer {
class Context;
class BinaryTokenOwnership;
struct AccessRecord;

// Orders a runtime initialization against accesses that MUST pass a sealed gate.
// It never orders two ordinary post-gate accesses.
class OnceInitialization {
  class Impl;
  std::unique_ptr< Impl > _impl;
public:
  OnceInitialization(Context&, const std::vector< AccessRecord >&, const BinaryTokenOwnership&);
  ~OnceInitialization();
  bool ordered(const AccessRecord&, const AccessRecord&) const;
  JsonDict info(const AccessRecord&, const AccessRecord&) const;
};
} // namespace analyzer
} // namespace ikos
