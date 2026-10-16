// Copyright and license: see numerical.hpp.
#pragma once

#include <llvm/ADT/Triple.h>
#include <llvm/IR/IntrinsicInst.h>
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

inline llvm::Function* native_handle_api(llvm::CallBase* call, bool create) {
  auto* function = llvm::dyn_cast< llvm::Function >(call->getCalledOperand()->stripPointerCasts());
  const unsigned n = create ? 4 : 2;
  if (!function || !function->isDeclaration() || function->isVarArg() ||
      function->getName() != (create ? "pthread_create" : "pthread_join") ||
      function->arg_size() != n || call->arg_size() != n || !call->use_empty() ||
      function->getCallingConv() != llvm::CallingConv::C || call->getCallingConv() != llvm::CallingConv::C ||
      !function->getReturnType()->isIntegerTy(32) || !call->getType()->isIntegerTy(32) ||
      call->getFunctionType() != function->getFunctionType()) return nullptr;
  for (unsigned i = 0; i < n; ++i) {
    auto* type = function->getFunctionType()->getParamType(i);
    if (type != call->getArgOperand(i)->getType()) return nullptr;
    if (i == 0 && !create) { if (!type->isIntegerTy(32)) return nullptr; }
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
    if (!output->getPointerElementType()->isIntegerTy(32) ||
        !llvm::isa< llvm::ConstantPointerNull >(call->getArgOperand(3)) || !entry || entry->isDeclaration() ||
        entry->isVarArg() || entry->arg_size() != 1 || !entry->getReturnType()->isPointerTy() ||
        !entry->getFunctionType()->getParamType(0)->isPointerTy() || entry->getName() == "main" ||
        entry->getCallingConv() != llvm::CallingConv::C ||
        signature->getPointerElementType() != entry->getFunctionType() ||
        llvm::cast< llvm::PointerType >(entry->getReturnType())->getAddressSpace() != 0 ||
        llvm::cast< llvm::PointerType >(entry->getFunctionType()->getParamType(0))->getAddressSpace() != 0) return nullptr;
  } else {
    auto* output = llvm::cast< llvm::PointerType >(call->getArgOperand(1)->getType());
    if (!output->getPointerElementType()->isPointerTy()) return nullptr;
  }
  return function;
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

// This inventory selects a value abstraction, not an effect/termination proof.
// Every producer must use native TOP even if the later range preflight fails.
inline void configure_native_thread_handles(Context& ctx) {
  if (ctx.native_handle_inventory_checked) return;
  ctx.native_handle_inventory_checked = true;
  auto* bundle = ctx.bundle;
  if (!ctx.opts.enable_thread_modular || bundle->data_layout().pointers.bit_width != 32 ||
      ctx.opts.entry_points.size() != 1 || ctx.opts.entry_points.front()->name() != "main" ||
      ctx.opts.entry_points.front()->num_parameters() != 0 || !ctx.opts.no_init_globals.empty() ||
      bundle->global_or_null("ar.global_ctors") || bundle->global_or_null("ar.global_dtors") ||
      ctx.concurrent_env.has_synthetic_handle_slots()) return;
  auto* main = ctx.opts.entry_points.front();
  if (!main->has_frontend()) return;
  auto* source_main = main->frontend< llvm::Function >();
  if (!source_main->use_empty()) return;
  if (!mutex_effect_pointer_representations_closed(*source_main->getParent())) return;
  llvm::Triple target(source_main->getParent()->getTargetTriple());
  if (target.getArch() != llvm::Triple::x86 || !target.isOSLinux() ||
      target.getEnvironment() != llvm::Triple::GNU) return;
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
      if (statement->has_frontend() && (isa< ar::CallBase >(statement) || isa< ar::Load >(statement))) {
        auto* original = statement->frontend< llvm::Value >();
        if (!originals.emplace(original, statement).second) return;
      }
      if (auto* allocation = dyn_cast< ar::Allocate >(statement)) {
        auto* count = dyn_cast< ar::IntegerConstant >(allocation->array_size());
        auto* array = dyn_cast< ar::ArrayType >(allocation->allocated_type());
        auto* element = array ? dyn_cast< ar::IntegerType >(array->element_type()) : nullptr;
        auto* source = allocation->has_frontend()
            ? llvm::dyn_cast< llvm::AllocaInst >(allocation->frontend< llvm::Value >()) : nullptr;
        auto* original = source ? llvm::dyn_cast< llvm::ArrayType >(source->getAllocatedType()) : nullptr;
        auto* repetitions = source ? llvm::dyn_cast< llvm::ConstantInt >(source->getArraySize()) : nullptr;
        if (allocation->code() != main->body() || allocation->parent() != main->body()->entry_block() ||
            !count || count->value().to_z_number() != 1 || !element || element->bit_width() != 32 ||
            array->num_elements() <= 0 || !source || source->getAddressSpace() != 0 ||
            source->getParent() != &source_main->getEntryBlock() || !repetitions || !repetitions->isOne() ||
            !original || !original->getElementType()->isIntegerTy(32) ||
            array->num_elements() != core::ZNumber(original->getNumElements())) continue;
        const auto bytes = bundle->data_layout().alloc_size_in_bytes(array);
        const auto& layout = source_main->getParent()->getDataLayout();
        if (bytes != 4 * array->num_elements() || bytes >= (core::ZNumber(1) << 32) ||
            bytes != core::ZNumber(layout.getTypeAllocSize(original).getFixedValue()) ||
            source->getAlign().value() < 4) continue;
        arrays.emplace(source, NativeHandleArray{allocation->result(), element, bytes});
      }
      auto* call = dyn_cast< ar::CallBase >(statement);
      if (!call) continue;
      auto* called = native_handle_callee(call);
      auto* original = call->has_frontend() ? llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >()) : nullptr;
      auto* callee = original ? llvm::dyn_cast< llvm::Function >(original->getCalledOperand()->stripPointerCasts()) : nullptr;
      if (!called || !callee) return;
      const bool pthread_api = callee->getName() == "pthread_create" || callee->getName() == "pthread_join";
      if (pthread_api) {
        auto* result = dyn_cast< ar::IntegerType >(called->type()->return_type());
        if (!result || result->bit_width() != 32 || called->type()->is_var_arg() ||
            called->num_parameters() != call->num_arguments()) return;
        for (unsigned i = 0; i < call->num_arguments(); ++i)
          if (called->type()->param_type(i) != call->argument(i)->type()) return;
      }
      if (callee->getName() == "pthread_create" || called->intrinsic_id() == ar::Intrinsic::PthreadCreate) {
        if (!native_handle_api(original, true) || !called->is_declaration() ||
            called->name() != "pthread_create" || !native_repeated_create(call) ||
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
        if (!type || type->bit_width() != 32) return;
        creates.insert(original);
      } else if (callee->getName() == "pthread_join" || called->intrinsic_id() == ar::Intrinsic::PthreadJoin) {
        if (!native_handle_api(original, false) || !called->is_declaration() ||
            call->code() != main->body() || call->num_arguments() != 2 ||
            !isa< ar::NullConstant >(call->argument(1))) return;
        auto* type = dyn_cast< ar::IntegerType >(call->argument(0)->type());
        if (!type || type->bit_width() != 32 ||
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
    std::set< llvm::CallBase* > array_joins;
    while (!pending.empty() && closed) {
      auto* value = pending.back(); pending.pop_back();
      if (!seen.insert(value).second) continue;
      if (seen.size() > 2048) return;
      for (auto* user : value->users()) {
        if (auto* gep = llvm::dyn_cast< llvm::GEPOperator >(user)) {
          if (gep->getPointerOperand() != value) { closed = false; break; }
          pending.push_back(user);
        } else if (llvm::isa< llvm::BitCastOperator >(user)) pending.push_back(user);
        else if (llvm::isa< llvm::DbgInfoIntrinsic >(user)) continue;
        else if (auto* call = llvm::dyn_cast< llvm::CallBase >(user)) {
          if (!creates.count(call) || call->getArgOperand(0) != value || !native_handle_element(value, storage)) {
            closed = false; break;
          }
          auto* imported = dyn_cast_or_null< ar::CallBase >(originals[call]);
          if (!imported || local_output_base(imported->argument(0)) != object.storage) { closed = false; break; }
          accesses.emplace(imported, object); accounted_creates.insert(call); used = true;
        } else if (auto* load = llvm::dyn_cast< llvm::LoadInst >(user)) {
          if (load->getPointerOperand() != value || !load->getType()->isIntegerTy(32) ||
              load->isVolatile() || load->isAtomic() || !native_handle_element(value, storage)) { closed = false; break; }
          auto* imported = dyn_cast_or_null< ar::Load >(originals[load]);
          auto* type = imported ? dyn_cast< ar::IntegerType >(imported->result()->type()) : nullptr;
          auto* pointer = imported ? dyn_cast< ar::PointerType >(imported->operand()->type()) : nullptr;
          auto* leaf = pointer ? dyn_cast< ar::IntegerType >(pointer->pointee()) : nullptr;
          if (!imported || !type || type->bit_width() != 32 || !leaf || leaf->bit_width() != 32 ||
              imported->is_volatile() || imported->is_atomic() ||
              local_output_base(imported->operand()) != object.storage) { closed = false; break; }
          for (auto* use : load->users()) {
            auto* join = llvm::dyn_cast< llvm::CallBase >(use);
            if (!join || !source_joins.count(join) || join->getArgOperand(0) != load) { closed = false; break; }
            array_joins.insert(join);
          }
          accesses.emplace(imported, object);
        } else { closed = false; break; }
      }
    }
    if (!closed && used) return;
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
      !offset.is_bottom() && !offset.is_top() && offset.lb().to_z_number() >= 0 &&
      offset.ub().to_z_number() + 4 <= found->second.size;
}

template < typename E >
void observe_native_handle_range(E& eng, ar::Statement* statement, ar::Value* operand) {
  auto& ctx = eng.ctx();
  if (!ctx.native_handle_accesses.count(statement) || eng.data().is_normal_flow_bottom()) return;
  bool valid = native_handle_range(eng, statement, operand);
  auto added = ctx.native_handle_observations.emplace(statement, valid);
  if (!added.second) added.first->second &= valid;
  if (!valid) ctx.concurrent_env.record_unmodeled_effect(statement, "native pthread handle storage range not covered");
}

} // namespace analyzer
} // namespace ikos
