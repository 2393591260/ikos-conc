// Copyright and license: see data_race.hpp.
#pragma once

#include <memory>
#include <ikos/analyzer/json/json.hpp>
#include <boost/optional.hpp>
#include <ikos/analyzer/analysis/value/abstract_domain.hpp>

namespace ikos {
namespace analyzer {
struct AccessRecord;
struct ThreadArgumentWindow {
  ar::Function* family;
  core::ZNumber lower;
  core::ZNumber upper;
};

// A final-pair proof, never an entry assumption, effect or interference filter.
class ThreadArgumentWindows {
  class Impl;
  std::unique_ptr< Impl > _impl;
  explicit ThreadArgumentWindows(Context&);
public:
  // Driver-only producer preparation. Unsupported configurations and modules
  // with more than one create site return before allocating helper/ghost state.
  static std::shared_ptr< ThreadArgumentWindows > prepare(Context&);
  ~ThreadArgumentWindows();
  Variable* anchor(ar::Function*) const;
  void observe(ar::Statement*, const value::AbstractDomain&, CallContext*);
  boost::optional< ThreadArgumentWindow > window(
      ar::Value*, const value::AbstractDomain&, const std::string&);
  bool disjoint(const AccessRecord&, const AccessRecord&) const;
  JsonDict proof(const AccessRecord&, const AccessRecord&) const;
  JsonDict statistics() const;
};
} // namespace analyzer
} // namespace ikos
