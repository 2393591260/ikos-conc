// Copyright and license: see numerical.hpp.
#pragma once

#include <llvm/IR/Operator.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>
#include <ikos/analyzer/analysis/literal.hpp>
#include <ikos/analyzer/analysis/execution_engine/heap_pointer_effects.hpp>
#include <ikos/analyzer/analysis/execution_engine/handle_origin.hpp>

namespace ikos {
namespace analyzer {

// The complete, defined dereference of this actual inbounds GEP selects a leaf
// of the whole global, even when the numerical index is TOP. No prefix pruning.
inline bool whole_pointer_array_access(ar::Statement* statement,
                                        ar::GlobalVariable* global) {
  if (!statement->has_frontend() || !global->has_frontend()) return false;
  auto* source = statement->frontend< llvm::Value >();
  llvm::Value* pointer = nullptr;
  llvm::Type* leaf = nullptr;
  if (auto* load = llvm::dyn_cast< llvm::LoadInst >(source)) {
    pointer = load->getPointerOperand(); leaf = load->getType();
    if (load->isVolatile() || load->isAtomic()) return false;
  } else if (auto* store = llvm::dyn_cast< llvm::StoreInst >(source)) {
    pointer = store->getPointerOperand(); leaf = store->getValueOperand()->getType();
    if (store->isVolatile() || store->isAtomic()) return false;
  } else return false;
  auto* gep = llvm::dyn_cast< llvm::GEPOperator >(pointer);
  auto* root = global->frontend< llvm::GlobalVariable >();
  auto* array = llvm::dyn_cast< llvm::ArrayType >(root->getValueType());
  auto* ar_array = dyn_cast< ar::ArrayType >(global->type()->pointee());
  if (!gep || !gep->isInBounds() || gep->getPointerOperand() != root ||
      !array || !ar_array || !leaf->isPointerTy() ||
      llvm::cast< llvm::PointerType >(leaf)->getAddressSpace() != 0 ||
      llvm::cast< llvm::PointerType >(leaf)->getPointerElementType()->isFunctionTy() ||
      array->getElementType() != leaf || gep->getSourceElementType() != array ||
      gep->getNumIndices() != 2 || !ar_array->element_type()->is_pointer() ||
      ar_array->num_elements() != core::ZNumber(array->getNumElements())) return false;
  auto* zero = llvm::dyn_cast< llvm::ConstantInt >(gep->getOperand(1));
  const auto& layout = root->getParent()->getDataLayout();
  return zero && zero->isZero() &&
      core::ZNumber(layout.getTypeStoreSize(leaf).getFixedValue()) == global->bundle()->data_layout().store_size_in_bytes(
          ar_array->element_type()) &&
      core::ZNumber(layout.getTypeAllocSize(array).getFixedValue()) == global->bundle()->data_layout().alloc_size_in_bytes(ar_array);
}

inline bool prove_readonly_pointer_array(ar::GlobalVariable* global,
                                          Context* ctx = nullptr,
                                          const GlobalArraySubobject* object = nullptr) {
  auto* array = object ? object->type : dyn_cast< ar::ArrayType >(global->type()->pointee());
  auto access = [&](ar::Statement* statement) {
    if (!object || !object->nested()) return whole_pointer_array_access(statement, global);
    auto proof = ctx->subarray_bounds_certificates.find(statement);
    return proof != ctx->subarray_bounds_certificates.end() && proof->second == *object &&
           pointer_array_access(statement, *object);
  };
  auto mutex_call = [&](ar::CallBase* call) {
    if (!ctx || !object || !object->nested() ||
        !ctx->mutex_effect_certificates.count(call)) return false;
    auto mutex = mutex_array_effect_object(call);
    return mutex && mutex.global == global &&
        (mutex.begin + mutex.size <= object->begin || object->begin + object->size <= mutex.begin);
  };
  if (!array || !array->element_type()->is_pointer() || array->num_elements() <= 0 ||
      !global->is_definition() || global->is_thread_local() || !global->has_frontend()) return false;
  auto* source = global->frontend< llvm::GlobalVariable >();
  if (!source->hasInitializer() || !source->getInitializer()->isNullValue() ||
      source->getAddressSpace() != 0 || source->isExternallyInitialized()) return false;
  if (object && object->nested()) {
    if (!ctx) return false;
    std::set< llvm::Value* > accesses, calls;
    for (const auto& item : ctx->subarray_bounds_certificates)
      if (item.second == *object && item.first->has_frontend())
        accesses.insert(item.first->frontend< llvm::Value >());
    for (auto* call : ctx->mutex_effect_certificates)
      if (mutex_call(call) && call->has_frontend()) calls.insert(call->frontend< llvm::Value >());
    if (!closed_global_array_uses(global, [&](llvm::User* use) {
          return accesses.count(use) || calls.count(use);
        })) return false;
  } else if (!closed_global_array_uses(global, [](llvm::User* use) {
               return llvm::isa< llvm::LoadInst >(use) || llvm::isa< llvm::StoreInst >(use);
             })) return false;
  std::vector< ar::Store* > writers;
  std::vector< ar::CallBase* > creates;
  unsigned budget = 8192;
  auto scan = [&](ar::Code* code) {
    if (!code) return true;
    for (auto* block : *code) for (auto* statement : *block) {
      if (budget-- == 0) return false;
      if (auto* call = dyn_cast< ar::CallBase >(statement)) {
        auto* called = call->called();
        for (unsigned depth = 0; depth < 16; ++depth) {
          auto* variable = dyn_cast< ar::InternalVariable >(called);
          auto* cast = variable ? dyn_cast_or_null< ar::UnaryOperation >(unique_def(variable)) : nullptr;
          if (!cast || cast->op() != ar::UnaryOperation::Bitcast) break;
          called = cast->operand();
        }
        auto* function = dyn_cast< ar::FunctionPointerConstant >(called);
        auto* target = function ? function->function() : nullptr;
        if (target && target->name() == "pthread_create") creates.push_back(call);
        // An indirect call could introduce another entry before initialization.
        if (!target) return false;
      }
      if (auto* call = dyn_cast< ar::CallBase >(statement))
        if (mutex_call(call)) continue;
      bool mentioned = false;
      for (auto it = statement->op_begin(); it != statement->op_end(); ++it)
        mentioned |= mentions_handle_slot(*it, global);
      if (!mentioned) continue;
      if (auto* load = dyn_cast< ar::Load >(statement)) {
        if (load->result()->type()->is_pointer() &&
            access(load)) continue;
      } else if (auto* store = dyn_cast< ar::Store >(statement)) {
        if (!code->is_function_body()) {
          if (code == global->initializer_or_null() && store->pointer() == global &&
              isa< ar::AggregateZeroConstant >(store->value()) &&
              store->value()->type() == global->type()->pointee()) continue;
          return false;
        }
        if (code->function()->name() == "main" &&
            store->value()->type()->is_pointer() &&
            !mentions_handle_slot(store->value(), global) &&
            access(store)) {
          writers.push_back(store); continue;
        }
      } else if (auto* copy = dyn_cast< ar::Assignment >(statement)) {
        if (copy->result()->type()->is_pointer() && unique_def(copy->result()) == copy) continue;
      } else if (auto* cast = dyn_cast< ar::UnaryOperation >(statement)) {
        if (cast->op() == ar::UnaryOperation::Bitcast && cast->result()->type()->is_pointer() &&
            unique_def(cast->result()) == cast) continue;
      } else if (auto* shift = dyn_cast< ar::PointerShift >(statement)) {
        if (unique_def(shift->result()) == shift) continue;
      }
      return false;
    }
    return true;
  };
  auto* bundle = global->bundle();
  if (bundle->global_or_null("ar.global_ctors")) return false;
  for (auto it = bundle->function_begin(); it != bundle->function_end(); ++it)
    if (!scan((*it)->body_or_null())) return false;
  for (auto it = bundle->global_begin(); it != bundle->global_end(); ++it)
    if (!scan((*it)->initializer_or_null())) return false;
  for (auto* create : creates) {
    if (!create->code()->is_function_body() || create->code()->function()->name() != "main")
      return false;
    for (auto* writer : writers) {
      if (writer->code() != create->code()) return false;
      std::unordered_set< ar::BasicBlock* > seen;
      std::vector< ar::BasicBlock* > pending{create->parent()};
      while (!pending.empty()) {
        if (budget-- == 0) return false;
        auto* block = pending.back(); pending.pop_back();
        if (!seen.insert(block).second) continue;
        if (block == writer->parent()) return false;
        pending.insert(pending.end(), block->successor_begin(), block->successor_end());
      }
    }
  }
  return true;
}

inline bool prove_readonly_pointer_array(const GlobalArraySubobject& object, Context& ctx) {
  return object && prove_readonly_pointer_array(object.global, &ctx, &object);
}

template < typename E >
bool readonly_array_value(E& eng, ar::Load* load, const ScalarLit& pointer,
                           core::ConcurrentGlobalEnv::PointerValue& value) {
  if (!eng.ctx().opts.no_init_globals.empty()) return false;
  for (auto* root : eng.ctx().opts.entry_points) if (root->name() != "main") return false;
  auto sources = eng.data().normal().pointer_to_points_to(pointer.var());
  if (!sources.is_set() || sources.size() != 1) return false;
  auto* source = dyn_cast< GlobalMemoryLocation >(*sources.begin());
  if (!source) return false;
  auto& ctx = eng.ctx();
  auto& env = ctx.concurrent_env;
  auto* global = source->global_var();
  if (!env.function_executes_at_most_once(global->bundle()->function_or_null("main"))) return false;
  if (whole_pointer_array_access(load, global)) {
    bool certified = false;
    if (!env.get_readonly_array_certificate(global, certified)) {
      certified = prove_readonly_pointer_array(global);
      env.cache_readonly_array_certificate(global, certified);
    }
    return certified && env.get_readonly_array_pointer(source, value);
  }
  auto proof = ctx.subarray_bounds_certificates.find(load);
  if (proof == ctx.subarray_bounds_certificates.end()) return false;
  const auto& object = proof->second;
  if (object.global != global || !object.nested() || !pointer_array_access(load, object) ||
      !subarray_address_within(eng, load->operand(), object,
          ctx.bundle->data_layout().store_size_in_bytes(load->result()->type()))) return false;
  auto key = std::make_tuple(global, object.type, object.begin, object.size);
  auto cached = ctx.readonly_subarray_certificates.find(key);
  if (cached == ctx.readonly_subarray_certificates.end())
    cached = ctx.readonly_subarray_certificates.emplace(key, prove_readonly_pointer_array(object, ctx)).first;
  return cached->second && env.get_readonly_array_pointer(source, value);
}

} // namespace analyzer
} // namespace ikos
