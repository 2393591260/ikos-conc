// Copyright and license: see source_call_binding.hpp.
#pragma once
#include <llvm/Analysis/ConstantFolding.h>
#include <llvm/IR/Instructions.h>

namespace ikos {
namespace analyzer {
// A narrow, total fold: unsupported operations or poison never supply a fact.
inline llvm::ConstantInt* closed_integer_fold(
    llvm::Instruction* i, llvm::ArrayRef< llvm::Constant* > operands,
    const llvm::DataLayout& layout) {
  if (!i || operands.size() != i->getNumOperands()) return nullptr;
  for (unsigned n = 0; n < operands.size(); ++n)
    if (!operands[n] || operands[n]->getType() != i->getOperand(n)->getType()) return nullptr;
  for (auto* c : operands) if (!llvm::isa< llvm::ConstantInt >(c)) return nullptr;
  if (auto* binary = llvm::dyn_cast< llvm::BinaryOperator >(i)) {
    if (operands.size() != 2 || (binary->getOpcode() != llvm::Instruction::Add &&
        binary->getOpcode() != llvm::Instruction::Sub)) return nullptr;
    const auto& a = llvm::cast< llvm::ConstantInt >(operands[0])->getValue();
    const auto& b = llvm::cast< llvm::ConstantInt >(operands[1])->getValue();
    bool sign = false, unsign = false;
    if (binary->getOpcode() == llvm::Instruction::Add) {
      (void)a.sadd_ov(b, sign); (void)a.uadd_ov(b, unsign);
    } else { (void)a.ssub_ov(b, sign); (void)a.usub_ov(b, unsign); }
    if ((binary->hasNoSignedWrap() && sign) ||
        (binary->hasNoUnsignedWrap() && unsign)) return nullptr;
  } else if (!llvm::isa< llvm::ICmpInst >(i) && !llvm::isa< llvm::TruncInst >(i) &&
             !llvm::isa< llvm::SExtInst >(i) && !llvm::isa< llvm::ZExtInst >(i)) return nullptr;
  auto* compare = llvm::dyn_cast< llvm::ICmpInst >(i);
  auto* result = compare
      ? llvm::ConstantFoldCompareInstOperands(compare->getPredicate(), operands[0], operands[1], layout)
      : llvm::ConstantFoldInstOperands(i, operands, layout);
  auto* c = llvm::dyn_cast_or_null< llvm::ConstantInt >(result);
  if (!c) return nullptr;
  if (llvm::isa< llvm::TruncInst >(i)) {
    const auto& before = llvm::cast< llvm::ConstantInt >(operands[0])->getValue();
    if (before != c->getValue().sext(before.getBitWidth())) return nullptr;
  }
  return c;
}
} // namespace analyzer
} // namespace ikos
