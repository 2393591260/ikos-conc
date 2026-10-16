// Copyright and license: see data_race.hpp.
#pragma once

#include <memory>
#include <vector>
#include <ikos/analyzer/json/json.hpp>

namespace llvm {
class CallBase;
class GlobalVariable;
}

namespace ikos {
namespace analyzer {
class Context;
struct AccessRecord;

struct BinaryTokenEvent {
  llvm::GlobalVariable* source = nullptr;
  bool acquire = false;
};

// Closed named-atomic binary transitions plus per-thread MUST ownership.
// A verified atomic function body does not make its caller atomic.
class BinaryTokenOwnership {
  class Impl;
  std::unique_ptr< Impl > _impl;
public:
  BinaryTokenOwnership(Context&, const std::vector< AccessRecord >&);
  ~BinaryTokenOwnership();
  BinaryTokenEvent event(llvm::CallBase*) const;
  bool excludes(const AccessRecord&, const AccessRecord&) const;
  JsonDict info(const AccessRecord&, const AccessRecord&) const;
};
} // namespace analyzer
} // namespace ikos
