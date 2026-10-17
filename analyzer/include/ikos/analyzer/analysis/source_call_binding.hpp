// Copyright and license: see execution_engine/numerical.hpp.
#pragma once

#include <ikos/analyzer/analysis/execution_engine/native_thread_handles.hpp>

namespace ikos {
namespace analyzer {

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
