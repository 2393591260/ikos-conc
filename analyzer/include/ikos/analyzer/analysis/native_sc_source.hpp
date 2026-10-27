#pragma once
// Raw frontend admission facts, shared by the effect and finite BMC consumers.
// Never grants verifier-atomic exclusion or establishes a global SC HB edge.
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Instructions.h>
#include <ikos/ar/semantic/statement.hpp>

namespace ikos { namespace analyzer {
inline llvm::Instruction* native_sc_source(ar::Statement* s) {
  return s && s->has_frontend()
      ? llvm::dyn_cast< llvm::Instruction >(s->frontend< llvm::Value >()) : nullptr;
}
inline const char* native_sc_rmw_name(llvm::AtomicRMWInst::BinOp op) {
  switch (op) {
    case llvm::AtomicRMWInst::Xchg: return "xchg";
    case llvm::AtomicRMWInst::Add: return "add";
    case llvm::AtomicRMWInst::Sub: return "sub";
    case llvm::AtomicRMWInst::And: return "and";
    case llvm::AtomicRMWInst::Or: return "or";
    case llvm::AtomicRMWInst::Xor: return "xor";
    default: return nullptr;
  }
}
struct NativeSCSource {
  llvm::Instruction* instruction = nullptr;
  llvm::Value *pointer = nullptr, *argument = nullptr, *expected = nullptr;
  const char* operation = nullptr;
  unsigned bits = 0, alignment = 0;
  bool weak = false, volatile_access = false;
  explicit operator bool() const {return instruction != nullptr;}
};
inline NativeSCSource native_sc_source_descriptor(ar::Statement* s) {
  NativeSCSource d;
  auto* n = native_sc_source(s);
  llvm::Type* t = nullptr;
  const auto sc = llvm::AtomicOrdering::SequentiallyConsistent;
  if (auto* x = llvm::dyn_cast_or_null< llvm::LoadInst >(n)) {
    if (!x->isAtomic() || x->getOrdering() != sc || x->getSyncScopeID() != llvm::SyncScope::System) return {};
    d.operation = "load"; d.pointer = x->getPointerOperand(); t = x->getType();
    d.alignment = x->getAlign().value(); d.volatile_access = x->isVolatile();
  } else if (auto* x = llvm::dyn_cast_or_null< llvm::StoreInst >(n)) {
    if (!x->isAtomic() || x->getOrdering() != sc || x->getSyncScopeID() != llvm::SyncScope::System) return {};
    d.operation = "store"; d.pointer = x->getPointerOperand(); d.argument = x->getValueOperand(); t = d.argument->getType();
    d.alignment = x->getAlign().value(); d.volatile_access = x->isVolatile();
  } else if (auto* x = llvm::dyn_cast_or_null< llvm::AtomicRMWInst >(n)) {
    if (x->getOrdering() != sc || x->getSyncScopeID() != llvm::SyncScope::System) return {};
    d.operation = native_sc_rmw_name(x->getOperation());
    if (!d.operation) return {};
    d.pointer = x->getPointerOperand(); d.argument = x->getValOperand(); t = x->getType();
    d.alignment = x->getAlign().value(); d.volatile_access = x->isVolatile();
  } else if (auto* x = llvm::dyn_cast_or_null< llvm::AtomicCmpXchgInst >(n)) {
    if (x->getSuccessOrdering() != sc || x->getFailureOrdering() != sc ||
        x->getSyncScopeID() != llvm::SyncScope::System) return {};
    d.operation = "cas"; d.pointer = x->getPointerOperand();
    d.argument = x->getNewValOperand(); d.expected = x->getCompareOperand(); t = d.argument->getType();
    d.alignment = x->getAlign().value(); d.volatile_access = x->isVolatile(); d.weak = x->isWeak();
  } else return {};
  auto* i = llvm::dyn_cast< llvm::IntegerType >(t);
  auto* ptr = llvm::dyn_cast< llvm::PointerType >(d.pointer->getType());
  if (!i || !ptr || ptr->getAddressSpace() != 0 ||
      (i->getBitWidth() != 8 && i->getBitWidth() != 16 && i->getBitWidth() != 32 && i->getBitWidth() != 64) ||
      d.alignment < i->getBitWidth() / 8) return {};
  d.bits = i->getBitWidth(); d.instruction = n;
  return d;
}
inline bool native_sc_operand(ar::Value* a, llvm::Value* n) {
  if (!a || !n) return false;
  if (auto* x = ar::dyn_cast< ar::IntegerConstant >(a)) {
    auto* y = llvm::dyn_cast< llvm::ConstantInt >(n);
    auto* t = ar::dyn_cast< ar::IntegerType >(a->type());
    if (!y || !t || t->bit_width() != y->getBitWidth() || y->getBitWidth() > 64) return false;
    const auto mod = ar::ZNumber(1) << t->bit_width();
    auto v = x->value().to_z_number() % mod; if (v < 0) v += mod;
    return v == ar::ZNumber(y->getZExtValue());
  }
  auto* v = ar::dyn_cast< ar::Variable >(a);
  if (!v || !v->has_frontend() || v->frontend< llvm::Value >() != n) return false;
  if (auto* integer = llvm::dyn_cast< llvm::IntegerType >(n->getType())) {
    auto* type = ar::dyn_cast< ar::IntegerType >(a->type());
    return type && type->bit_width() == integer->getBitWidth();
  }
  return n->getType()->isPointerTy() && a->type()->is_pointer();
}
inline bool native_sc_empty_compiler_barrier(ar::Statement* s) {
  auto* a = ar::dyn_cast_or_null< ar::CallBase >(s);
  auto* c = llvm::dyn_cast_or_null< llvm::CallInst >(native_sc_source(s));
  if (!a || !a->is_asm() || a->has_result() || a->num_arguments() != 0 || !c ||
      !c->isInlineAsm() || !c->getType()->isVoidTy() || !c->arg_empty() ||
      !c->use_empty() || c->hasOperandBundles()) return false;
  auto* asm_ = llvm::dyn_cast< llvm::InlineAsm >(c->getCalledOperand());
  if (!asm_ || !asm_->getAsmString().empty() || asm_->isAlignStack() || asm_->canThrow() ||
      asm_->getDialect() != llvm::InlineAsm::AD_ATT || !asm_->hasSideEffects()) return false;
  // Exact x86 compiler-memory-barrier forms, including Clang's implicit clobbers.
  const auto triple = c->getModule()->getTargetTriple();
  if (triple.find("x86_64") != 0 && triple.find("i386") != 0 &&
      triple.find("i486") != 0 && triple.find("i586") != 0 && triple.find("i686") != 0) return false;
  const auto constraints = asm_->getConstraintString();
  return constraints == "~{memory}" || constraints == "~{memory},~{dirflag},~{fpsr},~{flags}";
}
}} // namespace ikos::analyzer
