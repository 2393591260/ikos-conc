// Copyright and license: see data_race.hpp.
#pragma once
#include <ikos/analyzer/json/json.hpp>
#include <memory>
namespace ikos {
namespace analyzer {
class Context;
struct AccessRecord;
// Finite relational control and serial dynamic child generations.
class ThreadLifecycle {
  class Impl;
  std::unique_ptr< Impl > _impl;

public:
  explicit ThreadLifecycle(Context&);
  ~ThreadLifecycle();
  bool excludes(const AccessRecord&, const AccessRecord&);
  JsonDict info(const AccessRecord&, const AccessRecord&) const;
};
} // namespace analyzer
} // namespace ikos
