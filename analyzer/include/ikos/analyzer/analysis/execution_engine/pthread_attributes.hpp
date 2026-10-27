// Copyright and license: see numerical.hpp.
#pragma once

#include <ikos/analyzer/analysis/context.hpp>
#include <ikos/analyzer/analysis/source_program_facts.hpp>
#include <ikos/analyzer/analysis/literal.hpp>
#include <ikos/analyzer/analysis/memory_location.hpp>
#include <ikos/analyzer/analysis/variable.hpp>

namespace ikos {
namespace analyzer {
using ClosedPthreadAttributeEffect = SourceProgramFacts::PthreadAttributeEffect;
using ClosedPthreadAttributeEffects = std::map< ar::CallBase*, ClosedPthreadAttributeEffect >;

// A bounded immutable proof, exposed separately for adversarial AR unit controls.
// A caller must supply the unique configured main after option reconciliation.
ClosedPthreadAttributeEffects build_closed_pthread_attribute_effects(
    ar::Bundle*, ar::Function*, SourceProgramFacts&, std::size_t work_limit = 200000);
const ClosedPthreadAttributeEffect* closed_pthread_attribute_effect(ar::CallBase*, Context&);

// This query is intentionally pure: never prepare_mem_access/assert/refine here.
// Exception-domain D is shared by the numerical engine and checker pre-state.
template < typename D >
bool closed_pthread_attribute_range(const ClosedPthreadAttributeEffect& effect,
                                   const ScalarLit& literal, const D& data,
                                   MemoryFactory& memory, VariableFactory& variables) {
  if (data.is_normal_flow_bottom()) return false;
  if (!literal.is_pointer_var()) return false;
  const auto& state = data.normal();
  if (!state.uninit_is_initialized(literal.var()) ||
      !state.nullity_is_non_null(literal.var())) return false;
  const auto pointer = state.pointer_to_pointer(literal.var());
  const auto offset = pointer.offset().singleton();
  auto* expected = memory.get_local(effect.storage);
  if (!pointer.points_to().is_set() || pointer.points_to().size() != 1 ||
      *pointer.points_to().begin() != expected || !offset || !offset->is_zero() ||
      effect.storage->alignment() < effect.alignment ||
      !state.lifetime_to_lifetime(expected).is_allocated()) return false;
  auto size = state.int_to_interval(variables.get_alloc_size(expected)).singleton();
  return size && size->to_z_number() == core::ZNumber(effect.bytes);
}
template < typename D >
bool closed_pthread_attribute_range(const ClosedPthreadAttributeEffect& effect,
                                   ar::CallBase* call, const D& data, Context& ctx) {
  return closed_pthread_attribute_range(effect, ctx.lit_factory->get_scalar(call->argument(0)),
                                       data, *ctx.mem_factory, *ctx.var_factory);
}
// Ordinary private byte write only; callers retain the existing nondet-return path.
template < typename D >
void havoc_closed_pthread_attribute_bytes(const ClosedPthreadAttributeEffect& effect,
                                         const ScalarLit& destination, D& data,
                                         MemoryFactory& memory,
                                         core::ConcurrentGlobalEnv& concurrent,
                                         unsigned pointer_bits) {
  const auto pointer = data.normal().pointer_to_pointer(destination.var());
  auto* location = memory.get_local(effect.storage);
  const auto address = core::IndexableTraits< MemoryLocation* >::index(location);
  const auto width = core::MachineInt(effect.bytes, pointer_bits, core::Unsigned);
  data.normal().mem_forget_reachable(destination.var(), width);
  concurrent.record_pointer_write(address, pointer.offset(), width.to_z_number(), false);
  if (concurrent.has_mutex_value_cell(address)) concurrent.invalidate_mutex_values();
}
} // namespace analyzer
} // namespace ikos
