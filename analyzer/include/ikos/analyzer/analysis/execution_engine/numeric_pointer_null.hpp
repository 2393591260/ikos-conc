// Copyright and license: see numerical.hpp.
#pragma once

#include <set>
#include <vector>

#include <llvm/ADT/Triple.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <ikos/analyzer/analysis/context.hpp>

namespace ikos {
namespace analyzer {

// Closed null tests observe native integer bits without addressing an object.
inline bool numeric_pointer_null(ar::UnaryOperation* cast, Context& ctx) {
  const auto width = ctx.bundle->data_layout().pointers.bit_width;
  auto* integer = dyn_cast< ar::IntegerType >(cast->operand()->type());
  auto* original = cast->has_frontend()
      ? llvm::dyn_cast< llvm::IntToPtrInst >(cast->frontend< llvm::Value >()) : nullptr;
  if ((width != 32 && width != 64) || !integer || integer->bit_width() != width || !original ||
      !original->getOperand(0)->getType()->isIntegerTy(width) ||
      original->getType()->getPointerAddressSpace() != 0 ||
      original->getModule()->getDataLayout().getPointerSizeInBits() != width ||
      !original->getModule()->getModuleInlineAsm().empty() ||
      !cast->code()->is_function_body()) return false;
  const llvm::Triple target(original->getModule()->getTargetTriple());
  if (!((target.getArch() == llvm::Triple::x86 && width == 32) ||
         (target.getArch() == llvm::Triple::x86_64 && width == 64)) ||
      !target.isOSLinux() ||
      target.getEnvironment() != llvm::Triple::GNU) return false;

  unsigned budget = 32768;
  std::set< llvm::Value* > source_seen;
  std::vector< llvm::Value* > source_pending{original};
  while (!source_pending.empty()) {
    auto* value = source_pending.back(); source_pending.pop_back();
    if (!source_seen.insert(value).second) continue;
    for (auto* user : value->users()) {
      if (!budget) return false;
      --budget;
      if (llvm::isa< llvm::DbgInfoIntrinsic >(user)) continue;
      if (auto* copy = llvm::dyn_cast< llvm::BitCastInst >(user)) {
        if (!copy->getType()->isPointerTy() ||
            copy->getType()->getPointerAddressSpace() != 0) return false;
        source_pending.push_back(copy);
      } else if (auto* test = llvm::dyn_cast< llvm::ICmpInst >(user)) {
        if (!test->isEquality() ||
            !llvm::isa< llvm::ConstantPointerNull >(
                test->getOperand(test->getOperand(0) == value ? 1 : 0))) return false;
      } else return false;
    }
  }

  std::set< ar::Value* > seen;
  std::vector< ar::Value* > pending{cast->result()};
  while (!pending.empty()) {
    auto* value = pending.back(); pending.pop_back();
    if (!seen.insert(value).second) continue;
    for (auto* block : *cast->code()) for (auto* statement : *block) {
      if (!budget) return false;
      --budget;
      bool used = false;
      for (auto i = statement->op_begin(); i != statement->op_end(); ++i)
        used |= *i == value;
      if (!used) continue;
      if (auto* copy = dyn_cast< ar::Assignment >(statement)) {
        if (!copy->result()->type()->is_pointer()) return false;
        pending.push_back(copy->result());
      } else if (auto* copy = dyn_cast< ar::UnaryOperation >(statement)) {
        if (copy->op() != ar::UnaryOperation::Bitcast ||
            !copy->result()->type()->is_pointer()) return false;
        pending.push_back(copy->result());
      } else if (auto* test = dyn_cast< ar::Comparison >(statement)) {
        if ((test->predicate() != ar::Comparison::PEQ &&
             test->predicate() != ar::Comparison::PNE) ||
            !isa< ar::NullConstant >(test->left() == value ? test->right() : test->left()))
          return false;
      } else return false;
    }
  }
  return true;
}

} // namespace analyzer
} // namespace ikos
