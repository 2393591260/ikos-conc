// Copyright and license: see numerical.hpp.
#pragma once

#include <cstdlib>

#include <llvm/ADT/Triple.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Dominators.h>
#include <ikos/analyzer/util/log.hpp>
#include <ikos/analyzer/analysis/source_c_abi.hpp>
#include <ikos/analyzer/analysis/context.hpp>
#include <ikos/analyzer/analysis/execution_engine/global_array_subobject.hpp>
#include <ikos/analyzer/analysis/execution_engine/heap_pointer_effects.hpp>
#include <ikos/analyzer/analysis/execution_engine/protected_values.hpp>

namespace ikos {
namespace analyzer {

inline ar::Function* native_handle_callee(ar::CallBase* call) {
  auto* value = call->called();
  for (unsigned depth = 0; depth < 32; ++depth) {
    if (auto* function = dyn_cast< ar::FunctionPointerConstant >(value)) return function->function();
    auto* variable = dyn_cast< ar::InternalVariable >(value);
    if (variable && variable->code()->is_function_body())
      for (auto it = variable->code()->function()->param_begin(); it != variable->code()->function()->param_end(); ++it)
        if (*it == variable) return nullptr;
    auto* definition = variable ? unique_def(variable) : nullptr;
    if (auto* copy = dyn_cast_or_null< ar::Assignment >(definition)) value = copy->operand();
    else if (auto* cast = dyn_cast_or_null< ar::UnaryOperation >(definition)) {
      if (cast->op() != ar::UnaryOperation::Bitcast) return nullptr;
      value = cast->operand();
    } else return nullptr;
  }
  return nullptr;
}

inline bool native_repeated_create(ar::CallBase* call) {
  auto* start = call->parent();
  std::vector< ar::BasicBlock* > pending(start->successor_begin(), start->successor_end());
  std::set< ar::BasicBlock* > seen;
  while (!pending.empty()) {
    auto* block = pending.back(); pending.pop_back();
    if (block == start) return true;
    if (!seen.insert(block).second) continue;
    if (seen.size() > 1024) return false;
    pending.insert(pending.end(), block->successor_begin(), block->successor_end());
  }
  return false;
}

// Scalar one-create is a positive acyclicity proof on both representations.
// A bounded search that did not finish cannot certify it.
inline bool native_handle_scalar_acyclic(llvm::CallBase* source,
                                         ar::CallBase* imported) {
  auto* source_start = source->getParent();
  std::vector< llvm::BasicBlock* > native_pending;
  auto* terminal = source_start->getTerminator();
  for (unsigned i = 0; i < terminal->getNumSuccessors(); ++i)
    native_pending.push_back(terminal->getSuccessor(i));
  std::set< llvm::BasicBlock* > native_seen;
  while (!native_pending.empty()) {
    auto* block = native_pending.back(); native_pending.pop_back();
    if (block == source_start) return false;
    if (!native_seen.insert(block).second) continue;
    if (native_seen.size() > 1024) return false;
    auto* end = block->getTerminator();
    for (unsigned i = 0; i < end->getNumSuccessors(); ++i)
      native_pending.push_back(end->getSuccessor(i));
  }
  auto* start = imported->parent();
  std::vector< ar::BasicBlock* > pending(start->successor_begin(), start->successor_end());
  std::set< ar::BasicBlock* > seen;
  while (!pending.empty()) {
    auto* block = pending.back(); pending.pop_back();
    if (block == start) return false;
    if (!seen.insert(block).second) continue;
    if (seen.size() > 1024) return false;
    pending.insert(pending.end(), block->successor_begin(), block->successor_end());
  }
  return true;
}

inline llvm::Function* native_handle_api(llvm::CallBase* call, bool create) {
  if (!llvm::isa< llvm::CallInst >(call) || call->isInlineAsm() ||
      call->hasOperandBundles()) return nullptr;
  const auto abi = pthread_object_layout(
      call->getModule()->getDataLayout().getPointerSizeInBits(0), call->getModule());
  if (!abi) return nullptr;
  const auto bits = abi.word * 8;
  auto* function = llvm::dyn_cast< llvm::Function >(call->getCalledOperand()->stripPointerCasts());
  const unsigned n = create ? 4 : 2;
  if (!function || !function->isDeclaration() || function->isVarArg() ||
      function->getName() != (create ? "pthread_create" : "pthread_join") ||
      function->arg_size() != n || call->arg_size() != n || !call->use_empty() ||
      function->getCallingConv() != llvm::CallingConv::C || call->getCallingConv() != llvm::CallingConv::C ||
      !function->getReturnType()->isIntegerTy(32) || !call->getType()->isIntegerTy(32) ||
      call->getFunctionType() != function->getFunctionType() ||
      !ordinary_source_call_abi_attributes(call, function)) return nullptr;
  for (unsigned i = 0; i < n; ++i) {
    auto* type = function->getFunctionType()->getParamType(i);
    if (type != call->getArgOperand(i)->getType()) return nullptr;
    for (auto attr : {llvm::Attribute::ByVal, llvm::Attribute::StructRet,
                      llvm::Attribute::InAlloca, llvm::Attribute::Preallocated})
      if (call->paramHasAttr(i, attr) || function->hasParamAttribute(i, attr))
        return nullptr;
    if (i == 0 && !create) { if (!type->isIntegerTy(bits)) return nullptr; }
    else {
      auto* pointer = llvm::dyn_cast< llvm::PointerType >(type);
      if (!pointer || pointer->getAddressSpace() != 0) return nullptr;
    }
  }
  if (!llvm::isa< llvm::ConstantPointerNull >(call->getArgOperand(1))) return nullptr;
  if (create) {
    auto* output = llvm::cast< llvm::PointerType >(call->getArgOperand(0)->getType());
    auto* entry = llvm::dyn_cast< llvm::Function >(call->getArgOperand(2)->stripPointerCasts());
    auto* signature = llvm::cast< llvm::PointerType >(call->getArgOperand(2)->getType());
    if (!output->getPointerElementType()->isIntegerTy(bits) ||
        !llvm::isa< llvm::ConstantPointerNull >(call->getArgOperand(3)) || !entry || entry->isDeclaration() ||
        entry->isVarArg() || entry->arg_size() != 1 || !entry->getReturnType()->isPointerTy() ||
        !entry->getFunctionType()->getParamType(0)->isPointerTy() || entry->getName() == "main" ||
        entry->getCallingConv() != llvm::CallingConv::C ||
        !ordinary_source_function_abi_attributes(entry) ||
        signature->getPointerElementType() != entry->getFunctionType() ||
        llvm::cast< llvm::PointerType >(entry->getReturnType())->getAddressSpace() != 0 ||
        llvm::cast< llvm::PointerType >(entry->getFunctionType()->getParamType(0))->getAddressSpace() != 0) return nullptr;
  } else {
    auto* output = llvm::cast< llvm::PointerType >(call->getArgOperand(1)->getType());
    if (!output->getPointerElementType()->isPointerTy()) return nullptr;
  }
  return function;
}

// POSIX.1-2024 pthread_detach changes joinability, not application memory or
// termination. ponytail: direct-main scalar slots only; wider uses need a
// lifecycle/value model. No join or identity observer may consume that state.
inline bool closed_pthread_detach(ar::CallBase* query, Context& ctx) {
  auto* source = query->has_frontend()
      ? llvm::dyn_cast< llvm::CallInst >(query->frontend< llvm::Value >()) : nullptr;
  const auto abi = pthread_object_layout(ctx.bundle, source ? source->getModule() : nullptr);
  if (!abi) return false;
  const auto bits = abi.word * 8;
  auto* api = source ? source->getCalledFunction() : nullptr;
  auto* target = dyn_cast< ar::FunctionPointerConstant >(query->called());
  auto* function = target ? target->function() : nullptr;
  auto* main = query->code()->function_or_null();
  if (!source || !api || !source->getModule()->getModuleInlineAsm().empty() ||
      !function || !main || main->name() != "main" ||
      !main->has_frontend() || source->getFunction() != main->frontend< llvm::Value >() ||
      !source->getFunction()->use_empty() || ctx.opts.entry_points.size() != 1 ||
      ctx.opts.entry_points.front() != main || !ctx.opts.no_init_globals.empty() ||
      ctx.bundle->global_or_null("ar.global_ctors") || ctx.bundle->global_or_null("ar.global_dtors") ||
      !api->isDeclaration() || api->getName() != "pthread_detach" || api->isVarArg() ||
      api->arg_size() != 1 || source->arg_size() != 1 || !source->use_empty() ||
      source->hasOperandBundles() || source->getCallingConv() != llvm::CallingConv::C ||
      api->getCallingConv() != llvm::CallingConv::C ||
      source->getFunctionType() != api->getFunctionType() ||
      !source->getType()->isIntegerTy(32) || !api->getReturnType()->isIntegerTy(32) ||
      !api->getFunctionType()->getParamType(0)->isIntegerTy(bits) ||
      !function->is_declaration() || function->name() != "pthread_detach" ||
      !function->has_frontend() || function->frontend< llvm::Value >() != api ||
      function->type()->is_var_arg() || function->num_parameters() != 1 ||
      query->num_arguments() != 1 || !isa< ar::Call >(query)) return false;
  auto integer = [bits](ar::Type* type) {
    auto* value = dyn_cast< ar::IntegerType >(type);
    return value && value->bit_width() == bits;
  };
  auto* returned = dyn_cast< ar::IntegerType >(function->type()->return_type());
  if (!returned || returned->bit_width() != 32 || !returned->is_signed() ||
      !integer(query->argument(0)->type()) ||
      function->type()->param_type(0) != query->argument(0)->type() ||
      (query->has_result() && query->result()->type() != returned)) return false;
  const auto& layout = source->getModule()->getDataLayout();

  auto attributes = [](llvm::CallBase* call) {
    auto* callee = call->getCalledFunction();
    if (!callee || call->doesNotReturn() || call->hasFnAttr(llvm::Attribute::ReturnsTwice) ||
        callee->hasFnAttribute(llvm::Attribute::ReturnsTwice)) return false;
    for (unsigned i = 0; i < call->arg_size(); ++i) {
      for (auto set : {call->getAttributes().getParamAttrs(i),
                       callee->getAttributes().getParamAttrs(i)})
        for (auto attr : set) {
          if (attr.isEnumAttribute() && attr.getKindAsEnum() == llvm::Attribute::NoUndef) continue;
          if (attr.isEnumAttribute() && attr.getKindAsEnum() == llvm::Attribute::NonNull &&
              (llvm::isa< llvm::AllocaInst >(call->getArgOperand(i)) ||
               llvm::isa< llvm::Function >(call->getArgOperand(i)))) continue;
          return false;
        }
    }
    return true;
  };
  auto previous = [](llvm::Instruction* instruction) {
    auto* result = instruction->getPrevNode();
    while (result && llvm::isa< llvm::DbgInfoIntrinsic >(result)) result = result->getPrevNode();
    return result;
  };
  auto* load = llvm::dyn_cast< llvm::LoadInst >(source->getArgOperand(0));
  auto* storage = load ? llvm::dyn_cast< llvm::AllocaInst >(load->getPointerOperand()) : nullptr;
  auto* count = storage ? llvm::dyn_cast< llvm::ConstantInt >(storage->getArraySize()) : nullptr;
  auto* create = load ? llvm::dyn_cast_or_null< llvm::CallInst >(previous(load)) : nullptr;
  if (!attributes(source) || !load || !storage || !count || !count->isOne() ||
      !storage->getAllocatedType()->isIntegerTy(bits) || storage->getAddressSpace() != 0 ||
      layout.getTypeAllocSize(storage->getAllocatedType()) != abi.word ||
      layout.getTypeStoreSize(storage->getAllocatedType()) != abi.word ||
      storage->getParent() != &source->getFunction()->getEntryBlock() || storage->getAlign().value() < abi.word ||
      load->isVolatile() || load->isAtomic() || load->getAlign().value() < abi.word ||
      !load->hasOneUse() || previous(source) != load || !create ||
      create->hasOperandBundles() || !native_handle_api(create, true) || !attributes(create) ||
      create->getArgOperand(0) != storage || !llvm::isa< llvm::Function >(create->getArgOperand(2))) return false;
  auto* worker = llvm::cast< llvm::Function >(create->getArgOperand(2));
  if (worker->hasFnAttribute(llvm::Attribute::NoReturn) ||
      worker->hasFnAttribute(llvm::Attribute::ReturnsTwice)) return false;
  for (auto attr : worker->getAttributes().getParamAttrs(0))
    if (!attr.isEnumAttribute() || attr.getKindAsEnum() != llvm::Attribute::NoUndef) return false;
  llvm::StoreInst* initialization = nullptr;
  for (auto* user : storage->users()) {
    if (user == create || user == load || llvm::isa< llvm::DbgInfoIntrinsic >(user)) continue;
    auto* store = llvm::dyn_cast< llvm::StoreInst >(user);
    if (!store || initialization || store->getPointerOperand() != storage ||
        !store->getValueOperand()->getType()->isIntegerTy(bits) || store->isVolatile() || store->isAtomic() ||
        store->getParent() != storage->getParent() || !storage->comesBefore(store) ||
        (create->getParent() == store->getParent() && !store->comesBefore(create))) return false;
    initialization = store;
  }

  // Reject observers in both representations, including unresolved calls that
  // might hide a join. This does not assume failed creates produce fresh ids.
  auto observer = [](const std::string& name) {
    return name == "pthread_join" || name == "pthread_self" || name == "pthread_equal" ||
           name == "pthread_attr_getdetachstate";
  };
  unsigned budget = 32768;
  for (auto& f : *source->getModule()) for (auto& block : f) for (auto& instruction : block) {
    if (budget == 0) return false;
    --budget;
    auto* call = llvm::dyn_cast< llvm::CallBase >(&instruction);
    if (!call || llvm::isa< llvm::DbgInfoIntrinsic >(call)) continue;
    auto* callee = llvm::dyn_cast< llvm::Function >(call->getCalledOperand()->stripPointerCasts());
    if (!callee || observer(callee->getName().str())) return false;
  }
  ar::Allocate* allocation = nullptr;
  ar::CallBase* imported_create = nullptr;
  ar::Load* imported_load = nullptr;
  ar::Store* imported_initialization = nullptr;
  for (auto it = ctx.bundle->function_begin(); it != ctx.bundle->function_end(); ++it) {
    if (!(*it)->is_definition()) continue;
    for (auto* block : *(*it)->body()) for (auto* statement : *block) {
      if (budget == 0) return false;
      --budget;
      if (auto* call = dyn_cast< ar::CallBase >(statement)) {
        auto* callee = native_handle_callee(call);
        if (!callee || observer(callee->name()) ||
            callee->intrinsic_id() == ar::Intrinsic::PthreadJoin) return false;
      }
      if (!statement->has_frontend() || statement->code() != query->code()) continue;
      auto* original = statement->frontend< llvm::Value >();
      if (original == storage && isa< ar::Allocate >(statement)) {
        if (allocation) return false;
        allocation = cast< ar::Allocate >(statement);
      } else if (original == create && isa< ar::CallBase >(statement)) {
        if (imported_create) return false;
        imported_create = cast< ar::CallBase >(statement);
      } else if (original == load && isa< ar::Load >(statement)) {
        if (imported_load) return false;
        imported_load = cast< ar::Load >(statement);
      } else if (initialization && original == initialization && isa< ar::Store >(statement)) {
        if (imported_initialization) return false;
        imported_initialization = cast< ar::Store >(statement);
      }
    }
  }
  auto* repetitions = allocation ? dyn_cast< ar::IntegerConstant >(allocation->array_size()) : nullptr;
  auto* imported_api = imported_create ? native_handle_callee(imported_create) : nullptr;
  auto* imported_worker = imported_create && imported_create->num_arguments() == 4
      ? dyn_cast< ar::FunctionPointerConstant >(imported_create->argument(2)) : nullptr;
  auto* create_returned = imported_api
      ? dyn_cast< ar::IntegerType >(imported_api->type()->return_type()) : nullptr;
  if (!allocation || !repetitions || repetitions->value().to_z_number() != 1 ||
      !integer(allocation->allocated_type()) || allocation->result()->alignment() < abi.word ||
      ctx.bundle->data_layout().alloc_size_in_bytes(allocation->allocated_type()) != abi.word ||
      ctx.bundle->data_layout().store_size_in_bytes(allocation->allocated_type()) != abi.word ||
      allocation->parent() != main->body()->entry_block() ||
      !imported_load || !imported_create || !imported_api || !imported_api->is_declaration() ||
      !isa< ar::Call >(imported_create) ||
      (imported_api->name() != "pthread_create" &&
       imported_api->intrinsic_id() != ar::Intrinsic::PthreadCreate) ||
      imported_api->type()->is_var_arg() || imported_api->num_parameters() != 4 ||
      !create_returned || create_returned->bit_width() != 32 || !create_returned->is_signed() ||
      (imported_create->has_result() && imported_create->result()->type() != create_returned) ||
      !imported_api->has_frontend() || imported_api->frontend< llvm::Value >() != create->getCalledFunction() ||
      imported_create->num_arguments() != 4 || imported_create->argument(0) != allocation->result() ||
      !isa< ar::NullConstant >(imported_create->argument(1)) ||
      !isa< ar::NullConstant >(imported_create->argument(3)) ||
      !imported_worker || !imported_worker->function()->is_definition() ||
      imported_worker->function()->num_parameters() != 1 ||
      !imported_worker->function()->param(0)->type()->is_pointer() ||
      !imported_worker->function()->type()->return_type()->is_pointer() ||
      !imported_worker->function()->has_frontend() ||
      imported_worker->function()->frontend< llvm::Value >() != worker ||
      imported_load->operand() != allocation->result() || imported_load->alignment() < abi.word ||
      imported_load->is_volatile() || imported_load->is_atomic() ||
      !integer(imported_load->result()->type()) || query->argument(0) != imported_load->result() ||
      imported_create->parent() != query->parent() || imported_load->parent() != query->parent() ||
      bool(initialization) != bool(imported_initialization)) return false;
  for (unsigned i = 0; i < 4; ++i)
    if (imported_api->type()->param_type(i) != imported_create->argument(i)->type()) return false;
  if (imported_initialization &&
      (imported_initialization->parent() != allocation->parent() ||
       imported_initialization->pointer() != allocation->result() ||
       !integer(imported_initialization->value()->type()) || imported_initialization->is_volatile() ||
       imported_initialization->is_atomic())) return false;
  unsigned phase = 0;
  bool allocated = false, initialized = !initialization;
  for (auto* block : *main->body()) for (auto* statement : *block) {
    if (budget == 0) return false;
    --budget;
    if ((phase == 1 && statement != imported_load) || (phase == 2 && statement != query)) return false;
    if (statement == allocation) allocated = true;
    if (statement == imported_initialization) { if (!allocated) return false; initialized = true; }
    if (statement == imported_create) { if (!allocated || !initialized) return false; phase = 1; }
    if (statement == imported_load) { if (phase != 1) return false; phase = 2; }
    if (statement == query) { if (phase != 2) return false; phase = 3; }
    for (auto operand = statement->op_begin(); operand != statement->op_end(); ++operand) {
      if (*operand == allocation->result() && statement != imported_create &&
          statement != imported_load && statement != imported_initialization) return false;
      if (*operand == imported_load->result() && statement != query) return false;
      if ((query->has_result() && *operand == query->result()) ||
          (imported_create->has_result() && *operand == imported_create->result())) return false;
    }
  }
  return phase == 3;
}

inline bool native_handle_element(llvm::Value* value, llvm::AllocaInst* storage) {
  value = value->stripPointerCasts();
  if (value == storage) return true;
  auto* gep = llvm::dyn_cast< llvm::GEPOperator >(value);
  auto* zero = gep && gep->getNumIndices() == 2
      ? llvm::dyn_cast< llvm::ConstantInt >(gep->getOperand(1)) : nullptr;
  return gep && gep->getPointerOperand() == storage && gep->getSourceElementType() == storage->getAllocatedType() &&
      zero && zero->isZero() && pure_local_array_index(gep->getOperand(2));
}


// A scalar seed is frontend bookkeeping, never an observed handle identity.
// The native and imported sides must name the same exact store/call/allocation.
inline bool native_handle_scalar_seed(
    llvm::StoreInst* store, llvm::AllocaInst* storage,
    const NativeHandleArray& object,
    const std::map< llvm::Value*, ar::Statement* >& originals) {
  auto lookup = [&](llvm::Value* v) -> ar::Statement* {
    auto i = originals.find(v); return i == originals.end() ? nullptr : i->second;
  };
  auto* seed = llvm::dyn_cast< llvm::CallInst >(store->getValueOperand());
  auto* api = seed ? seed->getCalledFunction() : nullptr;
  auto* imported = dyn_cast_or_null< ar::Store >(lookup(store));
  auto* call = seed ? dyn_cast_or_null< ar::CallBase >(lookup(seed)) : nullptr;
  auto* target = call ? native_handle_callee(call) : nullptr;
  const auto bits = object.type->bit_width();
  return store->getPointerOperand() == storage && store->getAlign().value() >= bits / 8 &&
      !store->isAtomic() &&
      !store->isVolatile() && store->getMetadata("ikos.freeze_uninit") &&
      store->getParent() == storage->getParent() && seed &&
      store->getPrevNode() == seed && seed->hasOneUse() &&
      seed->getMetadata("ikos.freeze_uninit") && api && api->isDeclaration() &&
      !api->isVarArg() && api->arg_empty() && seed->arg_empty() &&
      !seed->hasOperandBundles() && !seed->isInlineAsm() &&
      seed->getCallingConv() == llvm::CallingConv::C &&
      api->getCallingConv() == llvm::CallingConv::C &&
      ordinary_source_call_abi_attributes(seed, api) &&
      seed->getFunctionType() == api->getFunctionType() &&
      api->getReturnType()->isIntegerTy(bits) &&
      api->getName() == (bits == 32 ? "__ikos_nondet_int" : "__ikos_nondet_int64") &&
      imported && imported->pointer() == object.storage &&
      !imported->is_atomic() && !imported->is_volatile() &&
      imported->alignment() >= bits / 8 && call && isa< ar::Call >(call) &&
      call->has_result() && imported->value() == call->result() &&
      call->result()->type()->is_integer() &&
      cast< ar::IntegerType >(call->result()->type())->bit_width() == bits &&
      call->num_arguments() == 0 &&
      target && target->is_declaration() &&
      (!target->has_frontend() || target->frontend< llvm::Value >() == api) &&
      target->intrinsic_id() == ar::Intrinsic::IkosNonDet &&
      target->num_parameters() == 0 && !target->type()->is_var_arg() &&
      target->type()->return_type() == call->result()->type() &&
      imported->code() == object.storage->function()->body() &&
      call->parent() == imported->parent();
}

// A native use audit is not an imported use audit. In particular, an extra AR
// copy or output use must not be hidden by a correctly closed LLVM alloca.
inline bool native_handle_imported_uses_closed(
    Context& ctx, const NativeHandleArray& object,
    const std::map< ar::Statement*, NativeHandleArray >& accesses,
    const std::set< ar::CallBase* >& joins,
    const std::set< ar::Store* >& seeds,
    const std::set< llvm::Value* >& native_addresses) {
  std::set< ar::Value* > addresses{object.storage};
  unsigned budget = 32768;
  bool changed = true;
  while (changed) {
    changed = false;
    for (auto f = ctx.bundle->function_begin(); f != ctx.bundle->function_end(); ++f) {
      if (!(*f)->is_definition()) continue;
      for (auto* block : *(*f)->body()) for (auto* s : *block) {
        if (!budget--) return false;
        auto* result = s->result_or_null();
        bool derived = false;
        if (auto* a = dyn_cast< ar::Assignment >(s)) derived = addresses.count(a->operand());
        else if (auto* u = dyn_cast< ar::UnaryOperation >(s))
          derived = u->op() == ar::UnaryOperation::Bitcast && addresses.count(u->operand());
        else if (auto* shift = dyn_cast< ar::PointerShift >(s)) derived = addresses.count(shift->pointer());
        if (!derived || !result || addresses.count(result)) continue;
        // The first scalar subset needs no imported pointer aliases. Arrays
        // retain their source GEP/cast grammar, with actual source identity.
        if (object.shape != NativeHandleShape::Array || !result->type()->is_pointer() ||
            !s->has_frontend() || !native_addresses.count(s->frontend< llvm::Value >()) ||
            local_output_base(result) != object.storage) return false;
        addresses.insert(result); changed = true;
      }
    }
  }
  std::set< ar::Value* > loaded, seed_values, unused_api_results;
  for (const auto& access : accesses) {
    auto* producer = dyn_cast< ar::CallBase >(access.first);
    if (producer && producer->has_result()) unused_api_results.insert(producer->result());
  }
  for (auto* join : joins)
    if (join->has_result()) unused_api_results.insert(join->result());
  for (const auto& a : accesses) if (a.second.storage == object.storage)
    if (auto* load = dyn_cast< ar::Load >(a.first)) loaded.insert(load->result());
  for (auto* seed : seeds) seed_values.insert(seed->value());
  auto code = [&](ar::Code* body) {
    if (!body) return true;
    for (auto* block : *body) for (auto* s : *block) {
      if (!budget--) return false;
      // A second allocation of the same AR local is a different lifetime.
      if (s->result_or_null() == object.storage) {
        auto* a = dyn_cast< ar::Allocate >(s);
        if (!a || !a->has_frontend() ||
            a->frontend< llvm::Value >() != object.storage->frontend< llvm::Value >()) return false;
      }
      for (auto op = s->op_begin(); op != s->op_end(); ++op) {
        if (!budget-- || unused_api_results.count(*op)) return false;
        if (addresses.count(*op)) {
          auto i = accesses.find(s);
          bool allowed = false;
          if (i != accesses.end() && i->second.storage == object.storage) {
            if (auto* call = dyn_cast< ar::CallBase >(s))
              allowed = call->num_arguments() == 4 && call->argument(0) == *op;
            if (auto* load = dyn_cast< ar::Load >(s)) allowed = load->operand() == *op;
          }
          if (auto* seed = dyn_cast< ar::Store >(s))
            allowed |= seeds.count(seed) && seed->pointer() == *op;
          if (s->result_or_null() && addresses.count(s->result_or_null())) allowed = true;
          if (!allowed) return false;
        }
        if (loaded.count(*op)) {
          auto* join = dyn_cast< ar::CallBase >(s);
          if (!join || !joins.count(join) || join->argument(0) != *op) return false;
        }
        if (seed_values.count(*op)) {
          auto* store = dyn_cast< ar::Store >(s);
          if (!store || !seeds.count(store) || store->value() != *op) return false;
        }
      }
    }
    return true;
  };
  for (auto f = ctx.bundle->function_begin(); f != ctx.bundle->function_end(); ++f)
    if (!code((*f)->body_or_null())) return false;
  for (auto g = ctx.bundle->global_begin(); g != ctx.bundle->global_end(); ++g)
    if (!code((*g)->initializer_or_null())) return false;
  return true;
}

// Exact source/AR marker and nondet call shapes for closed-effect consumers.
// This grants neither atomicity nor numerical or synchronization facts.
inline bool native_handle_range_call(ar::CallBase* call) {
  auto* site = call->has_frontend()
      ? llvm::dyn_cast< llvm::CallInst >(call->frontend< llvm::Value >()) : nullptr;
  auto* source = site ? llvm::dyn_cast< llvm::Function >(
      site->getCalledOperand()->stripPointerCasts()) : nullptr;
  auto* imported = native_handle_callee(call);
  if (!site || !source || !source->isDeclaration() || !imported ||
      !imported->is_declaration() || !isa< ar::Call >(call) ||
      site->hasOperandBundles() || site->isInlineAsm() ||
      !source->arg_empty() || !site->arg_empty() || call->num_arguments() != 0 ||
      imported->num_parameters() != 0 ||
      site->getCallingConv() != llvm::CallingConv::C ||
      source->getCallingConv() != llvm::CallingConv::C ||
      !ordinary_source_call_abi_attributes(site, source)) return false;
  const auto name = source->getName();
  const bool marker = name == "__VERIFIER_atomic_begin" || name == "__VERIFIER_atomic_end";
  if (!marker && name != "__VERIFIER_nondet_bool") return false;
  // C no-prototype markers may arrive through a void(...)* -> void()* cast.
  if (site->getFunctionType() != source->getFunctionType() &&
      !(marker && source->isVarArg() &&
        site->getFunctionType()->getNumParams() == 0 && site->getType()->isVoidTy()))
    return false;
  if (!marker && (source->isVarArg() || imported->type()->is_var_arg())) return false;
  if (imported->has_frontend()) {
    if (imported->frontend< llvm::Value >() != source) return false;
  } else if (marker || imported->intrinsic_id() != ar::Intrinsic::IkosNonDet) return false;
  if (marker) return source->getReturnType()->isVoidTy() &&
      site->getType()->isVoidTy() && imported->type()->return_type()->is_void() && !call->has_result();
  auto* result = dyn_cast< ar::IntegerType >(imported->type()->return_type());
  return source->getReturnType()->isIntegerTy(1) && site->getType()->isIntegerTy(1) &&
      result && result->bit_width() == 1 &&
      (!call->has_result() || call->result()->type() == result);
}

// This inventory selects a value abstraction, not an effect/termination proof.
// Every producer must use native TOP even if the later range preflight fails.
inline void configure_native_thread_handles(Context& ctx) {
  if (ctx.native_handle_inventory_checked) return;
  ctx.native_handle_inventory_checked = true;
  auto* bundle = ctx.bundle;
  if (!ctx.opts.enable_thread_modular ||
      ctx.opts.entry_points.size() != 1 || ctx.opts.entry_points.front()->name() != "main" ||
      ctx.opts.entry_points.front()->num_parameters() != 0 || !ctx.opts.no_init_globals.empty() ||
      bundle->global_or_null("ar.global_ctors") || bundle->global_or_null("ar.global_dtors") ||
      ctx.concurrent_env.has_synthetic_handle_slots()) return;
  auto* main = ctx.opts.entry_points.front();
  if (!main->has_frontend()) return;
  auto* source_main = main->frontend< llvm::Function >();
  if (!source_main->use_empty() || !source_main->getParent()->getModuleInlineAsm().empty()) return;
  if (!mutex_effect_pointer_representations_closed(*source_main->getParent())) return;
  const auto abi = pthread_object_layout(bundle, source_main->getParent());
  if (!abi) return;
  const auto bits = abi.word * 8;
  if (source_main->getParent()->getDataLayout().getIndexSizeInBits(0) != bits) return;
  std::map< llvm::Value*, ar::Statement* > originals;
  std::map< llvm::AllocaInst*, NativeHandleArray > arrays;
  std::map< ar::Statement*, NativeHandleArray > accesses;
  std::set< ar::CallBase* > joins;
  std::set< llvm::CallBase* > creates, source_joins;
  unsigned budget = 16384;
  for (auto fit = bundle->function_begin(); fit != bundle->function_end(); ++fit) {
    if (!(*fit)->is_definition()) continue;
    for (auto* block : *(*fit)->body()) for (auto* statement : *block) {
      if (budget-- == 0) return;
      if (statement->has_frontend() && (isa< ar::CallBase >(statement) || isa< ar::Load >(statement) ||
          isa< ar::Store >(statement) || isa< ar::Allocate >(statement))) {
        auto* original = statement->frontend< llvm::Value >();
        if (!originals.emplace(original, statement).second) return;
      }
      if (auto* allocation = dyn_cast< ar::Allocate >(statement)) {
        auto* count = dyn_cast< ar::IntegerConstant >(allocation->array_size());
        auto* array = dyn_cast< ar::ArrayType >(allocation->allocated_type());
        auto* element = array ? dyn_cast< ar::IntegerType >(array->element_type())
                              : dyn_cast< ar::IntegerType >(allocation->allocated_type());
        auto* source = allocation->has_frontend()
            ? llvm::dyn_cast< llvm::AllocaInst >(allocation->frontend< llvm::Value >()) : nullptr;
        auto* original = source ? llvm::dyn_cast< llvm::ArrayType >(source->getAllocatedType()) : nullptr;
        auto* repetitions = source ? llvm::dyn_cast< llvm::ConstantInt >(source->getArraySize()) : nullptr;
        if (allocation->code() != main->body() || allocation->parent() != main->body()->entry_block() ||
            !count || count->value().to_z_number() != 1 || !element || element->bit_width() != bits ||
            !source || source->getAddressSpace() != 0 || !allocation->result()->has_frontend() ||
            allocation->result()->frontend< llvm::Value >() != source ||
            source->getParent() != &source_main->getEntryBlock() || !repetitions || !repetitions->isOne()) continue;
        if (array ? (!original || array->num_elements() <= 0 ||
            !original->getElementType()->isIntegerTy(bits) ||
            array->num_elements() != core::ZNumber(original->getNumElements()))
            : !source->getAllocatedType()->isIntegerTy(bits)) continue;
        const auto bytes = bundle->data_layout().alloc_size_in_bytes(allocation->allocated_type());
        const auto& layout = source_main->getParent()->getDataLayout();
        if (!array && std::getenv("IKOS_RACE_DISABLE_NATIVE_SCALAR_HANDLES")) continue;
        const auto elements = array ? array->num_elements() : core::ZNumber(1);
        if (bytes != abi.word * elements || bytes >= (core::ZNumber(1) << bits) ||
            bytes != bundle->data_layout().store_size_in_bytes(allocation->allocated_type()) ||
            bytes != core::ZNumber(layout.getTypeAllocSize(source->getAllocatedType()).getFixedValue()) ||
            bytes != core::ZNumber(layout.getTypeStoreSize(source->getAllocatedType()).getFixedValue()) ||
            source->getAlign().value() < abi.word || allocation->result()->alignment() < abi.word) continue;
        arrays.emplace(source, NativeHandleArray{allocation->result(), element, bytes,
            array ? NativeHandleShape::Array : NativeHandleShape::Scalar});
      }
      auto* call = dyn_cast< ar::CallBase >(statement);
      if (!call) continue;
      auto* called = native_handle_callee(call);
      auto* original = call->has_frontend() ? llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >()) : nullptr;
      auto* callee = original ? llvm::dyn_cast< llvm::Function >(original->getCalledOperand()->stripPointerCasts()) : nullptr;
      if (!called || !callee) return;
      const bool pthread_api = callee->getName() == "pthread_create" || callee->getName() == "pthread_join";
      if (pthread_api) {
        const auto expected = callee->getName() == "pthread_create"
            ? ar::Intrinsic::PthreadCreate : ar::Intrinsic::PthreadJoin;
        if (!isa< ar::Call >(call) ||
            (called->has_frontend() ? called->frontend< llvm::Value >() != callee
                                    : called->intrinsic_id() != expected)) return;
        auto* result = dyn_cast< ar::IntegerType >(called->type()->return_type());
        if (!result || result->bit_width() != 32 || called->type()->is_var_arg() ||
            called->num_parameters() != call->num_arguments()) return;
        for (unsigned i = 0; i < call->num_arguments(); ++i)
          if (called->type()->param_type(i) != call->argument(i)->type()) return;
      }
      if (callee->getName() == "pthread_create" || called->intrinsic_id() == ar::Intrinsic::PthreadCreate) {
        if (!native_handle_api(original, true) || !called->is_declaration() ||
            (called->name() != "pthread_create" &&
             called->intrinsic_id() != ar::Intrinsic::PthreadCreate) ||
            call->code() != main->body() || call->num_arguments() != 4 ||
            !isa< ar::NullConstant >(call->argument(1)) || !isa< ar::NullConstant >(call->argument(3)) ||
            !isa< ar::FunctionPointerConstant >(call->argument(2))) return;
        auto* child = cast< ar::FunctionPointerConstant >(call->argument(2))->function();
        auto* original_child = llvm::cast< llvm::Function >(original->getArgOperand(2)->stripPointerCasts());
        if (!child->is_definition() || child->num_parameters() != 1 ||
            !child->type()->return_type()->is_pointer() || !child->param(0)->type()->is_pointer() ||
            !child->has_frontend() || child->frontend< llvm::Value >() != original_child) return;
        auto* pointer = dyn_cast< ar::PointerType >(call->argument(0)->type());
        auto* type = pointer ? dyn_cast< ar::IntegerType >(pointer->pointee()) : nullptr;
        if (!type || type->bit_width() != bits) return;
        creates.insert(original);
      } else if (callee->getName() == "pthread_join" || called->intrinsic_id() == ar::Intrinsic::PthreadJoin) {
        if (!native_handle_api(original, false) || !called->is_declaration() ||
            call->code() != main->body() || call->num_arguments() != 2 ||
            !isa< ar::NullConstant >(call->argument(1))) return;
        auto* type = dyn_cast< ar::IntegerType >(call->argument(0)->type());
        if (!type || type->bit_width() != bits ||
            (called->name() != "pthread_join" &&
             called->intrinsic_id() != ar::Intrinsic::PthreadJoin)) return;
        source_joins.insert(original);
        joins.insert(call);
      } else if (callee->getName().startswith("pthread_") &&
                 !callee->getName().startswith("pthread_mutex_")) return;
    }
  }
  if (creates.empty()) return;
  // Close the original producers independently of imported AR aliases.
  for (auto& function : *source_main->getParent()) for (auto& block : function) for (auto& instruction : block) {
    if (auto* call = llvm::dyn_cast< llvm::CallBase >(&instruction)) {
      if (llvm::isa< llvm::DbgInfoIntrinsic >(call)) continue;
      auto* callee = llvm::dyn_cast< llvm::Function >(call->getCalledOperand()->stripPointerCasts());
      if (!callee || (callee->getName() == "pthread_create" && !creates.count(call)) ||
          (callee->getName() == "pthread_join" && !source_joins.count(call)) ||
          callee->getName() == "pthread_self") return;
    }
  }
  std::set< llvm::CallBase* > accounted_creates, accounted_joins;
  for (const auto& item : arrays) {
    auto* storage = item.first;
    const auto& object = item.second;
    std::vector< llvm::Value* > pending{storage};
    std::set< llvm::Value* > seen;
    bool used = false, closed = true;
    std::set< llvm::CallBase* > array_joins, object_creates;
    std::set< ar::Store* > seeds;
    llvm::DominatorTree dominance(*source_main);
    while (!pending.empty() && closed) {
      auto* value = pending.back(); pending.pop_back();
      if (!seen.insert(value).second) continue;
      if (seen.size() > 2048) return;
      for (auto* user : value->users()) {
        if (auto* gep = llvm::dyn_cast< llvm::GEPOperator >(user)) {
          if (object.shape != NativeHandleShape::Array || gep->getPointerOperand() != value) { closed = false; break; }
          pending.push_back(user);
        } else if (llvm::isa< llvm::BitCastOperator >(user)) {
          if (object.shape != NativeHandleShape::Array) { closed = false; break; }
          pending.push_back(user);
        }
        else if (llvm::isa< llvm::DbgInfoIntrinsic >(user)) continue;
        else if (auto* call = llvm::dyn_cast< llvm::CallBase >(user)) {
          if (!creates.count(call) || call->getArgOperand(0) != value || !native_handle_element(value, storage)) {
            closed = false; break;
          }
          auto* imported = dyn_cast_or_null< ar::CallBase >(originals[call]);
          if (!imported || local_output_base(imported->argument(0)) != object.storage ||
              (object.shape == NativeHandleShape::Scalar
                   ? (imported->argument(0) != object.storage || !native_handle_scalar_acyclic(call, imported))
                   : !native_repeated_create(imported))) { closed = false; break; }
          object_creates.insert(call);
          accesses.emplace(imported, object); accounted_creates.insert(call); used = true;
        } else if (auto* load = llvm::dyn_cast< llvm::LoadInst >(user)) {
          if (load->getPointerOperand() != value || !load->getType()->isIntegerTy(bits) ||
              load->isVolatile() || load->isAtomic() || !native_handle_element(value, storage)) { closed = false; break; }
          auto* imported = dyn_cast_or_null< ar::Load >(originals[load]);
          auto* type = imported ? dyn_cast< ar::IntegerType >(imported->result()->type()) : nullptr;
          auto* pointer = imported ? dyn_cast< ar::PointerType >(imported->operand()->type()) : nullptr;
          auto* leaf = pointer ? dyn_cast< ar::IntegerType >(pointer->pointee()) : nullptr;
          if (!imported || !type || type->bit_width() != bits || !leaf || leaf->bit_width() != bits ||
              imported->is_volatile() || imported->is_atomic() ||
              local_output_base(imported->operand()) != object.storage ||
              (object.shape == NativeHandleShape::Scalar && imported->operand() != object.storage) ||
              imported->alignment() < abi.word || load->getAlign().value() < abi.word) { closed = false; break; }
          for (auto* use : load->users()) {
            auto* join = llvm::dyn_cast< llvm::CallBase >(use);
            if (!join || !source_joins.count(join) || join->getArgOperand(0) != load) { closed = false; break; }
            auto* imported_join = dyn_cast_or_null< ar::CallBase >(originals[join]);
            if (!imported_join || !joins.count(imported_join) ||
                imported_join->argument(0) != imported->result()) { closed = false; break; }
            array_joins.insert(join);
          }
          accesses.emplace(imported, object);
        } else if (auto* store = llvm::dyn_cast< llvm::StoreInst >(user)) {
          if (object.shape != NativeHandleShape::Scalar ||
              !native_handle_scalar_seed(store, storage, object, originals)) { closed = false; break; }
          seeds.insert(cast< ar::Store >(originals[store]));
        } else { closed = false; break; }
      }
    }
    if (used && object.shape == NativeHandleShape::Scalar) {
      if (object_creates.size() != 1 || seeds.size() > 1) closed = false;
      else for (auto* seed : seeds) {
        auto* source_seed = llvm::cast< llvm::StoreInst >(seed->frontend< llvm::Value >());
        if (!dominance.dominates(storage, source_seed) ||
            !dominance.dominates(source_seed, *object_creates.begin())) closed = false;
      }
    }
    if (!closed && used) return;
    if (used && !native_handle_imported_uses_closed(ctx, object, accesses, joins, seeds, seen)) return;
    // An unrelated ordinary integer array is not part of the selected plan.
    if (!used) {
      for (auto it = accesses.begin(); it != accesses.end();)
        if (it->second.storage == object.storage) it = accesses.erase(it); else ++it;
    } else if (!closed) return;
    else accounted_joins.insert(array_joins.begin(), array_joins.end());
  }
  if (accounted_creates != creates || accounted_joins != source_joins) return;
  ctx.native_handle_accesses = std::move(accesses);
  ctx.native_handle_joins = std::move(joins);
  ctx.native_handles_only = true;
  for (const auto& access : ctx.native_handle_accesses)
    if (access.second.shape == NativeHandleShape::Scalar) {
      log::debug("Native pthread handles: closed scalar storage inventory");
      break;
    }
}

template < typename E >
bool native_handle_range(E& eng, ar::Statement* statement, ar::Value* operand) {
  auto& ctx = eng.ctx();
  auto found = ctx.native_handle_accesses.find(statement);
  if (found == ctx.native_handle_accesses.end() || eng.data().is_normal_flow_bottom()) return false;
  const auto& ptr = ctx.lit_factory->get_scalar(operand);
  if (!ptr.is_pointer_var()) return false;
  const auto address = eng.data().normal().pointer_to_pointer(ptr.var());
  const auto& offset = address.offset();
  return eng.data().normal().nullity_is_non_null(ptr.var()) &&
      address.points_to().is_set() && address.points_to().size() == 1 &&
      *address.points_to().begin() == ctx.mem_factory->get_local(found->second.storage) &&
      !offset.is_bottom() && !offset.is_top() &&
      (found->second.shape != NativeHandleShape::Scalar ||
       (offset.singleton() && offset.lb().to_z_number() == 0)) &&
      offset.lb().to_z_number() >= 0 &&
      offset.ub().to_z_number() +
           ctx.bundle->data_layout().store_size_in_bytes(found->second.type) <= found->second.size;
}

template < typename E >
void observe_native_handle_range(E& eng, ar::Statement* statement, ar::Value* operand) {
  auto& ctx = eng.ctx();
  if (!ctx.native_handle_accesses.count(statement) || eng.data().is_normal_flow_bottom()) return;
  bool valid = native_handle_range(eng, statement, operand);
  ctx.native_handle_ranges.observe(statement, valid);
  if (!valid) ctx.concurrent_env.record_unmodeled_effect(statement, "native pthread handle storage range not covered");
}

} // namespace analyzer
} // namespace ikos
