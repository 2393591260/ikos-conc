#pragma once

#include <limits>
#include <map>
#include <string>
#include <vector>

#include "unroll.hpp"

namespace ikos { namespace analyzer { namespace bmc {

// Each trace is one dynamic invocation, even when AR variables are shared.
struct ExecutionTrace {
  static constexpr std::size_t no_creation = std::numeric_limits< std::size_t >::max();
  std::string id;
  ar::Function* root = nullptr;
  std::vector< BmcEvent > events;
  ThreadArgument argument;
  std::size_t creator_path = 0;
  std::size_t creator_step = no_creation;
  std::size_t clock_start = 0;

  std::string value_prefix() const {
    return id == "startup" ? "value_init_" : "value_" + id + "_";
  }
};

// A finite completion choice, not an abstract summary of omitted child writes.
struct CompletionPlan {
  std::size_t target_creation = ExecutionTrace::no_creation;
  std::vector< ExecutionTrace > auxiliary;
  std::map< std::size_t, std::size_t > joins; // main join step -> trace index
};

inline bool bind_creation_step(ar::Function* child,
    const std::vector< BmcEvent >& creator, std::size_t step, ThreadArgument& out) {
  std::size_t ordinal = 0;
  for (std::size_t i = 0; i < creator.size(); ++i) {
    if (creator[i].kind != EvKind::Create ||
        creator[i].thread != reinterpret_cast< std::uint64_t >(child)) continue;
    if (i == step) return bind_created_argument(child, creator, ordinal, out);
    ++ordinal;
  }
  return false;
}

// Version one keeps dynamic step indices stable and excludes nested lifecycles.
inline bool completion_prefix_supported(const std::vector< BmcEvent >& path,
                                        bool child = false) {
  for (const auto& event : path) {
    auto* call = core::dyn_cast< ar::CallBase >(event.stmt);
    auto* function = call != nullptr ? exact_callee(call) : nullptr;
    if (event.synthetic_condition || (call != nullptr && condition_call(call)) ||
        (function != nullptr && function->name() == "pthread_detach") ||
        (child && (event.kind == EvKind::Create || event.kind == EvKind::Join)) ||
        (event.called_function != nullptr && event.called_function->name() == "main")) return false;
  }
  return true;
}

inline ar::ReturnValue* completion_return(const std::vector< BmcEvent >& path,
                                         ar::Function* root) {
  auto* ret = path.empty() ? nullptr : core::dyn_cast< ar::ReturnValue >(path.back().stmt);
  return ret != nullptr && ret->code() == root->body_or_null() ? ret : nullptr;
}

// Match generations at the consuming load, before a later slot overwrite.
// Only an unescaped, typed scalar/array in this main invocation is supported.
inline bool completion_join_creation(const std::vector< BmcEvent >& path,
                                      std::size_t before, std::size_t& creation) {
  auto* join = core::dyn_cast< ar::CallBase >(path[before].stmt);
  auto* api = join != nullptr ? exact_callee(join) : nullptr;
  if (api == nullptr || !api->is_declaration() || join->num_arguments() != 2 ||
      !(api->name() == "pthread_join" || api->intrinsic_id() == ar::Intrinsic::PthreadJoin) ||
      !core::isa< ar::NullConstant >(join->argument(1))) return false;
  int atomic = 0;
  for (std::size_t i = 0; i < before; ++i) {
    if (path[i].mutex != PSEUDO_ATOMIC_LOCK) continue;
    if (path[i].kind == EvKind::Lock) ++atomic;
    if (path[i].kind == EvKind::Unlock) --atomic;
  }
  if (atomic != 0) return false;
  ar::Value* value = join->argument(0);
  ar::Load* load = nullptr;
  std::size_t read = before;
  for (unsigned depth = 0; depth < 32; ++depth) {
    auto* variable = core::dyn_cast< ar::InternalVariable >(value);
    if (variable == nullptr) return false;
    std::size_t index = 0;
    auto* definition = reaching_definition(variable, path, read, index);
    if ((load = core::dyn_cast_or_null< ar::Load >(definition))) {read = index; break;}
    auto* assignment = core::dyn_cast_or_null< ar::Assignment >(definition);
    if (assignment == nullptr || assignment->operand()->type() != value->type()) return false;
    value = assignment->operand(); read = index;
  }
  auto* main = join->bundle()->function_or_null("main");
  BmcEvent address;
  if (load == nullptr || !load->result()->type()->is_integer() ||
      !pointer_on_path(load->operand(), path, read, main, nullptr, address) ||
      address.local_base == nullptr || address.local_base->function() != main ||
      address.var_id != 0 || address.base != 0 || address.heap_allocation != nullptr ||
      !shared_main_allocation(address.local_base, path)) return false;
  const auto width = join->bundle()->data_layout().pointers.bit_width / 8;
  core::ZNumber extent;
  auto* type = address.local_base->type()->pointee();
  while (auto* array = core::dyn_cast< ar::ArrayType >(type)) type = array->element_type();
  auto* integer = core::dyn_cast< ar::IntegerType >(type);
  if (integer == nullptr || integer->bit_width() != width * 8 ||
      join->bundle()->data_layout().store_size_in_bytes(load->result()->type()) != core::ZNumber(width) ||
      !memory_extent(address, extent) || address.const_off < 0 ||
      address.const_off + width > extent || address.const_off % width != 0) return false;
  bool matched = false;
  for (std::size_t j = read; j-- > 0;) {
    if (path[j].stmt->result_or_null() == address.local_base) break;
    if (path[j].kind != EvKind::Create) continue;
    auto* call = core::cast< ar::CallBase >(path[j].stmt);
    BmcEvent output;
    if (!pointer_on_path(call->argument(0), path, j, main, nullptr, output)) return false;
    if (output.local_base != address.local_base) continue;
    std::uint64_t output_width, alignment;
    if (output.var_id != 0 || !create_output_layout(call, output_width, alignment)) return false;
    if (output.const_off + output_width <= address.const_off ||
        address.const_off + width <= output.const_off) continue;
    if (output.const_off != address.const_off || output_width != width ||
        !successful_create_result(path[j]) || !constant_create_output_aligned(call, output)) return false;
    creation = j; matched = true; break;
  }
  if (!matched) return false;
  // Calls and explicit stores cannot mutate or publish the opaque handle object.
  for (std::size_t i = 0; i < path.size(); ++i) {
    if (auto* store = core::dyn_cast< ar::Store >(path[i].stmt)) {
      if (abstract_initializer(store)) continue;
      BmcEvent output;
      if (!pointer_on_path(store->pointer(), path, i, main, nullptr, output) ||
          output.local_base == address.local_base) return false;
      if (store->value()->type()->is_pointer() &&
          !core::isa< ar::NullConstant >(store->value()) &&
          !core::isa< ar::FunctionPointerConstant >(store->value())) {
        BmcEvent stored;
        if (!pointer_on_path(store->value(), path, i, main, nullptr, stored) ||
            stored.local_base == address.local_base) return false;
      }
    }
    auto* call = core::dyn_cast< ar::CallBase >(path[i].stmt);
    if (call == nullptr) continue;
    for (std::size_t arg = 0; arg < call->num_arguments(); ++arg) {
      auto* operand = call->argument(arg);
      if (!operand->type()->is_pointer() || core::isa< ar::NullConstant >(operand) ||
          core::isa< ar::FunctionPointerConstant >(operand)) continue;
      BmcEvent passed;
      if (!pointer_on_path(operand, path, i, main, nullptr, passed) ||
          (passed.local_base == address.local_base &&
           !(path[i].kind == EvKind::Create && arg == 0))) return false;
    }
  }
  return true;
}

} } }
