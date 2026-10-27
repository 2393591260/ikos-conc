// Copyright and license: see data_race.hpp.
#pragma once
#include <ikos/analyzer/json/json.hpp>
#include <ikos/analyzer/analysis/value/abstract_domain.hpp>
#include <memory>
namespace ikos {
namespace analyzer {
class Context;
class CallContext;
struct AccessRecord;
// Same-owner static schema plus sticky final-checker observations. It supplies
// no early proof and never mutates the analysis or its environment.
class ThreadLifecycleObservations {
  class Impl;
  std::unique_ptr< Impl > _impl;
  friend class ThreadLifecycle;
  bool excludes(const AccessRecord&, const AccessRecord&) const;
  JsonDict info(const AccessRecord&, const AccessRecord&) const;
public:
  explicit ThreadLifecycleObservations(Context&);
  ~ThreadLifecycleObservations();
  void observe(ar::Statement*, const value::AbstractDomain&, CallContext*);
};
// Finite relational control, serial generations, and complete indexed joins.
class ThreadLifecycle {
  class Impl;
  std::unique_ptr< Impl > _impl;
  const ThreadLifecycleObservations* _observations;

public:
  explicit ThreadLifecycle(Context&, const ThreadLifecycleObservations* = nullptr);
  ~ThreadLifecycle();
  bool excludes(const AccessRecord&, const AccessRecord&);
  JsonDict info(const AccessRecord&, const AccessRecord&) const;
};
} // namespace analyzer
} // namespace ikos
