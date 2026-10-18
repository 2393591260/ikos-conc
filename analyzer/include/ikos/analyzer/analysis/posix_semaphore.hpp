// Copyright and license: see context.hpp.
#pragma once
#include <map>
#include <memory>
#include <vector>
namespace llvm { class CallBase; class GlobalVariable; }
namespace ikos {
namespace ar { class CallBase; class GlobalVariable; }
namespace analyzer {
class Context;
// Closed GNU/Linux no-handler profile. Library return values stay TOP.
class PosixSemaphore {
  class Impl;
  std::unique_ptr< Impl > _impl;
public:
  struct Token {
    llvm::GlobalVariable* source;
    std::map< llvm::CallBase*, bool > operations;
  };
  explicit PosixSemaphore(Context&);
  ~PosixSemaphore();
  ar::GlobalVariable* effect(ar::CallBase*) const;
  bool has_effects() const;
  bool opaque_handle_call(llvm::CallBase*) const;
  bool allocation_call(llvm::CallBase*) const;
  // Only initialization with one permit can supply an ownership token.
  const std::vector< Token >& tokens() const;
};
void configure_posix_semaphore(Context&);
} // namespace analyzer
} // namespace ikos
