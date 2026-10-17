// Copyright and license: see data_race.hpp.
#pragma once
#include <ikos/analyzer/json/json.hpp>
#include <memory>
namespace ikos {
namespace analyzer {
class Context;
struct AccessRecord;
// Main-only writes precede a unique mutex-protected monotone flag publication.
class GuardedPublication {
  class Impl;
  std::unique_ptr< Impl > _impl;
public:
  explicit GuardedPublication(Context&);
  ~GuardedPublication();
  bool ordered(const AccessRecord&, const AccessRecord&) const;
  JsonDict info(const AccessRecord&, const AccessRecord&) const;
};
} // namespace analyzer
} // namespace ikos
