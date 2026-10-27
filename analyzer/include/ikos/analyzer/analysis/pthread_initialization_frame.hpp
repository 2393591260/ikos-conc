// Copyright and license: see context.hpp.
#pragma once
#include <ikos/core/number/z_number.hpp>
#include <memory>
#include <cstdint>
#include <string>
#include <vector>
namespace ikos {
namespace ar { class CallBase; class GlobalVariable; class LocalVariable; class Function; class Value; }
namespace analyzer {
class Context;
class MemoryLocation;
struct PthreadInitializationEffect {
  struct Target {
    ar::CallBase* allocation;
    ar::GlobalVariable* global;
    core::ZNumber offset;
    ar::LocalVariable* local = nullptr;
  };
  unsigned write_bytes;
  unsigned read_bytes;
  std::vector< Target > destinations;
  bool complete_prefix = false;
  bool result_known = false;
  unsigned result = 0;
  bool matches(MemoryLocation*, const core::ZNumber&) const;
  bool matches_range(MemoryLocation*, const core::ZNumber&, const core::ZNumber&) const;
};
struct PthreadMutexArrayAccess {
  ar::GlobalVariable* global;
  ar::Value* index;
  unsigned long long count;
};
// Exact finite-source own-count facts. No child inherits its creator's depth.
struct PthreadRecursiveMutexEffect {
  ar::GlobalVariable* global;
  unsigned before;
  unsigned after;
  unsigned result;
  bool lock;
};
struct PthreadRecursiveMutexRegion {
  ar::GlobalVariable* global;
  ar::Function* root;
  ar::CallBase* lock = nullptr;
  ar::CallBase* unlock = nullptr;
};
// Source-only constant byte closure, sealed before any numerical publication.
class PthreadInitializationFrame {
  class Impl;
  std::unique_ptr< Impl > _impl;
public:
  explicit PthreadInitializationFrame(Context&);
  ~PthreadInitializationFrame();
  const PthreadInitializationEffect* effect(ar::CallBase*) const;
  const PthreadMutexArrayAccess* array_access(ar::CallBase*) const;
  const PthreadRecursiveMutexEffect* recursive_mutex(ar::CallBase*) const;
  const PthreadRecursiveMutexRegion* recursive_region(ar::GlobalVariable*, ar::Function*) const;
  bool recursive_object(ar::GlobalVariable*) const;
  bool recursive_attribute(ar::LocalVariable*) const;
  bool recursive_key(std::uint64_t) const;
  const std::string& refusal_reason() const;
};
void configure_pthread_initialization_frame(Context&);
} // namespace analyzer
} // namespace ikos
