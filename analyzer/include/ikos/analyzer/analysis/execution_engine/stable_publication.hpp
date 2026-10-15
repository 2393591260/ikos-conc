// Copyright and license: see numerical.hpp.
#pragma once

#include <ikos/analyzer/analysis/execution_engine/handle_origin.hpp>

namespace ikos {
namespace analyzer {

inline bool direct_storage_address(ar::Value* value) {
  for (unsigned depth = 0; depth < 32; ++depth) {
    if (isa< ar::GlobalVariable >(value) || isa< ar::LocalVariable >(value)) return true;
    auto* variable = dyn_cast< ar::InternalVariable >(value);
    auto* definition = variable ? unique_def(variable) : nullptr;
    if (auto* copy = dyn_cast_or_null< ar::Assignment >(definition)) value = copy->operand();
    else if (auto* cast = dyn_cast_or_null< ar::UnaryOperation >(definition)) {
      if (cast->op() != ar::UnaryOperation::Bitcast) return false;
      value = cast->operand();
    } else if (auto* shift = dyn_cast_or_null< ar::PointerShift >(definition)) {
      for (auto it = shift->term_begin(); it != shift->term_end(); ++it)
        if (!isa< ar::IntegerConstant >((*it).second)) return false;
      value = shift->pointer();
    } else return false;
  }
  return false;
}

// Only this closed effects subset may treat the heap Store board as complete.
// Other intrinsics can havoc bytes without publishing a typed pointer write.
inline bool heap_pointer_call_covered(ar::CallBase* call) {
  auto* called = call->called();
  for (unsigned depth = 0; depth < 16; ++depth) {
    auto* variable = dyn_cast< ar::InternalVariable >(called);
    auto* cast = variable ? dyn_cast_or_null< ar::UnaryOperation >(unique_def(variable)) : nullptr;
    if (!cast || cast->op() != ar::UnaryOperation::Bitcast) break;
    if (variable->code()->is_function_body()) {
      auto* owner = variable->code()->function();
      for (auto it = owner->param_begin(); it != owner->param_end(); ++it)
        if (*it == variable) return false;
    }
    called = cast->operand();
  }
  auto* target = dyn_cast< ar::FunctionPointerConstant >(called);
  if (!target) return false;
  auto* function = target->function();
  if (function->is_definition()) return true;
  auto id = function->intrinsic_id();
  switch (id) {
    case ar::Intrinsic::IkosNonDet:
    case ar::Intrinsic::IkosAssume:
    case ar::Intrinsic::IkosAssert:
    case ar::Intrinsic::LibcMalloc:
    case ar::Intrinsic::LibcFree:
    case ar::Intrinsic::LibcAbort:
    case ar::Intrinsic::Trap:
      return true;
    case ar::Intrinsic::LibcPrintf:
      for (unsigned i = 1; i < call->num_arguments(); ++i)
        if (!call->argument(i)->type()->is_integer() &&
            !call->argument(i)->type()->is_float()) return false;
      return call->num_arguments() > 0;
    default: break;
  }
  const auto& name = function->name();
  if (name == "pthread_create" || id == ar::Intrinsic::PthreadCreate)
    return call->num_arguments() == 4 && direct_storage_address(call->argument(0));
  if (name == "pthread_join" || id == ar::Intrinsic::PthreadJoin)
    return call->num_arguments() == 2 && isa< ar::NullConstant >(call->argument(1));
  if (name == "pthread_mutex_lock" || name == "pthread_mutex_unlock" ||
      id == ar::Intrinsic::PthreadMutexLock || id == ar::Intrinsic::PthreadMutexUnlock)
    return call->num_arguments() == 1 && direct_storage_address(call->argument(0));
  if (name == "__VERIFIER_nondet_int" || name == "__VERIFIER_nondet_uint") {
    auto* type = dyn_cast< ar::IntegerType >(function->type()->return_type());
    return call->num_arguments() == 0 && function->num_parameters() == 0 &&
           type && type->bit_width() == 32;
  }
  return false;
}

// Close every invocation of the worker, including address escapes and aliases.
inline bool worker_created_after(ar::Store* writer, ar::Function* worker,
                                  core::ConcurrentGlobalEnv& env,
                                  bool require_heap_effects = false) {
  if (!env.function_executes_at_most_once(writer->code()->function())) return false;
  std::vector< ar::CallBase* > creates;
  unsigned budget = 8192;
  auto mentions_worker = [&](auto&& self, ar::Value* value, unsigned depth) -> bool {
    if (depth >= 64) return true;
    if (auto* function = dyn_cast< ar::FunctionPointerConstant >(value))
      return function->function() == worker;
    if (auto* aggregate = dyn_cast< ar::StructConstant >(value)) {
      for (const auto& field : aggregate->values())
        if (self(self, field.value, depth + 1)) return true;
    } else if (auto* aggregate = dyn_cast< ar::SequentialConstant >(value)) {
      for (auto* element : aggregate->values())
        if (self(self, element, depth + 1)) return true;
    }
    return false;
  };
  auto scan = [&](ar::Code* code) {
    if (code == nullptr) return true;
    for (auto* block : *code) for (auto* statement : *block) {
      if (budget-- == 0) return false;
      if (code->is_function_body()) {
        auto* function = code->function();
        for (auto it = function->param_begin(); it != function->param_end(); ++it)
          if (statement->result_or_null() == *it) return false;
      }
      if (auto* call = dyn_cast< ar::CallBase >(statement)) {
        if (require_heap_effects && !heap_pointer_call_covered(call)) return false;
        if (mentions_worker(mentions_worker, call->called(), 0)) return false;
        for (unsigned i = 0; i < call->num_arguments(); ++i) {
          if (!mentions_worker(mentions_worker, call->argument(i), 0)) continue;
          auto* target = dyn_cast< ar::FunctionPointerConstant >(call->called());
          if (i != 2 || call->num_arguments() != 4 || target == nullptr ||
              !target->function()->is_declaration() ||
              target->function()->name() != "pthread_create" ||
              !isa< ar::FunctionPointerConstant >(call->argument(i))) return false;
          creates.push_back(call);
        }
      } else {
        for (auto it = statement->op_begin(); it != statement->op_end(); ++it)
          if (mentions_worker(mentions_worker, *it, 0)) return false;
      }
    }
    return true;
  };
  auto* bundle = worker->bundle();
  for (auto it = bundle->function_begin(); it != bundle->function_end(); ++it)
    if (!scan((*it)->body_or_null())) return false;
  for (auto it = bundle->global_begin(); it != bundle->global_end(); ++it)
    if (!scan((*it)->initializer_or_null())) return false;
  if (creates.empty()) return false;
  auto* block = writer->parent();
  std::unordered_set< ar::BasicBlock* > seen;
  std::vector< ar::BasicBlock* > pending(block->successor_begin(), block->successor_end());
  while (!pending.empty()) {
    auto* next = pending.back(); pending.pop_back();
    if (next == block || seen.size() >= 256) return false;
    if (!seen.insert(next).second) continue;
    pending.insert(pending.end(), next->successor_begin(), next->successor_end());
  }
  for (auto* create : creates)
    if (create->parent() != block || !handle_statement_dominates(writer, create))
      return false;
  return true;
}

// A whole pointer slot is written once, before every invocation of this worker.
// The first subset accepts same-block creates; helpers/escapes fall back to may-values.
inline ar::Store* prove_stable_publication_writer(ar::GlobalVariable* slot,
                                             ar::Function* worker,
                                             core::ConcurrentGlobalEnv& env) {
  if (!slot->is_definition() || slot->is_thread_local() ||
      !slot->type()->pointee()->is_pointer()) return nullptr;
  ar::Store* writer = nullptr;
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
        if (load->result()->type()->is_pointer() &&
            exact_handle_slot(load->operand(), slot)) continue;
      } else if (auto* store = dyn_cast< ar::Store >(statement)) {
        if (!store->value()->type()->is_pointer() ||
            !exact_handle_slot(store->pointer(), slot) ||
            mentions_handle_slot(store->value(), slot)) return false;
        if (code->is_function_body()) {
          if (writer != nullptr) return false;
          writer = store;
        }
        continue;
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
  auto* bundle = slot->bundle();
  for (auto it = bundle->function_begin(); it != bundle->function_end(); ++it)
    if (!scan((*it)->body_or_null())) return nullptr;
  for (auto it = bundle->global_begin(); it != bundle->global_end(); ++it)
    if (!scan((*it)->initializer_or_null())) return nullptr;
  return writer && worker_created_after(writer, worker, env) ? writer : nullptr;
}

inline ar::Store* stable_publication_writer(ar::GlobalVariable* slot,
                                             ar::Function* worker,
                                             core::ConcurrentGlobalEnv& env) {
  ar::Store* writer = nullptr;
  if (env.get_publication_certificate(slot, worker, writer)) return writer;
  writer = prove_stable_publication_writer(slot, worker, env);
  // The bundle is immutable; runtime effects invalidate values, not this proof.
  env.cache_publication_certificate(slot, worker, writer);
  return writer;
}

} // namespace analyzer
} // namespace ikos
