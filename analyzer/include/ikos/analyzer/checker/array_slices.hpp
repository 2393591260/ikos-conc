// Copyright and license: see data_race.hpp.
#pragma once
#include <ikos/analyzer/json/json.hpp>
#include <memory>
namespace ikos {
namespace analyzer {
class Context;
struct AccessRecord;
// Inductive byte ranges over immutable, published flat-array bases.
class ArraySlices {
  class Impl;
  std::unique_ptr< Impl > _impl;

public:
  explicit ArraySlices(Context&);
  ~ArraySlices();
  bool excludes(const AccessRecord&, const AccessRecord&);
  JsonDict info() const;
};
} // namespace analyzer
} // namespace ikos
