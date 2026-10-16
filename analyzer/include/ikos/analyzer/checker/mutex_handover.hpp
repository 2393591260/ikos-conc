// Copyright and license: see data_race.hpp.
#pragma once

#include <memory>
#include <ikos/analyzer/json/json.hpp>

namespace ikos {
namespace analyzer {
class Context;
struct AccessRecord;

// Continuous lock intervals can order two accesses without a common held lock.
class MutexHandover {
  class Impl;
  std::unique_ptr< Impl > _impl;
public:
  explicit MutexHandover(Context&);
  ~MutexHandover();
  bool ordered(const AccessRecord&, const AccessRecord&) const;
  JsonDict info(const AccessRecord&, const AccessRecord&) const;
};
} // namespace analyzer
} // namespace ikos
