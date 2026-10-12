// Copyright and license: see data_race.hpp.
#pragma once

#include <ikos/analyzer/analysis/execution_engine/handle_origin.hpp>
#include <ikos/analyzer/analysis/execution_engine/pthread_initialization.hpp>
#include <ikos/analyzer/checker/data_race.hpp>

namespace ikos {
namespace analyzer {

// A pair-specific init -> first publication -> mutex handoff certificate.
// Unknown provenance or effects keep the pair; never mark a whole heap site private.
class InitPublication {
  struct Source {
    ar::Value* root = nullptr;
    CallContext* context = nullptr;
    core::ZNumber offset{0};
  };

  Context& _ctx;
  const std::vector< AccessRecord >& _accesses;
  bool _available = false;
  unsigned _budget = 65536;

  bool spend() { return _budget != 0 && (--_budget, true); }

  Source source(ar::Value* value, CallContext* context) {
    Source out;
    for (unsigned depth = 0; depth < 64 && spend(); ++depth) {
      if (!value || !value->type()->is_pointer()) return {};
      auto* variable = dyn_cast< ar::InternalVariable >(value);
      if (!variable) { out.root = value; out.context = context; return out; }
      auto* definition = unique_def(variable);
      if (!definition) {
        // Only an unassigned formal in this exact dynamic call frame is fixed.
        auto* function = variable->code()->function_or_null();
        if (!function || !context || context->empty()) return {};
        for (auto* block : *variable->code()) for (auto* s : *block)
          if (!spend() || s->result_or_null() == variable) return {};
        auto* call = context->call();
        if (pthread_initialization_callee(call) != function) return {};
        unsigned index = 0;
        bool bound = false;
        for (auto it = function->param_begin(); it != function->param_end(); ++it, ++index)
          if (*it == variable && index < call->num_arguments()) {
            value = call->argument(index); context = context->parent(); bound = true; break;
          }
        if (!bound) return {};
        continue;
      }
      if (auto* copy = dyn_cast< ar::Assignment >(definition)) {
        value = copy->operand();
      } else if (auto* cast = dyn_cast< ar::UnaryOperation >(definition)) {
        if (cast->op() != ar::UnaryOperation::Bitcast ||
            !cast->operand()->type()->is_pointer()) return {};
        value = cast->operand();
      } else if (auto* shift = dyn_cast< ar::PointerShift >(definition)) {
        for (auto it = shift->term_begin(); it != shift->term_end(); ++it) {
          auto* constant = dyn_cast< ar::IntegerConstant >((*it).second);
          if (!constant) return {};
          out.offset += (*it).first.to_z_number() * constant->value().to_z_number();
        }
        value = shift->pointer();
      } else {
        out.root = variable; out.context = context; return out;
      }
    }
    return {};
  }

  ar::CallBase* malloc_source(const Source& s) {
    auto* variable = dyn_cast_or_null< ar::InternalVariable >(s.root);
    auto* call = variable ? dyn_cast_or_null< ar::CallBase >(unique_def(variable)) : nullptr;
    auto* function = call ? pthread_initialization_callee(call) : nullptr;
    if (!function || !function->is_declaration() ||
        function->intrinsic_id() != ar::Intrinsic::LibcMalloc ||
        call->num_arguments() != 1 || !call->argument(0)->type()->is_integer()) return nullptr;
    return call;
  }

  bool same_allocation(const Source& s, const DynAllocMemoryLocation* allocation) {
    return malloc_source(s) == allocation->call() && s.context == allocation->context();
  }

  bool once(ar::CallBase* allocation) {
    if (!allocation || !_ctx.concurrent_env.function_executes_at_most_once(
                           allocation->code()->function_or_null())) return false;
    auto* block = allocation->parent();
    std::vector< ar::BasicBlock* > pending(block->successor_begin(), block->successor_end());
    std::unordered_set< ar::BasicBlock* > seen;
    while (!pending.empty()) {
      if (!spend()) return false;
      auto* next = pending.back(); pending.pop_back();
      if (next == block) return false;
      if (!seen.insert(next).second) continue;
      pending.insert(pending.end(), next->successor_begin(), next->successor_end());
    }
    return true;
  }

