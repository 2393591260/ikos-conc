// Copyright and license: see data_race.hpp.
#pragma once
#include <ikos/analyzer/json/json.hpp>
#include <memory>
namespace ikos {
namespace analyzer {
class Context;
struct AccessRecord;
// GNU/Linux ILP32/LP64 relative byte ranges over immutable published arrays.
// The loop induction remains signed i32; this proves neither validity nor completion.
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
