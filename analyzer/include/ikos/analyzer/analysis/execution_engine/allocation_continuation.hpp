// Copyright and license: see numerical.hpp.
#pragma once

#include <ikos/analyzer/analysis/execution_engine/heap_pointer_effects.hpp>

namespace ikos {
namespace analyzer {

// A certificate may cross an allocation check only when the other edge has no
// observable effects before abort. This grants neither nonnull nor FALSE rights.
inline ar::BasicBlock* heap_initialization_successor(
    ar::BasicBlock* block, ar::Comparison*& guard) {
  guard = nullptr;
  std::vector< ar::BasicBlock* > successors(block->successor_begin(),
                                           block->successor_end());
  if (successors.size() == 1) return successors.front();
  if (successors.size() != 2) return nullptr;
  for (unsigned side = 0; side < 2; ++side) {
    auto* failure = successors[side];
    auto* success = successors[1 - side];
    if (failure->empty() || success->empty()) continue;
    auto* equal = dyn_cast< ar::Comparison >(*failure->begin());
    auto* unequal = dyn_cast< ar::Comparison >(*success->begin());
    if (!equal || !unequal || equal->predicate() != ar::Comparison::PEQ ||
        unequal->predicate() != ar::Comparison::PNE) continue;
    auto null_operand = [](ar::Comparison* comparison) -> ar::Value* {
      if (isa< ar::NullConstant >(comparison->right())) return comparison->left();
      if (isa< ar::NullConstant >(comparison->left())) return comparison->right();
      return nullptr;
    };
    auto* value = dyn_cast_or_null< ar::InternalVariable >(null_operand(equal));
    if (!value || null_operand(unequal) != value) continue;
    auto* allocation = dyn_cast_or_null< ar::CallBase >(unique_def(value));
    auto* target = allocation
        ? dyn_cast< ar::FunctionPointerConstant >(allocation->called()) : nullptr;
    auto* native = allocation && allocation->has_frontend()
        ? llvm::dyn_cast< llvm::CallInst >(allocation->frontend< llvm::Value >()) : nullptr;
    auto* callee = native
        ? llvm::dyn_cast< llvm::Function >(native->getCalledOperand()->stripPointerCasts()) : nullptr;
    if (!target || !target->function()->is_declaration() ||
        target->function()->intrinsic_id() != ar::Intrinsic::LibcMalloc ||
        allocation->parent() != block || allocation->num_arguments() != 1 ||
        !callee || !callee->isDeclaration() || callee->getName() != "malloc" ||
        callee->isVarArg() || callee->arg_size() != 1 || native->arg_size() != 1 ||
        !native->getType()->isPointerTy() ||
        llvm::cast< llvm::PointerType >(native->getType())->getAddressSpace() != 0 ||
        !native->getArgOperand(0)->getType()->isIntegerTy(
            allocation->code()->function()->bundle()->data_layout().pointers.bit_width) ||
        !target->function()->has_frontend() ||
        target->function()->frontend< llvm::Value >() != callee ||
        native->getFunctionType() != callee->getFunctionType() ||
        native->getCallingConv() != llvm::CallingConv::C ||
        callee->getCallingConv() != llvm::CallingConv::C) continue;
    auto it = failure->begin();
    ++it;
    if (it == failure->end()) continue;
    auto* abort = dyn_cast< ar::CallBase >(*it++);
    auto* abort_target = abort
        ? dyn_cast< ar::FunctionPointerConstant >(abort->called()) : nullptr;
    auto* abort_native = abort && abort->has_frontend()
        ? llvm::dyn_cast< llvm::CallInst >(abort->frontend< llvm::Value >()) : nullptr;
    auto* abort_callee = abort_native
        ? llvm::dyn_cast< llvm::Function >(abort_native->getCalledOperand()->stripPointerCasts()) : nullptr;
    if (!abort_target || !abort_target->function()->is_declaration() ||
        abort_target->function()->intrinsic_id() != ar::Intrinsic::LibcAbort ||
        abort->num_arguments() != 0 || abort->has_result() ||
        !abort_callee || !abort_callee->isDeclaration() ||
        abort_callee->getName() != "abort" || abort_callee->isVarArg() ||
        abort_callee->arg_size() != 0 || !abort_callee->getReturnType()->isVoidTy() ||
        abort_native->getFunctionType() != abort_callee->getFunctionType() ||
        abort_native->getCallingConv() != llvm::CallingConv::C ||
        abort_callee->getCallingConv() != llvm::CallingConv::C ||
        !abort_target->function()->has_frontend() ||
        abort_target->function()->frontend< llvm::Value >() != abort_callee ||
        it == failure->end() || !isa< ar::Unreachable >(*it++) ||
        it != failure->end()) continue;
    guard = unequal;
    return success;
  }
  return nullptr;
}

} // namespace analyzer
} // namespace ikos