  ar::Statement* owner_statement(ar::Statement* s, CallContext* context,
                                  ar::Code* owner, CallContext* owner_context) {
    for (unsigned depth = 0; depth < 32 && spend(); ++depth) {
      if (s->code() == owner) return context == owner_context ? s : nullptr;
      if (!context || context->empty()) return nullptr;
      s = context->call(); context = context->parent();
    }
    return nullptr;
  }

  bool scalar_initializer(ar::CallBase* call, CallContext* caller,
                            const DynAllocMemoryLocation* allocation) {
    auto* function = pthread_initialization_callee(call);
    if (!function || !function->is_definition() || function->num_parameters() != call->num_arguments() ||
        !function->type()->return_type()->is_void() ||
        std::distance(function->body()->begin(), function->body()->end()) != 1) return false;
    for (unsigned i = 0; i < call->num_arguments(); ++i)
      if (function->type()->param_type(i) != call->argument(i)->type()) return false;
    auto* frame = _ctx.call_context_factory->get_context(caller, call);
    bool wrote = false;
    for (auto* block : *function->body()) for (auto* s : *block) {
      if (!spend()) return false;
      if (auto* store = dyn_cast< ar::Store >(s)) {
        auto address = source(store->pointer(), frame);
        if (!same_allocation(address, allocation) || address.offset < 0 ||
            (store->value()->type()->is_pointer() && !isa< ar::NullConstant >(store->value())) ||
            !store->value()->type()->is_scalar()) return false;
        auto* count = dyn_cast< ar::IntegerConstant >(allocation->call()->argument(0));
        auto width = _ctx.bundle->data_layout().store_size_in_bytes(store->value()->type());
        if (!count || address.offset + width > count->value().to_z_number()) return false;
        wrote = true;
      } else if (auto* nested = dyn_cast< ar::CallBase >(s)) {
        auto* target = pthread_initialization_callee(nested);
        if (!target || target->intrinsic_id() != ar::Intrinsic::IkosNonDet ||
            nested->num_arguments() != 0 || !nested->has_result() ||
            !nested->result()->type()->is_integer()) return false;
      } else if (auto* ret = dyn_cast< ar::ReturnValue >(s)) {
        if (ret->has_operand()) return false;
      } else if (isa< ar::Load >(s)) {
        return false;
      } else if (auto* cast = dyn_cast< ar::UnaryOperation >(s)) {
        if (cast->operand()->type()->is_pointer() != cast->result()->type()->is_pointer()) return false;
        if (cast->result()->type()->is_pointer() && cast->op() != ar::UnaryOperation::Bitcast) return false;
      } else if (!isa< ar::PointerShift >(s) && !isa< ar::Assignment >(s) &&
                 !isa< ar::BinaryOperation >(s)) {
        return false;
      }
      if (s->result_or_null()) {
        for (auto it = function->param_begin(); it != function->param_end(); ++it)
          if (*it == s->result_or_null()) return false;
      }
    }
    return wrote;
  }

  bool prefix_private(const DynAllocMemoryLocation* allocation,
                        ar::Store* publication, const AccessRecord& init) {
    auto* begin = allocation->call();
    auto* anchor = owner_statement(init.stmt, init.call_context, begin->code(), allocation->context());
    if (!anchor || begin->parent() != publication->parent() || anchor->parent() != begin->parent() ||
        !handle_statement_dominates(begin, anchor) || !handle_statement_dominates(anchor, publication)) return false;
    bool started = false;
    for (auto* s : *begin->parent()) {
      if (!spend()) return false;
      if (s == begin) { started = true; continue; }
      if (!started) continue;
      if (s == publication) return true;
      if (auto* call = dyn_cast< ar::CallBase >(s)) {
        bool mentions = false;
        for (auto it = call->op_begin(); it != call->op_end(); ++it)
          if ((*it)->type()->is_pointer()) mentions |= same_allocation(source(*it, allocation->context()), allocation);
        if (mentions && !scalar_initializer(call, allocation->context(), allocation)) return false;
        continue;
      }
      for (auto it = s->op_begin(); it != s->op_end(); ++it) {
        if (!(*it)->type()->is_pointer()) continue;
        auto operand = source(*it, allocation->context());
        if (!same_allocation(operand, allocation)) continue;
        if (auto* store = dyn_cast< ar::Store >(s)) {
          if (*it != store->pointer() ||
              (store->value()->type()->is_pointer() && !isa< ar::NullConstant >(store->value()))) return false;
        } else if (!isa< ar::PointerShift >(s) && !isa< ar::Assignment >(s) &&
                   !isa< ar::UnaryOperation >(s)) return false;
        if (auto* cast = dyn_cast< ar::UnaryOperation >(s))
          if (cast->op() != ar::UnaryOperation::Bitcast || !cast->result()->type()->is_pointer()) return false;
        if (auto* result = s->result_or_null())
          if (result->type()->is_pointer() &&
              !same_allocation(source(result, allocation->context()), allocation)) return false;
      }
    }
    return false;
  }

