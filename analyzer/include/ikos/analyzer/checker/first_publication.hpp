// Copyright and license: see data_race.hpp.
#pragma once

#include <ikos/analyzer/json/json.hpp>
#include <memory>
#include <vector>

namespace ikos {
namespace analyzer {
class Context;
struct AccessRecord;

// Orders only a qualified object's birth writes. Initialized pointers are not
// private, uniquely allocated, or exempt from ordinary data-race checks.
class FirstPublication {
  class Impl;
  std::unique_ptr< Impl > _impl;

public:
  FirstPublication(Context&, const std::vector< AccessRecord >&);
  ~FirstPublication();
  bool ordered(const AccessRecord&, const AccessRecord&) const;
  JsonDict info() const;
};
} // namespace analyzer
} // namespace ikos
