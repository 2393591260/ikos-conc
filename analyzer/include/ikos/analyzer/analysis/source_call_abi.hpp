// Copyright and license: see source_call_binding.hpp.
#pragma once

#include <ikos/analyzer/analysis/source_c_abi.hpp>
#include <ikos/ar/semantic/function.hpp>
#include <ikos/ar/semantic/statement.hpp>
#include <ikos/core/support/cast.hpp>

namespace ikos {
namespace analyzer {

inline const char* source_call_abi_reason() {
  return "source call ABI attributes not covered";
}

// Genuine LLVM intrinsics retain their own contracts (immarg, etc.). An
// AR-mapped libc/pthread intrinsic does NOT receive that exemption. Missing
// source retains its existing obligations; false here grants no support.
inline bool unsupported_source_call_abi(ar::CallBase* imported,
                                        ar::Function* resolved = nullptr) {
  auto* call = imported && imported->has_frontend()
      ? llvm::dyn_cast< llvm::CallBase >(imported->frontend< llvm::Value >())
      : nullptr;
  if (!call) return false;
  auto* direct = llvm::dyn_cast< llvm::Function >(
      call->getCalledOperand()->stripPointerCasts());
  if (direct && direct->isIntrinsic()) return false;
  if (call->getCallingConv() != llvm::CallingConv::C ||
      !ordinary_source_call_site_abi_attributes(call)) return true;
  auto* target = resolved && resolved->has_frontend()
      ? llvm::dyn_cast< llvm::Function >(resolved->frontend< llvm::Value >())
      : nullptr;
  for (auto* function : {direct, target})
    if (function &&
        (function->getCallingConv() != llvm::CallingConv::C ||
         !ordinary_source_call_abi_attributes(call, function))) return true;
  // The external runtime may enter a passed callback with no LLVM CallInst.
  // Check actual direct function values independently; this grants no callback
  // binding, successful runtime call, effect, validity or happens-before fact.
  for (auto& operand : call->args()) {
    auto* callback = llvm::dyn_cast_or_null< llvm::Function >(
        operand->getType()->isPointerTy()
            ? operand->stripPointerCasts() : nullptr);
    if (callback && !callback->isIntrinsic() &&
        (callback->getCallingConv() != llvm::CallingConv::C ||
         !ordinary_source_function_abi_attributes(callback))) return true;
  }
  return false;
}

inline bool unsupported_source_function_abi(ar::Function* imported) {
  auto* function = imported && imported->has_frontend()
      ? llvm::dyn_cast< llvm::Function >(imported->frontend< llvm::Value >())
      : nullptr;
  return function && !function->isIntrinsic() &&
      (function->getCallingConv() != llvm::CallingConv::C ||
       !ordinary_source_function_abi_attributes(function));
}

} // namespace analyzer
} // namespace ikos