  const AccessRecord* record(ar::Statement* s, CallContext* context) {
    const AccessRecord* result = nullptr;
    for (const auto& access : _accesses) {
      if (!spend()) return nullptr;
      if (access.stmt != s || access.call_context != context) continue;
      if (result) return nullptr; // No optimistic selection among observations.
      result = &access;
    }
    return result;
  }

  bool initialized_slot(const AccessRecord& write, const AccessRecord& slot) {
    auto* anchor = owner_statement(write.stmt, write.call_context,
        _ctx.bundle->function_or_null("main")->body(), _ctx.call_context_factory->get_empty());
    if (!anchor) return false;
    // Every possible create in this first subset is directly after the seed.
    for (auto it = _ctx.bundle->function_begin(); it != _ctx.bundle->function_end(); ++it) {
      auto* code = (*it)->body_or_null();
      if (!code) continue;
      for (auto* block : *code) for (auto* s : *block) {
        if (!spend()) return false;
        auto* call = dyn_cast< ar::CallBase >(s);
        auto* target = call ? pthread_initialization_callee(call) : nullptr;
        if (!target || (target->name() != "pthread_create" &&
                        target->intrinsic_id() != ar::Intrinsic::PthreadCreate)) continue;
        if (call->parent() != anchor->parent() || !handle_statement_dominates(anchor, call)) return false;
      }
    }
    return write.base == slot.base && write.offset == slot.offset && write.width == slot.width;
  }

  bool slot_sources(const AccessRecord& slot, const DynAllocMemoryLocation* allocation) {
    bool initial = false;
    auto offset = slot.offset.singleton();
    if (!offset || slot.pts.size() != 1) return false;
    auto* container = dyn_cast< DynAllocMemoryLocation >(*slot.pts.begin());
    auto* container_callee = container ? pthread_initialization_callee(container->call()) : nullptr;
    if (!container_callee || !container_callee->is_declaration() ||
        container_callee->intrinsic_id() != ar::Intrinsic::LibcMalloc ||
        !once(container->call()) || container->call() == allocation->call()) return false;
    for (const auto& write : _accesses) {
      if (!spend()) return false;
      if (write.kind != AccessKind::Write || write.pts.is_bottom() || write.pts.meet(slot.pts).is_empty()) continue;
      if (!write.offset.is_top() && write.width &&
          (write.offset.ub().to_z_number() + write.width <= offset->to_z_number() ||
           offset->to_z_number() + slot.width <= write.offset.lb().to_z_number())) continue;
      auto* store = dyn_cast< ar::Store >(write.stmt);
      if (!store || write.base != slot.base || write.offset != slot.offset || write.width != slot.width ||
          store->is_atomic() || !store->value()->type()->is_pointer()) return false;
      auto rhs = source(store->value(), write.call_context);
      auto* from = malloc_source(rhs);
      if (!(rhs.root && isa< ar::NullConstant >(rhs.root)) &&
          (!from || rhs.offset != 0 || !once(from))) return false;
      if (write.thread_id == "main" && !same_allocation(rhs, allocation) && initialized_slot(write, slot)) initial = true;
    }
    return initial;
  }

public:
  InitPublication(Context& ctx, const std::vector< AccessRecord >& accesses)
      : _ctx(ctx), _accesses(accesses) {
    if (ctx.opts.entry_points.size() != 1 || (*ctx.opts.entry_points.begin())->name() != "main" ||
        !ctx.opts.no_init_globals.empty() || ctx.bundle->global_or_null("ar.global_ctors") ||
        !ctx.concurrent_env.unmodeled_effects().empty() ||
        !ctx.concurrent_env.unmodeled_thread_creations().empty()) return;
    // Allocation generation must remain live. Omitted effects cannot establish it.
    for (auto it = ctx.bundle->function_begin(); it != ctx.bundle->function_end(); ++it) {
      auto* code = (*it)->body_or_null();
      if (!code) continue;
      for (auto* block : *code) for (auto* s : *block) {
        if (!spend()) return;
        if (auto* cast = dyn_cast< ar::UnaryOperation >(s))
          if (cast->op() == ar::UnaryOperation::PtrToUI || cast->op() == ar::UnaryOperation::UIToPtr ||
              cast->op() == ar::UnaryOperation::PtrToSI || cast->op() == ar::UnaryOperation::SIToPtr) return;
        auto* call = dyn_cast< ar::CallBase >(s);
        auto* target = call ? pthread_initialization_callee(call) : nullptr;
        if (call && (!target || target->intrinsic_id() == ar::Intrinsic::LibcFree ||
                     target->intrinsic_id() == ar::Intrinsic::LibcRealloc)) return;
      }
    }
    _available = true;
  }

