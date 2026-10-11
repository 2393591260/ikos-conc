// Copyright and license: see numerical.hpp.
#pragma once

#include <ikos/analyzer/analysis/call_context.hpp>
#include <ikos/analyzer/analysis/execution_engine/symbolic_index.hpp>

namespace ikos {
namespace analyzer {

// Only unique, value-preserving pointer definitions may carry this address.
inline bool mentions_handle_slot(ar::Value* value, ar::GlobalVariable* slot,
                                 unsigned depth = 0) {
  if (depth >= 64) return true;
  if (value == slot) return true;
  if (auto* aggregate = dyn_cast< ar::StructConstant >(value)) {
    for (const auto& field : aggregate->values())
      if (mentions_handle_slot(field.value, slot, depth + 1)) return true;
  } else if (auto* aggregate = dyn_cast< ar::SequentialConstant >(value)) {
    for (auto* element : aggregate->values())
      if (mentions_handle_slot(element, slot, depth + 1)) return true;
  }
  auto* variable = dyn_cast< ar::InternalVariable >(value);
  if (variable == nullptr || !variable->type()->is_pointer()) return false;
  auto* definition = unique_def(variable);
  if (auto* copy = dyn_cast_or_null< ar::Assignment >(definition))
    return mentions_handle_slot(copy->operand(), slot, depth + 1);
  if (auto* cast = dyn_cast_or_null< ar::UnaryOperation >(definition))
    if (cast->op() == ar::UnaryOperation::Bitcast)
      return mentions_handle_slot(cast->operand(), slot, depth + 1);
  if (auto* shift = dyn_cast_or_null< ar::PointerShift >(definition))
    return mentions_handle_slot(shift->pointer(), slot, depth + 1);
  return false;
}

inline bool exact_handle_slot(ar::Value* value, ar::GlobalVariable* slot,
                              unsigned depth = 0) {
  if (depth >= 64) return false;
  if (value == slot) return true;
  auto* variable = dyn_cast< ar::InternalVariable >(value);
  if (variable == nullptr || !variable->type()->is_pointer()) return false;
  auto* definition = unique_def(variable);
  if (auto* copy = dyn_cast_or_null< ar::Assignment >(definition))
    return exact_handle_slot(copy->operand(), slot, depth + 1);
  if (auto* cast = dyn_cast_or_null< ar::UnaryOperation >(definition))
    if (cast->op() == ar::UnaryOperation::Bitcast)
      return exact_handle_slot(cast->operand(), slot, depth + 1);
  if (auto* shift = dyn_cast_or_null< ar::PointerShift >(definition)) {
    for (auto it = shift->term_begin(); it != shift->term_end(); ++it) {
      auto* constant = dyn_cast< ar::IntegerConstant >((*it).second);
      if (constant == nullptr || !constant->value().is_zero()) return false;
    }
    return exact_handle_slot(shift->pointer(), slot, depth + 1);
  }
  return false;
}

// Removing the earlier statement must make the later statement unreachable.
inline bool handle_statement_dominates(ar::Statement* earlier,
                                        ar::Statement* later) {
  if (earlier->code() != later->code()) return false;
  if (earlier->parent() == later->parent()) {
    for (auto* statement : *earlier->parent()) {
      if (statement == later) return false;
      if (statement == earlier) return true;
    }
    return false;
  }
  std::unordered_set< ar::BasicBlock* > seen;
  std::vector< ar::BasicBlock* > pending{earlier->code()->entry_block()};
  while (!pending.empty()) {
    auto* block = pending.back(); pending.pop_back();
    if (block == earlier->parent() || !seen.insert(block).second) continue;
    if (block == later->parent() || seen.size() > 256) return false;
    pending.insert(pending.end(), block->successor_begin(), block->successor_end());
  }
  return true;
}

// A whole scalar slot has one create writer; its address never escapes.
inline ar::CallBase* single_handle_writer(ar::GlobalVariable* slot) {
  auto* type = dyn_cast< ar::IntegerType >(slot->type()->pointee());
  if (type == nullptr || !slot->is_definition() || slot->is_thread_local())
    return nullptr;
  ar::CallBase* writer = nullptr;
  unsigned budget = 8192;
  auto scan = [&](ar::Code* code) {
    if (code == nullptr) return true;
    for (auto* block : *code) for (auto* statement : *block) {
      if (budget-- == 0) return false;
      bool mentioned = false;
      for (auto it = statement->op_begin(); it != statement->op_end(); ++it)
        mentioned |= mentions_handle_slot(*it, slot);
      if (!mentioned) continue;
      if (auto* load = dyn_cast< ar::Load >(statement)) {
        auto* read_type = dyn_cast< ar::IntegerType >(load->result()->type());
        if (exact_handle_slot(load->operand(), slot) && read_type != nullptr &&
            read_type->bit_width() == type->bit_width()) continue;
      } else if (auto* store = dyn_cast< ar::Store >(statement)) {
        auto* constant = dyn_cast< ar::IntegerConstant >(store->value());
        if (!code->is_function_body() && constant != nullptr &&
            exact_handle_slot(store->pointer(), slot) &&
            constant->value().bit_width() == type->bit_width()) continue;
      } else if (auto* copy = dyn_cast< ar::Assignment >(statement)) {
        if (copy->result()->type()->is_pointer() &&
            unique_def(copy->result()) == copy &&
            exact_handle_slot(copy->operand(), slot)) continue;
      } else if (auto* cast = dyn_cast< ar::UnaryOperation >(statement)) {
        if (cast->op() == ar::UnaryOperation::Bitcast &&
            cast->result()->type()->is_pointer() &&
            unique_def(cast->result()) == cast &&
            exact_handle_slot(cast->operand(), slot)) continue;
      } else if (auto* shift = dyn_cast< ar::PointerShift >(statement)) {
        bool zero = unique_def(shift->result()) == shift &&
                    exact_handle_slot(shift->pointer(), slot);
        for (auto it = shift->term_begin(); it != shift->term_end(); ++it) {
          auto* constant = dyn_cast< ar::IntegerConstant >((*it).second);
          zero &= constant != nullptr && constant->value().is_zero();
        }
        if (zero) continue;
      } else if (auto* call = dyn_cast< ar::CallBase >(statement)) {
        auto* target = dyn_cast< ar::FunctionPointerConstant >(call->called());
        bool create = target != nullptr && target->function()->is_declaration() &&
                      target->function()->name() == "pthread_create" &&
                      call->num_arguments() == 4;
        if (create && writer == nullptr &&
            exact_handle_slot(call->argument(0), slot) &&
            !mentions_handle_slot(call->argument(1), slot) &&
            !mentions_handle_slot(call->argument(2), slot) &&
            !mentions_handle_slot(call->argument(3), slot) &&
            isa< ar::FunctionPointerConstant >(call->argument(2))) {
          writer = call;
          continue;
        }
      }
      return false;
    }
    return true;
  };
  auto* bundle = slot->bundle();
  for (auto it = bundle->function_begin(); it != bundle->function_end(); ++it)
    if (!scan((*it)->body_or_null())) return nullptr;
  for (auto it = bundle->global_begin(); it != bundle->global_end(); ++it)
    if (!scan((*it)->initializer_or_null())) return nullptr;
  return writer;
}

// Every zero return definition follows creation; other return values are constants.
inline bool handle_created_on_zero_return(ar::Statement* creation) {
  auto* code = creation->code();
  bool found_zero = false, found_return = false;
  for (auto* block : *code) for (auto* statement : *block) {
    auto* ret = dyn_cast< ar::ReturnValue >(statement);
    if (ret == nullptr) continue;
    found_return = true;
    if (!ret->has_operand()) return false;
    if (auto* constant = dyn_cast< ar::IntegerConstant >(ret->operand())) {
      if (constant->value().is_zero()) {
        if (!handle_statement_dominates(creation, ret)) return false;
        found_zero = true;
      }
      continue;
    }
    auto* variable = dyn_cast< ar::InternalVariable >(ret->operand());
    if (variable == nullptr) return false;
    bool assigned = false;
    for (auto* definitions : *code) for (auto* definition : *definitions) {
      if (definition->result_or_null() != variable) continue;
      auto* assign = dyn_cast< ar::Assignment >(definition);
      auto* constant = assign ? dyn_cast< ar::IntegerConstant >(assign->operand()) : nullptr;
      if (constant == nullptr) return false;
      assigned = true;
      if (constant->value().is_zero()) {
        if (!handle_statement_dominates(creation, definition)) return false;
        found_zero = true;
      }
    }
    if (!assigned) return false;
  }
  return found_return && found_zero;
}

inline ar::Statement* handle_observation_in(ar::Statement* observation,
                                           ar::Code* code,
                                           const CallContext* context) {
  while (observation->code() != code && context != nullptr && !context->empty()) {
    observation = context->call();
    context = context->parent();
  }
  return observation->code() == code ? observation : nullptr;
}

inline bool same_zero_return_source(ar::Value* value, ar::Value* source) {
  for (unsigned depth = 0; depth < 64; ++depth) {
    if (value == source) return true;
    auto* variable = dyn_cast< ar::InternalVariable >(value);
    if (variable == nullptr || !variable->type()->is_integer()) return false;
    auto* definition = unique_def(variable);
    if (auto* copy = dyn_cast_or_null< ar::Assignment >(definition)) {
      auto* type = dyn_cast< ar::IntegerType >(copy->operand()->type());
      if (type == nullptr || type->bit_width() !=
          cast< ar::IntegerType >(variable->type())->bit_width()) return false;
      value = copy->operand();
    } else if (auto* cast = dyn_cast_or_null< ar::UnaryOperation >(definition)) {
      if (cast->op() != ar::UnaryOperation::Bitcast &&
          cast->op() != ar::UnaryOperation::SExt &&
          cast->op() != ar::UnaryOperation::ZExt) return false;
      auto* type = dyn_cast< ar::IntegerType >(cast->operand()->type());
      auto width = ar::cast< ar::IntegerType >(variable->type())->bit_width();
      if (type == nullptr || type->bit_width() > width ||
          (cast->op() == ar::UnaryOperation::Bitcast && type->bit_width() != width))
        return false;
      value = cast->operand();
    } else return false;
  }
  return false;
}

// Liveness may forget the numeric result after its branch test. The executed
// dominating equality still establishes zero for the same single invocation.
inline bool zero_return_guard_dominates(ar::CallBase* caller, ar::Load* load,
                                        const CallContext* context) {
  auto* observation = handle_observation_in(load, caller->code(), context);
  if (observation == nullptr) return false;
  for (auto* block : *caller->code()) for (auto* statement : *block) {
    auto* cmp = dyn_cast< ar::Comparison >(statement);
    if (cmp == nullptr || (cmp->predicate() != ar::Comparison::SIEQ &&
                           cmp->predicate() != ar::Comparison::UIEQ)) continue;
    ar::Value* value = cmp->left();
    auto* constant = dyn_cast< ar::IntegerConstant >(cmp->right());
    if (constant == nullptr) {
      constant = dyn_cast< ar::IntegerConstant >(cmp->left());
      value = cmp->right();
    }
    if (constant != nullptr && constant->value().is_zero() &&
        same_zero_return_source(value, caller->result()) &&
        handle_statement_dominates(caller, cmp) &&
        handle_statement_dominates(cmp, observation)) return true;
  }
  return false;
}

// On every normal exit reachable after this creation, the returned value is
// zero. Track the last assignment to each return operand; unknown definitions
// and budgets reject the certificate, including a nonzero value set earlier.
inline bool handle_creation_returns_zero(ar::Statement* creation) {
  auto* code = creation->code();
  unsigned budget = 8192;
  bool found_return = false;
  for (auto* returns : *code) for (auto* statement : *returns) {
    auto* ret = dyn_cast< ar::ReturnValue >(statement);
    if (ret == nullptr) continue;
    found_return = true;
    if (!ret->has_operand()) return false;
    auto* constant = dyn_cast< ar::IntegerConstant >(ret->operand());
    auto* variable = dyn_cast< ar::InternalVariable >(ret->operand());
    if (constant == nullptr && variable == nullptr) return false;
    std::unordered_map< ar::BasicBlock*, unsigned > seen;
    std::vector< std::pair< ar::BasicBlock*, bool > > pending;
    auto process = [&](ar::BasicBlock* block, bool zero, bool suffix) {
      bool started = !suffix;
      for (auto* current : *block) {
        if (budget-- == 0) return false;
        if (!started) {
          if (current == creation) started = true;
          continue;
        }
        if (variable != nullptr && current->result_or_null() == variable) {
          auto* assign = dyn_cast< ar::Assignment >(current);
          auto* value = assign ? dyn_cast< ar::IntegerConstant >(assign->operand()) : nullptr;
          zero = value != nullptr && value->value().is_zero();
        }
        if (current == ret && !(constant ? constant->value().is_zero() : zero))
          return false;
        if (isa< ar::ReturnValue >(current)) return true;
      }
      for (auto it = block->successor_begin(); it != block->successor_end(); ++it)
        pending.emplace_back(*it, zero);
      return true;
    };
    if (!process(creation->parent(), false, true)) return false;
    while (!pending.empty()) {
      auto item = pending.back(); pending.pop_back();
      unsigned mask = item.second ? 2u : 1u;
      if (seen[item.first] & mask) continue;
      seen[item.first] |= mask;
      if (!process(item.first, item.second, false)) return false;
    }
  }
  return found_return;
}

// A nonzero result excludes a once-only creation whose every normal post-create
// return is zero. Historical spawned facts are deliberately not removed.
template < typename E >
void on_comparison_live_children(E& eng, ar::Comparison* cmp) {
  if (cmp->predicate() != ar::Comparison::SINE &&
      cmp->predicate() != ar::Comparison::UINE) return;
  ar::Value* value = cmp->left();
  auto* constant = dyn_cast< ar::IntegerConstant >(cmp->right());
  if (constant == nullptr) {
    constant = dyn_cast< ar::IntegerConstant >(cmp->left());
    value = cmp->right();
  }
  if (constant == nullptr || !constant->value().is_zero()) return;
  ar::CallBase* caller = nullptr;
  for (unsigned depth = 0; depth < 64; ++depth) {
    auto* variable = dyn_cast< ar::InternalVariable >(value);
    if (variable == nullptr) return;
    auto* definition = unique_def(variable);
    if (auto* call = dyn_cast_or_null< ar::CallBase >(definition)) {
      caller = call; break;
    }
    if (auto* copy = dyn_cast_or_null< ar::Assignment >(definition)) {
      if (!same_zero_return_source(value, copy->operand())) return;
      value = copy->operand();
    } else if (auto* cast = dyn_cast_or_null< ar::UnaryOperation >(definition)) {
      if (!same_zero_return_source(value, cast->operand())) return;
      value = cast->operand();
    } else return;
  }
  auto* target = caller ? dyn_cast< ar::FunctionPointerConstant >(caller->called()) : nullptr;
  if (target == nullptr || target->function()->is_declaration() ||
      !handle_statement_dominates(caller, cmp)) return;
  auto* function = target->function();
  auto& env = eng.ctx().concurrent_env;
  if (!env.function_executes_at_most_once(function)) return;
  auto* bundle = function->bundle();
  for (auto it = bundle->global_begin(); it != bundle->global_end(); ++it) {
    if (!(*it)->type()->pointee()->is_integer()) continue;
    auto* creation = single_handle_writer(*it);
    if (creation == nullptr || creation->code()->function_or_null() != function ||
        !handle_creation_returns_zero(creation)) continue;
    auto sid = env.site_id(creation);
    auto* child = env.site_func(sid);
    if (child != nullptr && env.thread_instances_are_static(child->name()))
      eng.lockset().remove_live_child(sid);
  }
}

// Certify a completed unique writer, including a caller's zero-return guard.
// Failure leaves ordinary may-memory/interference and UNKNOWN unchanged.
template < typename E >
std::uint64_t completed_handle_origin(E& eng, ar::GlobalVariable* slot,
                                      ar::Load* load) {
  auto* creation = single_handle_writer(slot);
  if (creation == nullptr ||
      !eng.ctx().concurrent_env.function_executes_at_most_once(
          creation->code()->function_or_null())) return 0;
  ar::Statement* completed = creation;
  for (unsigned depth = 0; depth < 32; ++depth) {
    auto* observation = handle_observation_in(load, completed->code(), eng.call_context());
    if (observation != nullptr) {
      if (!handle_statement_dominates(completed, observation)) return 0;
      return eng.ctx().concurrent_env.site_id(creation);
    }
    auto* function = completed->code()->function_or_null();
    if (function == nullptr || function->name() == "main") return 0;
    ar::CallBase* caller = nullptr;
    auto* bundle = function->bundle();
    for (auto it = bundle->function_begin(); it != bundle->function_end(); ++it) {
      if ((*it)->is_declaration()) continue;
      for (auto* block : *(*it)->body()) for (auto* statement : *block) {
        auto* call = dyn_cast< ar::CallBase >(statement);
        auto* target = call ? dyn_cast< ar::FunctionPointerConstant >(call->called()) : nullptr;
        if (target != nullptr && target->function() == function) {
          if (caller != nullptr) return 0;
          caller = call;
        }
      }
    }
    if (caller == nullptr) return 0;
    bool dominates_returns = true, found_return = false;
    for (auto* block : *completed->code()) for (auto* statement : *block)
      if (isa< ar::ReturnValue >(statement)) {
        found_return = true;
        dominates_returns &= handle_statement_dominates(completed, statement);
      }
    if (!found_return) return 0;
    if (!dominates_returns) {
      if (!caller->has_result() || !handle_created_on_zero_return(completed)) return 0;
      auto* result = eng.var_factory().get_internal(caller->result());
      if (!result->type()->is_integer()) return 0;
      auto value = eng.data().normal().int_to_interval(result).singleton();
      if ((!value || !value->is_zero()) &&
          !zero_return_guard_dominates(caller, load, eng.call_context())) return 0;
    }
    completed = caller;
  }
  return 0;
}

} // namespace analyzer
} // namespace ikos
