// Copyright and license: see execution_engine/numerical.hpp.
#pragma once

#include <memory>

#include <ikos/analyzer/analysis/value/abstract_domain.hpp>
#include <ikos/analyzer/json/json.hpp>

namespace ikos {
namespace analyzer {

class Context;
class CallContext;

// Conditional ownership of live heap byte accesses. Consumers must separately
// exclude absolute/TOP/non-heap points-to disjuncts. This does not prove
// pointer validity or narrow the value analysis and its interference summaries.
class RegionOwnership {
private:
  class Impl;
  std::unique_ptr< Impl > _impl;

public:
  explicit RegionOwnership(Context&);
  ~RegionOwnership();
  void observe(ar::Statement*, const value::AbstractDomain&, CallContext*);
  void run();
  bool excludes(ar::Statement*,
                CallContext*,
                ar::Statement*,
                CallContext*) const;
  JsonDict info() const;
};

} // namespace analyzer
} // namespace ikos