  bool ordered(const AccessRecord& init, const AccessRecord& reader) {
    if (!_available || init.kind != AccessKind::Write || init.is_atomic || !init.locks.empty() ||
        !init.pts.is_set() || init.pts.size() != 1 || !reader.pts.is_set() || reader.pts.is_empty() ||
        !isa< ar::Store >(init.stmt) || !isa< ar::Load >(reader.stmt)) return false;
    auto* allocation = dyn_cast< DynAllocMemoryLocation >(*init.pts.begin());
    if (!allocation || !once(allocation->call())) return false;
    auto origin = source(cast< ar::Store >(init.stmt)->pointer(), init.call_context);
    if (!same_allocation(origin, allocation)) return false;
    auto read_address = source(cast< ar::Load >(reader.stmt)->operand(), reader.call_context);
    auto* value = dyn_cast_or_null< ar::InternalVariable >(read_address.root);
    auto* load = value ? dyn_cast_or_null< ar::Load >(unique_def(value)) : nullptr;
    auto* observed = load ? record(load, read_address.context) : nullptr;
    if (!observed || observed->is_atomic || !observed->pts.is_set() || observed->pts.size() != 1 ||
        observed->width != _ctx.bundle->data_layout().pointers.bit_width / 8) return false;
    for (const auto& pub : _accesses) {
      if (!spend()) return false;
      auto* store = dyn_cast< ar::Store >(pub.stmt);
      if (!store || pub.is_atomic || pub.call_context != allocation->context() ||
          store->code() != allocation->call()->code() || pub.kind != AccessKind::Write ||
          pub.base != observed->base || pub.offset != observed->offset || pub.width != observed->width ||
          !pub.pts.is_set() || pub.pts.size() != 1) continue;
      auto rhs = source(store->value(), pub.call_context);
      if (rhs.offset != 0 || !same_allocation(rhs, allocation) || !prefix_private(allocation, store, init)) continue;
      for (auto lock : pub.locks) {
        std::uint64_t symbolic_base = 0;
        core::ConcurrentGlobalEnv::SymbolicIndex symbolic_index;
        if (_ctx.concurrent_env.lock_symbolic_index(lock, symbolic_base, symbolic_index)) continue;
        if (std::find(reader.locks.begin(), reader.locks.end(), lock) == reader.locks.end() ||
            std::find(observed->locks.begin(), observed->locks.end(), lock) == observed->locks.end()) continue;
        // A direct full global mutex, not a symbolic or pseudo lock.
        bool released = false, after = false;
        for (auto* s : *store->parent()) {
          if (!spend()) return false;
          if (s == store) { after = true; continue; }
          if (!after) continue;
          auto* call = dyn_cast< ar::CallBase >(s);
          auto* target = call ? pthread_initialization_callee(call) : nullptr;
          if (!target || target->intrinsic_id() != ar::Intrinsic::PthreadMutexUnlock || call->num_arguments() != 1) continue;
          auto address = source(call->argument(0), pub.call_context);
          auto* global = dyn_cast_or_null< ar::GlobalVariable >(address.root);
          if (!global || address.offset != 0) continue;
          auto id = core::IndexableTraits< MemoryLocation* >::index(_ctx.mem_factory->get_global(global));
          if (offset_lock_key(id, core::ZNumber(0)) == lock) { released = true; break; }
        }
        if (released && slot_sources(pub, allocation)) return true;
      }
    }
    return false;
  }
};

} // namespace analyzer
} // namespace ikos
