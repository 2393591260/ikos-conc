// Copyright and license: see context.hpp.
#pragma once
#include <ikos/core/number/z_number.hpp>
#include <memory>
#include <string>
#include <vector>
namespace ikos {
namespace ar { class CallBase; class GlobalVariable; }
namespace analyzer {
class Context;
class MemoryLocation;
struct PthreadInitializationEffect {
  struct Target {
    ar::CallBase* allocation;
    ar::GlobalVariable* global;
    core::ZNumber offset;
  };
  unsigned write_bytes;
  unsigned read_bytes;
  std::vector< Target > destinations;
  bool matches(MemoryLocation*, const core::ZNumber&) const;
};
// Source-only constant byte closure, sealed before any numerical publication.
class PthreadInitializationFrame {
  class Impl;
  std::unique_ptr< Impl > _impl;
public:
  explicit PthreadInitializationFrame(Context&);
  ~PthreadInitializationFrame();
  const PthreadInitializationEffect* effect(ar::CallBase*) const;
  const std::string& refusal_reason() const;
};
void configure_pthread_initialization_frame(Context&);
} // namespace analyzer
} // namespace ikos
