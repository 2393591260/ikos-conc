// Copyright and license: see data_race.hpp.
#pragma once

#include <memory>
#include <ikos/analyzer/json/json.hpp>

namespace ikos {
namespace analyzer {
class Context;
struct AccessRecord;

// A closed SC scalar protocol, explored to a finite reachable-state fixed point.
class ScProtocol {
  class Impl;
  std::unique_ptr< Impl > _impl;
public:
  explicit ScProtocol(Context&);
  ~ScProtocol();
  bool ordered(const AccessRecord&, const AccessRecord&) const;
  JsonDict info(const AccessRecord&, const AccessRecord&) const;
};
} // namespace analyzer
} // namespace ikos
