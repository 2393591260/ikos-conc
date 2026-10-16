// Copyright and license: see numerical.hpp.
#pragma once

#include <ikos/analyzer/analysis/execution_engine/stable_publication.hpp>
#include <ikos/analyzer/analysis/execution_engine/heap_initialization_prefix.hpp>
#include <ikos/analyzer/analysis/literal.hpp>
#include <ikos/analyzer/util/log.hpp>

namespace ikos {
namespace analyzer {

// Every returned instance has the slot initialized before its first escape.
// This proves coverage of values, never uniqueness of summarized allocations.
inline bool prove_heap_constructor_body(ar::CallBase* allocation, const core::ZNumber& field) {
  auto* code = allocation->code();
  if (!code->is_function_body() || field < 0) return false;
  auto* block = code->entry_block();
  for (auto* other : *code) if (other != block) return false;
  if (block->successor_begin() != block->successor_end()) return false;
  auto* target = dyn_cast< ar::FunctionPointerConstant >(allocation->called());
  if (!target || target->function()->intrinsic_id() != ar::Intrinsic::LibcMalloc ||
      allocation->num_arguments() != 1) return false;
  auto* count = dyn_cast< ar::IntegerConstant >(allocation->argument(0));
  if (!count || count->value().to_z_number() <= 0) return false;
  const auto extent = count->value().to_z_number();
  const auto& layout = code->function()->bundle()->data_layout();
  const core::ZNumber width(layout.pointers.bit_width / 8);
  std::unordered_map< ar::Value*, core::ZNumber > addresses;
  std::unordered_set< ar::Variable* > definitions;
  auto* function = code->function();
  for (auto it = function->param_begin(); it != function->param_end(); ++it)
    definitions.insert(*it);
  bool initialized = false, allocated = false, returned = false;
  for (auto* statement : *block) {
    if (returned) return false;
    if (auto* result = statement->result_or_null())
      if (!definitions.insert(result).second) return false;
    if (statement == allocation) {
      allocated = true;
      addresses.emplace(allocation->result(), core::ZNumber(0));
    } else if (auto* call = dyn_cast< ar::CallBase >(statement)) {
      auto* callee = dyn_cast< ar::FunctionPointerConstant >(call->called());
      if (!callee || callee->function()->intrinsic_id() != ar::Intrinsic::IkosNonDet ||
          call->num_arguments() != 0 || !call->has_result() ||
          !call->result()->type()->is_integer()) return false;
    } else if (auto* copy = dyn_cast< ar::Assignment >(statement)) {
      auto it = addresses.find(copy->operand());
      if (it == addresses.end()) return false;
      addresses.emplace(copy->result(), it->second);
    } else if (auto* cast = dyn_cast< ar::UnaryOperation >(statement)) {
      // Integer payload conversions cannot expose or dereference the fresh base.
      if (cast->operand()->type()->is_integer() && cast->result()->type()->is_integer())
        continue;
      auto it = addresses.find(cast->operand());
      if (cast->op() != ar::UnaryOperation::Bitcast || it == addresses.end() ||
          !cast->result()->type()->is_pointer()) return false;
      addresses.emplace(cast->result(), it->second);
    } else if (auto* shift = dyn_cast< ar::PointerShift >(statement)) {
      auto it = addresses.find(shift->pointer());
      if (it == addresses.end()) return false;
      auto offset = it->second;
      for (auto term = shift->term_begin(); term != shift->term_end(); ++term) {
        auto* index = dyn_cast< ar::IntegerConstant >((*term).second);
        if (!index) return false;
        offset += (*term).first.to_z_number() * index->value().to_z_number();
      }
      if (offset < 0 || offset > extent) return false;
      addresses.emplace(shift->result(), offset);
    } else if (auto* store = dyn_cast< ar::Store >(statement)) {
      auto it = addresses.find(store->pointer());
      if (it == addresses.end() || store->is_volatile() || store->is_atomic()) return false;
      const auto size = layout.store_size_in_bytes(store->value()->type());
      if (it->second < 0 || it->second + size > extent) return false;
      // No pointer escape, self-link, or foreign store is admitted in this subset.
      if (store->value()->type()->is_pointer()) {
        if (!isa< ar::NullConstant >(store->value()) || size != width) return false;
        if (it->second == field) initialized = true;
      } else if (!store->value()->type()->is_integer() &&
                 !store->value()->type()->is_float()) return false;
    } else if (auto* ret = dyn_cast< ar::ReturnValue >(statement)) {
      if (!ret->has_operand()) return false;
      auto it = addresses.find(ret->operand());
      if (it == addresses.end() || it->second != 0) return false;
      returned = true;
    } else return false;
  }
  if (!allocated || !initialized || !returned) return false;
  return true;
}

inline bool prove_heap_constructor(ar::CallBase* allocation, const core::ZNumber& field,
                                   const Context& ctx) {
  const bool constructor = prove_heap_constructor_body(allocation, field);
  if (!constructor && !prove_heap_initialization_prefix(allocation, field, ctx)) return false;
  auto* function = allocation->code()->function();
  auto* bundle = function->bundle();
  if (bundle->global_or_null("ar.global_ctors")) return false;
  unsigned budget = 8192;
  auto covered = [&](ar::Code* body) {
    if (!body) return true;
    for (auto* bb : *body) for (auto* statement : *bb) {
      if (budget-- == 0) return false;
      if (auto* call = dyn_cast< ar::CallBase >(statement))
        {
          auto* target = dyn_cast< ar::FunctionPointerConstant >(call->called());
          auto* source = call->has_frontend() ? llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >()) : nullptr;
          auto* original = source ? llvm::dyn_cast< llvm::Function >(source->getCalledOperand()->stripPointerCasts()) : nullptr;
          if (!constructor && target && (target->function()->intrinsic_id() == ar::Intrinsic::LibcFree ||
                                         target->function()->intrinsic_id() == ar::Intrinsic::LibcRealloc)) return false;
          if (!constructor && original && (original->getName() == "free" || original->getName() == "realloc")) return false;
          if (!heap_pointer_call_covered(call, &ctx) &&
              !ctx.mutex_effect_certificates.count(call)) return false;
        }
    }
    return true;
  };
  for (auto it = bundle->function_begin(); it != bundle->function_end(); ++it)
    if (!covered((*it)->body_or_null())) return false;
  for (auto it = bundle->global_begin(); it != bundle->global_end(); ++it)
    if (!covered((*it)->initializer_or_null())) return false;
  if (!constructor) log::debug("Heap field initialization: closed allocation prefix");
  return true;
}

template < typename E >
bool initialized_heap_field_value(E& eng, ar::Load* load, const ScalarLit& pointer,
                                   core::ConcurrentGlobalEnv::PointerValue& value) {
  if (load->is_atomic() || !eng.ctx().opts.no_init_globals.empty()) return false;
  for (auto* root : eng.ctx().opts.entry_points) if (root->name() != "main") return false;
  auto address = eng.data().normal().pointer_to_pointer(pointer.var());
  const auto& sources = address.points_to();
  auto offset = address.offset().singleton();
  if (!sources.is_set() || sources.size() == 0 || !offset) return false;
  auto& env = eng.ctx().concurrent_env;
  auto merged = core::ConcurrentGlobalEnv::PointerValue::bottom(
      value.offset().bit_width(), value.offset().sign());
  for (auto* source : sources) {
    auto* heap = dyn_cast< DynAllocMemoryLocation >(source);
    if (!heap) return false;
    bool certified = false;
    if (!env.get_heap_constructor_certificate(heap->call(), offset->to_z_number(), certified)) {
      certified = prove_heap_constructor(heap->call(), offset->to_z_number(), eng.ctx());
      env.cache_heap_constructor_certificate(heap->call(), offset->to_z_number(), certified);
    }
    auto incoming = core::ConcurrentGlobalEnv::PointerValue::top(
        value.offset().bit_width(), value.offset().sign());
    if (!certified || !env.get_heap_pointer(
        {core::IndexableTraits< MemoryLocation* >::index(source), true, offset->to_z_number()},
        incoming, true)) return false;
    merged.join_with(incoming);
  }
  value = merged;
  return true;
}

} // namespace analyzer
} // namespace ikos
