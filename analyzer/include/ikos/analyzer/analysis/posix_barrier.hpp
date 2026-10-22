// Copyright and license: see context.hpp.
#pragma once
#include <ikos/analyzer/json/json.hpp>
#include <memory>
#include <string>
namespace ikos {
namespace ar { class CallBase; class GlobalVariable; class Statement; }
namespace analyzer {
class Context;
// Fixed, closed GNU one-shot barrier storage and conditional phase ordering.
class PosixBarrier {
  class Impl;
  std::unique_ptr< Impl > _impl;
public:
  explicit PosixBarrier(Context&);
  ~PosixBarrier();
  ar::GlobalVariable* effect(ar::CallBase*) const;
  bool orders(ar::Statement*, const std::string&, ar::Statement*, const std::string&) const;
  JsonDict info() const;
};
void configure_posix_barrier(Context&);
} // namespace analyzer
} // namespace ikos
