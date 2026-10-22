// Copyright and license: see data_race.hpp.
#pragma once
#include <memory>
#include <ikos/analyzer/json/json.hpp>

namespace ikos {
namespace analyzer {
class Context;
struct AccessRecord;
// Actual lock intervals order accesses; children inherit no locks.
class LockedCreation {
  class Impl;
  std::unique_ptr< Impl > _impl;
public:
  explicit LockedCreation(Context&);
  ~LockedCreation();
  bool ordered(const AccessRecord&, const AccessRecord&) const;
  JsonDict info(const AccessRecord&, const AccessRecord&) const;
};
} // namespace analyzer
} // namespace ikos
