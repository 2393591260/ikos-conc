// Copyright and license: see numerical.hpp.
#pragma once

#include <algorithm>
#include <functional>
#include <vector>

#include <ikos/analyzer/analysis/execution_engine/handle_origin.hpp>
#include <ikos/analyzer/analysis/literal.hpp>
#include <ikos/analyzer/analysis/memory_location.hpp>

namespace ikos {
namespace analyzer {

// A defined direct output cannot leave the allocation of its syntactic local
// base. Keep this narrow: loads, formals and integer/address conversions fail.
inline ar::LocalVariable* local_output_base(ar::Value* value) {
  for (unsigned depth = 0; depth != 32; ++depth) {
    if (!isa< ar::PointerType >(value->type())) return nullptr;
    if (auto* local = dyn_cast< ar::LocalVariable >(value)) return local;
    auto* variable = dyn_cast< ar::InternalVariable >(value);
    if (!variable) return nullptr;
    if (variable->code()->is_function_body()) {
      auto* function = variable->code()->function();
      for (auto it = function->param_begin(); it != function->param_end(); ++it)
        if (*it == variable) return nullptr;
    }
    auto* definition = unique_def(variable);
    if (auto* copy = dyn_cast_or_null< ar::Assignment >(definition)) {
      value = copy->operand();
    } else if (auto* cast = dyn_cast_or_null< ar::UnaryOperation >(definition)) {
      if (cast->op() != ar::UnaryOperation::Bitcast) return nullptr;
      value = cast->operand();
    } else if (auto* shift = dyn_cast_or_null< ar::PointerShift >(definition)) {
      value = shift->pointer();
    } else return nullptr;
  }
  return nullptr;
}

// Covered library outputs write their direct destination. Unknown transitive
// effects/callbacks are separately gated; this is not an escape-analysis proof.
template < typename E >
bool uncovered_mutex_value_write(E& eng, ar::Value* source, const ScalarLit& pointer) {
  if (!pointer.is_pointer_var() || eng.data().normal().nullity_is_null(pointer.var()))
    return false;
  auto points = eng.data().normal().pointer_to_points_to(pointer.var());
  if (points.is_bottom()) return false;
  if (local_output_base(source)) return false;
  if (!points.is_set()) return true;
  for (auto* location : points) {
    if (isa< AbsoluteZeroMemoryLocation >(location) ||
        eng.ctx().concurrent_env.has_mutex_value_cell(
            core::IndexableTraits< MemoryLocation* >::index(location))) return true;
  }
  return false;
}

// This subset proves a complete access envelope before filtering interference.
// Escaped addresses, byte accesses and indirect helper writes use the old board.
inline bool protected_scalar_accesses(ar::GlobalVariable* slot) {
  auto* type = dyn_cast< ar::IntegerType >(slot->type()->pointee());
  if (!type || !slot->is_definition() || slot->is_thread_local()) return false;
  unsigned budget = 8192;
  auto scan = [&](ar::Code* code) {
    if (!code) return true;
    for (auto* block : *code) for (auto* statement : *block) {
      if (budget-- == 0) return false;
      if (code->is_function_body()) {
        auto* function = code->function();
        for (auto it = function->param_begin(); it != function->param_end(); ++it)
          if (statement->result_or_null() == *it) return false;
      }
      bool mentioned = false;
      for (auto it = statement->op_begin(); it != statement->op_end(); ++it)
        mentioned |= mentions_handle_slot(*it, slot);
      if (!mentioned) continue;
      if (auto* load = dyn_cast< ar::Load >(statement)) {
        auto* read = dyn_cast< ar::IntegerType >(load->result()->type());
        if (read && read->bit_width() == type->bit_width() &&
            exact_handle_slot(load->operand(), slot) && !load->is_volatile() &&
            !load->is_atomic()) continue;
      } else if (auto* store = dyn_cast< ar::Store >(statement)) {
        auto* written = dyn_cast< ar::IntegerType >(store->value()->type());
        if (written && written->bit_width() == type->bit_width() &&
            exact_handle_slot(store->pointer(), slot) && !store->is_volatile() &&
            !store->is_atomic()) continue;
      } else if (auto* copy = dyn_cast< ar::Assignment >(statement)) {
        if (unique_def(copy->result()) == copy &&
            exact_handle_slot(copy->result(), slot)) continue;
      } else if (auto* cast = dyn_cast< ar::UnaryOperation >(statement)) {
        if (cast->op() == ar::UnaryOperation::Bitcast &&
            unique_def(cast->result()) == cast &&
            exact_handle_slot(cast->result(), slot)) continue;
      } else if (auto* shift = dyn_cast< ar::PointerShift >(statement)) {
        if (unique_def(shift->result()) == shift &&
            exact_handle_slot(shift->result(), slot)) continue;
      }
      return false;
    }
    return true;
  };
  for (auto it = slot->bundle()->function_begin(); it != slot->bundle()->function_end(); ++it)
    if (!scan((*it)->body_or_null())) return false;
  for (auto it = slot->bundle()->global_begin(); it != slot->bundle()->global_end(); ++it)
    if (!scan((*it)->initializer_or_null())) return false;
  return true;
}

struct ProtectedIntegerCell {
  std::uint64_t offset;
  ar::IntegerType* type;
};

// Only finite, nonoverlapping integer leaves; padding never becomes a cell.
inline std::vector< ProtectedIntegerCell > protected_integer_cells(ar::GlobalVariable* global) {
  std::vector< ProtectedIntegerCell > cells;
  if (!global->is_definition() || global->is_thread_local()) return cells;
  const auto& dl = global->bundle()->data_layout();
  auto extent = dl.alloc_size_in_bytes(global->type()->pointee());
  if (extent <= 0 || !extent.fits< std::uint64_t >() ||
      extent >= (core::ZNumber(1) << dl.pointers.bit_width)) return cells;
  std::function< bool(ar::Type*, core::ZNumber, unsigned) > visit =
      [&](ar::Type* type, core::ZNumber offset, unsigned depth) {
    if (depth > 8 || cells.size() >= 64) return false;
    if (auto* integer = dyn_cast< ar::IntegerType >(type)) {
      auto bytes = dl.store_size_in_bytes(type);
      if (offset < 0 || offset + bytes > extent ||
          !offset.fits< std::uint64_t >()) return false;
      cells.push_back({offset.to< std::uint64_t >(), integer});
      return true;
    }
    if (auto* structure = dyn_cast< ar::StructType >(type)) {
      for (const auto& field : structure->layout())
        if (!visit(field.type, offset + field.offset, depth + 1)) return false;
      return true;
    }
    if (auto* array = dyn_cast< ar::ArrayType >(type)) {
      if (array->num_elements() > 64) return false;
      auto stride = dl.alloc_size_in_bytes(array->element_type());
      for (core::ZNumber i(0); i < array->num_elements(); ++i)
        if (!visit(array->element_type(), offset + i * stride, depth + 1)) return false;
      return true;
    }
    return false;
  };
  if (!visit(global->type()->pointee(), core::ZNumber(0), 0)) return {};
  std::sort(cells.begin(), cells.end(), [](const auto& a, const auto& b) {
    return a.offset < b.offset;
  });
  core::ZNumber end(0);
  for (const auto& cell : cells) {
    if (core::ZNumber(cell.offset) < end) return {};
    end = core::ZNumber(cell.offset) + dl.store_size_in_bytes(cell.type);
  }
  return cells;
}

// Match AR pointer arithmetic, including its machine-width offset arithmetic.
inline bool protected_cell_offset(ar::Value* value, ar::GlobalVariable* base,
                                   core::ZNumber& offset, unsigned depth = 0) {
  if (depth >= 64 || !value->type()->is_pointer()) return false;
  if (value == base) { offset = 0; return true; }
  auto* variable = dyn_cast< ar::InternalVariable >(value);
  if (!variable) return false;
  auto* definition = unique_def(variable);
  if (auto* copy = dyn_cast_or_null< ar::Assignment >(definition))
    return protected_cell_offset(copy->operand(), base, offset, depth + 1);
  if (auto* cast = dyn_cast_or_null< ar::UnaryOperation >(definition)) {
    return cast->op() == ar::UnaryOperation::Bitcast &&
           protected_cell_offset(cast->operand(), base, offset, depth + 1);
  }
  auto* shift = dyn_cast_or_null< ar::PointerShift >(definition);
  if (!shift || !protected_cell_offset(shift->pointer(), base, offset, depth + 1)) return false;
  auto bits = base->bundle()->data_layout().pointers.bit_width;
  for (auto it = shift->term_begin(); it != shift->term_end(); ++it) {
    auto* constant = dyn_cast< ar::IntegerConstant >((*it).second);
    if (!constant) return false;
    offset += (*it).first.to_z_number() *
              constant->value().cast(bits, core::Unsigned).to_z_number();
  }
  offset %= (core::ZNumber(1) << bits);
  return offset >= 0 &&
         offset <= base->bundle()->data_layout().alloc_size_in_bytes(base->type()->pointee());
}

inline bool protected_integer_accesses(ar::GlobalVariable* global,
                                        const std::vector< ProtectedIntegerCell >& cells) {
  if (global->type()->pointee()->is_integer()) return protected_scalar_accesses(global);
  if (cells.empty()) return false;
  auto complete = [&](ar::Value* pointer, ar::Type* type) {
    auto* integer = dyn_cast< ar::IntegerType >(type);
    core::ZNumber offset(0);
    if (!integer || !protected_cell_offset(pointer, global, offset)) return false;
    for (const auto& cell : cells)
      if (offset == core::ZNumber(cell.offset) &&
          integer->bit_width() == cell.type->bit_width()) return true;
    return false;
  };
  unsigned budget = 8192;
  auto scan = [&](ar::Code* code) {
    if (!code) return true;
    for (auto* block : *code) for (auto* statement : *block) {
      if (budget-- == 0) return false;
      if (code->is_function_body()) {
        auto* function = code->function();
        for (auto it = function->param_begin(); it != function->param_end(); ++it)
          if (statement->result_or_null() == *it) return false;
      }
      bool mentioned = false;
      for (auto it = statement->op_begin(); it != statement->op_end(); ++it)
        mentioned |= mentions_handle_slot(*it, global);
      if (!mentioned) continue;
      if (auto* load = dyn_cast< ar::Load >(statement)) {
        if (!load->is_volatile() && !load->is_atomic() &&
            complete(load->operand(), load->result()->type())) continue;
      } else if (auto* store = dyn_cast< ar::Store >(statement)) {
        if (!store->is_volatile() && !store->is_atomic()) {
          if (complete(store->pointer(), store->value()->type())) continue;
          // Static aggregate initialization is captured before worker analysis.
          if (!code->is_function_body() && store->pointer() == global &&
              store->value()->type() == global->type()->pointee()) continue;
        }
      } else if (isa< ar::Assignment >(statement) ||
                 isa< ar::UnaryOperation >(statement) ||
                 isa< ar::PointerShift >(statement)) {
        core::ZNumber offset(0);
        auto* result = statement->result_or_null();
        if (result && unique_def(cast< ar::InternalVariable >(result)) == statement &&
            protected_cell_offset(result, global, offset)) continue;
      }
      return false;
    }
    return true;
  };
  for (auto it = global->bundle()->function_begin(); it != global->bundle()->function_end(); ++it)
    if (!scan((*it)->body_or_null())) return false;
  for (auto it = global->bundle()->global_begin(); it != global->bundle()->global_end(); ++it)
    if (!scan((*it)->initializer_or_null())) return false;
  return true;
}

template < typename E >
std::unordered_set< std::uint64_t > value_mutexes(E& eng) {
  auto held = eng.lockset().held_mutexes();
  held.erase(0xA7E0AD1CULL); // verifier atomic marker has no mutex handoff.
  for (auto it = held.begin(); it != held.end();) {
    std::uint64_t base = 0;
    core::ConcurrentGlobalEnv::SymbolicIndex index;
    if (eng.ctx().concurrent_env.lock_symbolic_index(*it, base, index))
      it = held.erase(it);
    else ++it;
  }
  return held;
}

template < typename E >
void record_protected_scalar_write(E& eng, ar::Store* store,
                                   const ScalarLit& pointer, const Literal& value) {
  auto& env = eng.ctx().concurrent_env;
  if (!store->code()->is_function_body() || !env.mutex_values_available()) return;
  auto points = eng.data().normal().pointer_to_points_to(pointer.var());
  if (!points.is_set()) return;
  auto offset = eng.data().normal().pointer_to_pointer(pointer.var()).offset().singleton();
  for (auto* location : points) {
    auto* global = dyn_cast< GlobalMemoryLocation >(location);
    if (!global) continue;
    auto addr = core::IndexableTraits< MemoryLocation* >::index(location);
    if (!env.has_mutex_value_cell(addr)) continue;
    auto* written = dyn_cast< ar::IntegerType >(store->value()->type());
    ar::IntegerType* type = nullptr;
    std::uint64_t byte_offset = 0;
    if (offset && offset->to_z_number().template fits< std::uint64_t >()) {
      byte_offset = offset->to_z_number().template to< std::uint64_t >();
      for (const auto& cell : protected_integer_cells(global->global_var()))
        if (cell.offset == byte_offset) type = cell.type;
    }
    if (!type || !written || written->bit_width() != type->bit_width() ||
        store->is_atomic() || store->is_volatile() ||
        !env.has_mutex_value_cell(addr, byte_offset, type->bit_width())) {
      env.invalidate_mutex_values();
      return;
    }
    auto interval = core::machine_int::Interval::top(type->bit_width(), type->sign());
    if (value.is_scalar()) {
      const auto& rhs = value.scalar();
      if (rhs.is_machine_int()) interval = core::machine_int::Interval(rhs.machine_int()).cast(type->bit_width(), type->sign());
      else if (rhs.is_machine_int_var()) interval = eng.data().normal().int_to_interval(rhs.var()).cast(type->bit_width(), type->sign());
    }
    env.join_mutex_ongoing(addr, value_mutexes(eng), interval, byte_offset);
  }
}

// On first acquisition replace entry may-values with a complete handoff;
// on nested acquisition retain own values and cover waiting with the old locks.
template < typename E >
void refresh_protected_values(E& eng,
                             const std::unordered_set< std::uint64_t >& held,
                             bool first,
                             const std::unordered_set< std::uint64_t >& current) {
  auto& env = eng.ctx().concurrent_env;
  if (!env.mutex_values_available() || eng.data().normal().is_bottom()) return;
  auto* bundle = eng.ctx().bundle;
  for (auto it = bundle->global_begin(); it != bundle->global_end(); ++it) {
    auto* global = *it;
    if (!global->is_definition() || global->is_thread_local()) continue;
    auto* location = eng.ctx().mem_factory->get_global(global);
    auto addr = core::IndexableTraits< MemoryLocation* >::index(location);
    if (!env.has_mutex_value_cell(addr)) continue;
    for (const auto& cell : protected_integer_cells(global)) {
      auto* type = cell.type;
      if (!env.has_mutex_value_cell(addr, cell.offset, type->bit_width())) continue;
      auto external = (first || held.empty())
          ? env.read_mutex_handoff(addr, held, type->bit_width(), type->sign(), cell.offset)
          : env.read_mutex_ongoing(addr, held, type->bit_width(), type->sign(), cell.offset);
      if (!global->type()->pointee()->is_integer() &&
          !env.mutex_field_protected(addr, cell.offset, type->bit_width(), current))
        external.set_to_top();
      if (!first && external.is_bottom()) continue;
      if (external.is_bottom()) external.set_to_top();
      auto* pointer = eng.var_factory().create_unnamed_shadow(global->type());
      auto* temporary = eng.var_factory().create_unnamed_shadow(type);
      auto literal = core::Literal< Variable*, MemoryLocation* >::machine_int_var(temporary);
      core::MachineInt size(eng.data_layout().store_size_in_bytes(type),
                            eng.data_layout().pointers.bit_width, core::Unsigned);
      eng.data().normal().pointer_assign(pointer, location, core::Nullity::non_null());
      eng.data().normal().pointer_assign(pointer, pointer,
          core::MachineInt(cell.offset, size.bit_width(), core::Unsigned));
      if (!first) {
        eng.data().normal().mem_read(literal, pointer, size);
        external.join_with(eng.data().normal().int_to_interval(temporary));
      }
      eng.data().normal().int_set(temporary, external);
      eng.data().normal().mem_write(pointer, literal, size);
      eng.data().normal().scalar_forget(temporary);
      eng.data().normal().scalar_forget(pointer);
    }
  }
}

// A release endpoint is state, not another source-language write/race event.
template < typename E >
void publish_protected_release(E& eng,
                              const std::unordered_set< std::uint64_t >& before,
                              const std::unordered_set< std::uint64_t >& after) {
  auto& env = eng.ctx().concurrent_env;
  if (!env.mutex_values_available() || eng.data().normal().is_bottom()) return;
  auto* bundle = eng.ctx().bundle;
  for (auto it = bundle->global_begin(); it != bundle->global_end(); ++it) {
    auto* global = *it;
    if (!global->is_definition() || global->is_thread_local()) continue;
    auto* location = eng.ctx().mem_factory->get_global(global);
    auto addr = core::IndexableTraits< MemoryLocation* >::index(location);
    if (!env.has_mutex_value_cell(addr)) continue;
    for (const auto& cell : protected_integer_cells(global)) {
      auto* type = cell.type;
      if (!env.has_mutex_value_cell(addr, cell.offset, type->bit_width())) continue;
      auto* pointer = eng.var_factory().create_unnamed_shadow(global->type());
      auto* temporary = eng.var_factory().create_unnamed_shadow(type);
      auto literal = core::Literal< Variable*, MemoryLocation* >::machine_int_var(temporary);
      core::MachineInt size(eng.data_layout().store_size_in_bytes(type),
                            eng.data_layout().pointers.bit_width, core::Unsigned);
      eng.data().normal().pointer_assign(pointer, location, core::Nullity::non_null());
      eng.data().normal().pointer_assign(pointer, pointer,
          core::MachineInt(cell.offset, size.bit_width(), core::Unsigned));
      eng.data().normal().mem_read(literal, pointer, size);
      auto endpoint = eng.data().normal().int_to_interval(temporary);
      if (endpoint.is_bottom()) endpoint.set_to_top();
      if (!global->type()->pointee()->is_integer() &&
          !env.mutex_field_protected(addr, cell.offset, type->bit_width(), before))
        endpoint.set_to_top();
      endpoint.join_with(env.read_mutex_ongoing(addr, before, type->bit_width(), type->sign(), cell.offset));
      env.join_mutex_release(addr, after, endpoint, cell.offset);
      eng.data().normal().scalar_forget(temporary);
      eng.data().normal().scalar_forget(pointer);
    }
  }
}

} // namespace analyzer
} // namespace ikos
