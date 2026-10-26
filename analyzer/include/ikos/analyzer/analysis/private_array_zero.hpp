#pragma once

#include <unordered_set>
#include <vector>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <ikos/ar/semantic/code.hpp>
#include <ikos/ar/semantic/statement.hpp>
#include <ikos/analyzer/support/cast.hpp>
#include <ikos/analyzer/util/log.hpp>

namespace ikos { namespace analyzer {

// Structural closure only: each consumer must model the actual memory effect
// and establish its own executed allocation/address/lifetime premises.
inline bool private_array_zero_structure(ar::CallBase* call,
                                         ar::LocalVariable* local,
                                         bool unused_entry) {
  auto reject = [](unsigned line) {
    if (log::is_enabled_for(LogLevel::Debug))
      log::msg() << "[Private array zero] refused at concurrent_semantics.hpp:" << line << "\n";
    return false;
  };
  auto* source = call->has_frontend()
      ? llvm::dyn_cast< llvm::MemSetInst >(call->frontend< llvm::Value >()) : nullptr;
  auto* target = dyn_cast< ar::FunctionPointerConstant >(call->called());
  auto* owner = local ? local->function() : nullptr;
  unsigned budget = 32768;
  ar::Allocate* allocation = nullptr;
  if (owner) for (auto* block : *owner->body()) for (auto* statement : *block) {
    if (budget-- == 0) return reject(__LINE__);
    if (statement->result_or_null() != local) continue;
    if (allocation) return reject(__LINE__);
    allocation = dyn_cast< ar::Allocate >(statement);
  }
  auto* storage = allocation && allocation->has_frontend()
      ? llvm::dyn_cast< llvm::AllocaInst >(allocation->frontend< llvm::Value >()) : nullptr;
  auto* type = local ? dyn_cast< ar::ArrayType >(local->type()->pointee()) : nullptr;
  auto* element = type ? dyn_cast< ar::IntegerType >(type->element_type()) : nullptr;
  auto* original_type = storage ? llvm::dyn_cast< llvm::ArrayType >(storage->getAllocatedType()) : nullptr;
  auto* count = storage ? llvm::dyn_cast< llvm::ConstantInt >(storage->getArraySize()) : nullptr;
  auto* byte = source ? llvm::dyn_cast< llvm::ConstantInt >(source->getValue()) : nullptr;
  auto* length = source ? llvm::dyn_cast< llvm::ConstantInt >(source->getLength()) : nullptr;
  if (!source || !isa< ar::MemorySet >(call) || call->num_arguments() != 5 ||
      !target || target->function()->intrinsic_id() != ar::Intrinsic::MemorySet ||
      !source->getModule()->getModuleInlineAsm().empty() || source->hasOperandBundles() ||
      source->isVolatile() || !byte || !byte->isZero() || !length ||
      !owner || call->code() != owner->body() || !owner->has_frontend() ||
      owner->frontend< llvm::Value >() != source->getFunction() ||
      !storage || storage->getFunction() != source->getFunction() ||
      storage->getParent() != &source->getFunction()->getEntryBlock() ||
      storage->getAddressSpace() != 0 || !count || !count->isOne() ||
      source->getDest()->stripPointerCasts() != storage ||
      !type || !element || !original_type || type->num_elements() <= 0 ||
      !original_type->getElementType()->isIntegerTy(element->bit_width()) ||
      type->num_elements() != core::ZNumber(original_type->getNumElements()) ||
      element->bit_width() % 8 != 0) return reject(__LINE__);
  const auto extent = call->bundle()->data_layout().alloc_size_in_bytes(type);
  const auto& layout = source->getModule()->getDataLayout();
  const auto bits = call->bundle()->data_layout().pointers.bit_width;
  if ((bits != 32 && bits != 64) || layout.getPointerSizeInBits() != bits ||
      layout.getIndexSizeInBits(0) != bits ||
      extent != call->bundle()->data_layout().store_size_in_bytes(type) ||
      extent != type->num_elements() * (element->bit_width() / 8) ||
      extent != core::ZNumber(layout.getTypeAllocSize(original_type).getFixedSize()) ||
      extent != core::ZNumber(layout.getTypeStoreSize(original_type).getFixedSize()) ||
      length->getValue().getActiveBits() > bits || extent != core::ZNumber(length->getZExtValue()) ||
      local->alignment() != storage->getAlign().value()) return reject(__LINE__);
  auto constant = [&](unsigned index, const core::ZNumber& expected) {
    auto* value = dyn_cast< ar::IntegerConstant >(call->argument(index));
    return value && value->value().to_z_number() == expected;
  };
  if (call->has_result() || !constant(1, core::ZNumber(0)) || !constant(2, extent) ||
      !constant(3, core::ZNumber(source->getDestAlign().valueOrOne().value())) ||
      !constant(4, core::ZNumber(0))) return reject(__LINE__);
  auto* api = target->function();
  if (!api->is_declaration() || api->type()->is_var_arg() || api->num_parameters() != 5 ||
      !api->type()->return_type()->is_void()) return reject(__LINE__);
  for (unsigned i = 0; i < 5; ++i)
    if (call->argument(i)->type() != api->type()->param_type(i)) return reject(__LINE__);
  // Exclude direct/indirect reentry: the owner is main or a root created by main.
  // Separate workers may use this allocation site without sharing their stacks.
  for (auto* user : source->getFunction()->users()) {
    if (budget-- == 0) return reject(__LINE__);
    auto* create = llvm::dyn_cast< llvm::CallInst >(user);
    auto* api = create ? create->getCalledFunction() : nullptr;
    if (!api || !api->isDeclaration() || api->getName() != "pthread_create" ||
        create->arg_size() != 4 || create->getArgOperand(2) != source->getFunction() ||
        create->getFunction()->getName() != "main" || create->hasOperandBundles()) return reject(__LINE__);
  }
  if (source->getFunction()->use_empty() &&
      !unused_entry)
    return reject(__LINE__);
  for (auto it = call->bundle()->function_begin(); it != call->bundle()->function_end(); ++it) {
    if (!(*it)->is_definition()) continue;
    for (auto* block : *(*it)->body()) for (auto* statement : *block) {
      if (budget-- == 0) return reject(__LINE__);
      for (auto op = statement->op_begin(); op != statement->op_end(); ++op) {
        auto* pointer = dyn_cast< ar::FunctionPointerConstant >(*op);
        if (!pointer || pointer->function() != owner) continue;
        auto* create = dyn_cast< ar::Call >(statement);
        auto* api = create ? dyn_cast< ar::FunctionPointerConstant >(create->called()) : nullptr;
        auto* original = create && create->has_frontend()
            ? llvm::dyn_cast< llvm::CallInst >(create->template frontend< llvm::Value >()) : nullptr;
        if (!create || !api || !api->function()->is_declaration() ||
            (api->function()->intrinsic_id() != ar::Intrinsic::PthreadCreate &&
             api->function()->name() != "pthread_create") ||
            create->num_arguments() != 4 || create->argument(2) != *op ||
            (*it)->name() != "main" || !original || original->arg_size() != 4 ||
            original->getArgOperand(2) != source->getFunction()) return reject(__LINE__);
      }
    }
  }
  std::vector< llvm::Value* > pending{storage};
  std::unordered_set< llvm::Value* > seen;
  std::unordered_set< llvm::Value* > consumers;
  while (!pending.empty()) {
    auto* value = pending.back(); pending.pop_back();
    if (!seen.insert(value).second) continue;
    for (auto* user : value->users()) {
      if (budget-- == 0) return reject(__LINE__);
      if (user == source || llvm::isa< llvm::DbgInfoIntrinsic >(user)) continue;
      consumers.insert(user);
      if (auto* cast = llvm::dyn_cast< llvm::BitCastInst >(user)) {
        if (!cast->getType()->isPointerTy() || cast->getType()->getPointerAddressSpace() != 0) return reject(__LINE__);
        pending.push_back(cast);
      } else if (auto* shift = llvm::dyn_cast< llvm::GetElementPtrInst >(user)) {
        if (shift->getPointerOperand() != value || shift->getSourceElementType() != original_type ||
            shift->getNumIndices() != 2 ||
            !llvm::isa< llvm::ConstantInt >(shift->getOperand(1)) ||
            !llvm::cast< llvm::ConstantInt >(shift->getOperand(1))->isZero()) return reject(__LINE__);
        pending.push_back(shift);
      } else if (auto* load = llvm::dyn_cast< llvm::LoadInst >(user)) {
        if (load->getPointerOperand() != value || load->isAtomic() || load->isVolatile() ||
            !load->getType()->isIntegerTy(element->bit_width())) return reject(__LINE__);
      } else if (auto* store = llvm::dyn_cast< llvm::StoreInst >(user)) {
        if (store->getPointerOperand() != value || store->isAtomic() || store->isVolatile() ||
            !store->getValueOperand()->getType()->isIntegerTy(element->bit_width())) return reject(__LINE__);
      } else return reject(__LINE__);
    }
  }
  auto* repetitions = allocation ? dyn_cast< ar::IntegerConstant >(allocation->array_size()) : nullptr;
  if (!allocation || !repetitions || repetitions->value().to_z_number() != 1 ||
      allocation->allocated_type() != type || allocation->parent() != owner->body()->entry_block() ||
      !allocation->has_frontend() || allocation->frontend< llvm::Value >() != storage) return reject(__LINE__);
  std::vector< ar::Value* > imported_pending{local};
  std::unordered_set< ar::Value* > imported_seen;
  while (!imported_pending.empty()) {
    auto* value = imported_pending.back(); imported_pending.pop_back();
    if (!imported_seen.insert(value).second) continue;
    for (auto* block : *owner->body()) for (auto* statement : *block) {
      if (budget-- == 0) return reject(__LINE__);
      bool used = false;
      for (auto it = statement->op_begin(); it != statement->op_end(); ++it) used |= *it == value;
      if (!used || statement == call) continue;
      auto* original = statement->has_frontend() ? statement->frontend< llvm::Value >() : nullptr;
      if (!original || !consumers.count(original)) return reject(__LINE__);
      if (auto* cast = dyn_cast< ar::UnaryOperation >(statement)) {
        if (cast->op() != ar::UnaryOperation::Bitcast || !cast->result()->type()->is_pointer() ||
            !llvm::isa< llvm::BitCastInst >(original)) return reject(__LINE__);
        imported_pending.push_back(cast->result());
      } else if (auto* shift = dyn_cast< ar::PointerShift >(statement)) {
        auto* gep = llvm::dyn_cast< llvm::GetElementPtrInst >(original);
        auto zero = shift->num_terms() == 2
            ? dyn_cast< ar::IntegerConstant >(shift->term(0).second) : nullptr;
        if (shift->pointer() != value || !gep || !zero || !zero->value().is_zero() ||
            shift->term(0).first.to_z_number() != extent ||
            shift->term(1).first.to_z_number() != core::ZNumber(element->bit_width() / 8)) return reject(__LINE__);
        imported_pending.push_back(shift->result());
      } else if (auto* load = dyn_cast< ar::Load >(statement)) {
        auto* read = llvm::dyn_cast< llvm::LoadInst >(original);
        if (load->operand() != value || load->result()->type() != element ||
            load->is_atomic() || load->is_volatile() || !read ||
            load->alignment() != read->getAlign().value()) return reject(__LINE__);
      } else if (auto* store = dyn_cast< ar::Store >(statement)) {
        auto* write = llvm::dyn_cast< llvm::StoreInst >(original);
        if (store->pointer() != value || store->value()->type() != element ||
            store->is_atomic() || store->is_volatile() || !write ||
            store->alignment() != write->getAlign().value()) return reject(__LINE__);
      } else return reject(__LINE__);
    }
  }
  return imported_seen.count(call->argument(0)) != 0;
}

} }
