// Copyright and license: see data_race.hpp.
#pragma once
#include <ikos/analyzer/json/json.hpp>
#include <memory>
namespace ikos {
namespace ar {
class Statement;
}
namespace analyzer {
class Context;
struct AccessRecord;
// Closed source proof of exact initialized immutable heap reads.
class ReadonlyHeapReads {
  class Impl;
  std::unique_ptr< Impl > _impl;

public:
  explicit ReadonlyHeapReads(Context&);
  ~ReadonlyHeapReads();
  bool certifies(ar::Statement*);
  bool excludes(const AccessRecord&, const AccessRecord&);
  JsonDict info() const;
};
} // namespace analyzer
} // namespace ikos
