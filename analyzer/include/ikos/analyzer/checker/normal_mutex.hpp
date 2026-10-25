// Copyright and license: see data_race.hpp.
#pragma once

#include <ikos/analyzer/analysis/execution_engine/native_thread_handles.hpp>

namespace ikos {
namespace analyzer {

struct NormalMutex {
  llvm::GlobalVariable* source = nullptr;
  ar::GlobalVariable* object = nullptr;
  bool lock = false;
};

// A concrete default-normal glibc mutex, not a MUST-lock snapshot or a name.
// The consumer must separately close all address uses and ownership transfers.
inline NormalMutex normal_mutex(Context& ctx, ar::CallBase* call) {
  auto* c = call && call->has_frontend()
                ? llvm::dyn_cast< llvm::CallInst >(call->frontend< llvm::Value >()) : nullptr;
  auto* f = c ? llvm::dyn_cast< llvm::Function >(c->getCalledOperand()->stripPointerCasts()) : nullptr;
  auto* target = call ? native_handle_callee(call) : nullptr;
  if (!c || !f || !target || !target->is_declaration() || !f->isDeclaration() ||
      c->isInlineAsm() || c->hasOperandBundles() || !c->use_empty() ||
      c->getCallingConv() != llvm::CallingConv::C || f->getCallingConv() != llvm::CallingConv::C ||
      f->isVarArg() || c->getFunctionType() != f->getFunctionType() ||
      c->arg_size() != 1 || f->arg_size() != 1 || call->num_arguments() != 1 ||
      !c->getType()->isIntegerTy(32) || !call->argument(0)->type()->is_pointer()) return {};
  bool lock = f->getName() == "pthread_mutex_lock" && target->intrinsic_id() == ar::Intrinsic::PthreadMutexLock;
  bool unlock = f->getName() == "pthread_mutex_unlock" && target->intrinsic_id() == ar::Intrinsic::PthreadMutexUnlock;
  auto* pointer = llvm::dyn_cast< llvm::PointerType >(c->getArgOperand(0)->getType());
  auto* type = pointer ? llvm::dyn_cast< llvm::StructType >(pointer->getPointerElementType()) : nullptr;
  auto* g = llvm::dyn_cast< llvm::GlobalVariable >(c->getArgOperand(0)->stripPointerCasts());
  if ((!lock && !unlock) || !pointer || pointer->getAddressSpace() != 0 || !type || type->isOpaque() ||
      !type->isSized() || !type->hasName() || type->getName() != "union.pthread_mutex_t" ||
      !g || g->isConstant() || g->isThreadLocal() || g->isExternallyInitialized() ||
      g->getAddressSpace() != 0 || !g->hasInitializer() || !g->getInitializer()->isNullValue()) return {};
  for (auto a : {llvm::Attribute::ByVal, llvm::Attribute::StructRet,
                 llvm::Attribute::InAlloca, llvm::Attribute::Preallocated})
    if (c->paramHasAttr(0, a) || f->hasParamAttribute(0, a)) return {};
  const auto& layout = g->getParent()->getDataLayout();
  const auto abi = pthread_object_layout(ctx.bundle, g->getParent());
  const auto align = g->getAlign() ? g->getAlign()->value() : layout.getABITypeAlign(g->getValueType()).value();
  if (!abi || layout.getTypeAllocSize(type) != abi.mutex ||
      layout.getTypeAllocSize(g->getValueType()) != abi.mutex || align < abi.word) return {};
  for (auto it = ctx.bundle->global_begin(); it != ctx.bundle->global_end(); ++it) {
    auto* object = *it;
    if (!object->has_frontend() || object->frontend< llvm::Value >() != g ||
        !object->is_definition() || object->is_thread_local() ||
        !exact_handle_slot(call->argument(0), object) ||
        ctx.bundle->data_layout().alloc_size_in_bytes(object->type()->pointee()) != abi.mutex) continue;
    unsigned count = 0;
    for (auto* b : *object->initializer()) for (auto* s : *b) {
      auto* store = dyn_cast< ar::Store >(s);
      if (!store || store->pointer() != object || !isa< ar::AggregateZeroConstant >(store->value()) ||
          store->value()->type() != object->type()->pointee()) return {};
      ++count;
    }
    if (count == 1) return {g, object, lock};
  }
  return {};
}
} // namespace analyzer
} // namespace ikos
