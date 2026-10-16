// Copyright and license: see data_race.hpp.
#pragma once

#include <ikos/analyzer/json/json.hpp>
#include <memory>

namespace ikos {
namespace analyzer {
class Context;
struct AccessRecord;

// Closed scalar protocols with separate System-SC and protected-mutex states.
class ScProtocol {
  class Impl;
  std::unique_ptr< Impl > _impl;
  mutable std::unique_ptr< Impl > _protected;

public:
  explicit ScProtocol(Context&);
  ~ScProtocol();
  bool ordered(const AccessRecord&, const AccessRecord&) const;
  JsonDict info(const AccessRecord&, const AccessRecord&) const;
};
} // namespace analyzer
} // namespace ikos
