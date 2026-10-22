// Copyright and license: see execution_engine/numerical.hpp.
#pragma once

#include <ikos/analyzer/analysis/execution_engine/native_thread_handles.hpp>

namespace ikos {
namespace analyzer {

// Local storage can be allocated again without using its address as an operand.
inline bool unique_allocation_definition(ar::Allocate* allocation) {
  if (!allocation || !allocation->code() ||
      allocation->result()->function()->body() != allocation->code()) return false;
  unsigned budget = 8192, definitions = 0;
  for (auto* block : *allocation->code()) for (auto* s : *block) {
    if (!budget--) return false;
    if (s->result_or_null() == allocation->result()) {
      if (s != allocation) return false;
      ++definitions;
    }
  }
  return definitions == 1;
}

// Dynamic source identity, not equality of two independently computed ranges.
inline bool source_integer_binding(ar::Value* a, llvm::Value* n) {
  if (!a || !n) return false;
  for (unsigned depth = 0; depth < 32; ++depth) {
    auto* t = dyn_cast< ar::IntegerType >(a->type());
    if (!t || !n->getType()->isIntegerTy() ||
        t->bit_width() != n->getType()->getIntegerBitWidth()) return false;
    if (auto* c = dyn_cast< ar::IntegerConstant >(a)) {
      auto* nc = llvm::dyn_cast< llvm::ConstantInt >(n);
      if (!nc || t->bit_width() > 64) return false;
      auto bits = c->value().to_z_number();
      if (bits < 0) bits += core::ZNumber(1) << t->bit_width();
      return bits == core::ZNumber(nc->getZExtValue());
    }
    auto* v = dyn_cast< ar::InternalVariable >(a);
    if (!v) return false;
    if (v->has_frontend() && v->frontend< llvm::Value >() == n) return true;
    auto* d = unique_def(v);
    if (auto* copy = dyn_cast_or_null< ar::Assignment >(d)) a = copy->operand();
    else if (auto* cast = dyn_cast_or_null< ar::UnaryOperation >(d)) {
      if (cast->op() != ar::UnaryOperation::Bitcast) return false;
      a = cast->operand();
    } else return false;
  }
  return false;
}

// Bind a source call to its inventoried AR statement; this grants no library effect.
inline ar::CallBase* source_call_binding(llvm::CallBase* c,
                                       ar::Statement* original) {
  auto* a = dyn_cast_or_null< ar::CallBase >(original);
  auto* f = llvm::dyn_cast< llvm::Function >(
      c->getCalledOperand()->stripPointerCasts());
  auto* t = a ? native_handle_callee(a) : nullptr;
  if (!a || !isa< ar::Call >(a) || !f || !t ||
      !llvm::isa< llvm::CallInst >(c) || c->isInlineAsm() ||
      c->hasOperandBundles() || c->getCallingConv() != llvm::CallingConv::C ||
      f->getCallingConv() != llvm::CallingConv::C ||
      a->num_arguments() != c->arg_size() ||
      t->num_parameters() != a->num_arguments() ||
      (t->has_frontend() && t->frontend< llvm::Value >() != f) ||
      (f->isDeclaration() ? !t->is_declaration()
                          : !t->is_definition() || !t->has_frontend()))
    return nullptr;
  const bool old_nondet = f->isDeclaration() && c->arg_empty() &&
                          (f->getName() == "__VERIFIER_nondet_int" ||
                           f->getName() == "__VERIFIER_nondet_uint");
  if (c->arg_size() != f->arg_size() ||
      (!old_nondet &&
       (f->isVarArg() || c->getFunctionType() != f->getFunctionType())))
    return nullptr;
  for (unsigned n = 0; n < c->arg_size(); ++n) {
    if (a->argument(n)->type() != t->type()->param_type(n))
      return nullptr;
    for (auto attr : {llvm::Attribute::ByVal,
                      llvm::Attribute::StructRet,
                      llvm::Attribute::InAlloca,
                      llvm::Attribute::Preallocated})
      if (c->paramHasAttr(n, attr) || f->hasParamAttribute(n, attr))
        return nullptr;
  }
  return a;
}

} // namespace analyzer
} // namespace ikos
