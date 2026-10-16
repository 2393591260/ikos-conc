// Copyright and license: see data_race.hpp.
#pragma once

#include <memory>
#include <ikos/analyzer/json/json.hpp>

namespace ikos {
namespace analyzer {
class Context;
struct AccessRecord;

// A unique protected state transition orders later per-instance accesses.
class SelfPublication {
  class Impl;
  std::unique_ptr< Impl > _impl;
public:
  explicit SelfPublication(Context&);
  ~SelfPublication();
  bool ordered(const AccessRecord&, const AccessRecord&) const;
  JsonDict info(const AccessRecord&, const AccessRecord&) const;
};
} // namespace analyzer
} // namespace ikos
