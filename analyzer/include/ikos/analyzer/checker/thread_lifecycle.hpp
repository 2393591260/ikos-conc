// Copyright and license: see data_race.hpp.
#pragma once
#include <ikos/analyzer/json/json.hpp>
#include <ikos/analyzer/analysis/value/abstract_domain.hpp>
#include <memory>
namespace llvm {
class Value;
class CallBase;
}
namespace ikos {
namespace analyzer {
class Context;
class CallContext;
struct AccessRecord;
// Read-only final facts for the exact family; they do not identify a numeric
// pthread_t or establish ordering between two child activations.
struct IndexedThreadFamily {
  llvm::Value* source_count = nullptr;
  ar::Value* count = nullptr;
  llvm::CallBase* source_allocation = nullptr;
  ar::CallBase* allocation = nullptr;
  core::ZNumber count_upper{0};
  unsigned handle_stride = 0;
};
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
  bool indexed_family(ar::CallBase* create, ar::Function* child,
                      IndexedThreadFamily& result) const;
  bool completes_before(ar::CallBase* create, ar::Function* child,
                        ar::CallBase* release, CallContext* context) const;
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
