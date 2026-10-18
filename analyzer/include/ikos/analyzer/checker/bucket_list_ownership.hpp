// Copyright and license: see data_race.hpp.
#pragma once
#include <ikos/analyzer/json/json.hpp>
#include <memory>
#include <string>
#include <vector>
namespace ikos {
namespace ar {
class Statement;
}
namespace analyzer {
class Context;
struct AccessRecord;
class BucketListOwnership {
  class Impl;
  std::unique_ptr< Impl > _impl;

public:
  BucketListOwnership(Context&, const std::vector< AccessRecord >&);
  ~BucketListOwnership();
  bool certifies(ar::Statement*, const std::string& exact_reason);
  bool excludes(const AccessRecord&, const AccessRecord&);
  JsonDict info() const;
};
} // namespace analyzer
} // namespace ikos
