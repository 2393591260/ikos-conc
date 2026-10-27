// Copyright and license: see data_race.hpp.
#pragma once

#include <ikos/ar/semantic/pthread.hpp>
#include <boost/optional.hpp>
#include <ikos/analyzer/analysis/execution_engine/pthread_initialization.hpp>
#include <ikos/analyzer/checker/data_race.hpp>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace ikos {
namespace analyzer {

// Exhaustive constant-control interpretation, not a bounded safety query.
// Any unknown branch/effect, invalid handle, or exhausted budget discards all
// certificates. Thread data stay abstract; only private handles are
// interpreted.
class ConstantThreadCompletion {
  struct Value {
    enum Kind { Unknown, Integer, Pointer, Handle } kind = Unknown;
    core::ZNumber number{0};
    ar::LocalVariable* base = nullptr;
    std::size_t instance = 0;
  };
  struct Instance {
    ar::Function* function;
    bool joined = false;
  };
  using Names = std::unordered_set< std::string >;
  using Slot = std::pair< ar::LocalVariable*, core::ZNumber >;
  Context& _ctx;
  std::unordered_map< ar::Value*, Value > _values;
  std::map< Slot, std::size_t > _handles;
  std::unordered_map< ar::LocalVariable*, core::ZNumber > _extents;
  std::vector< Instance > _instances;
  std::map< ar::Statement*, Names > _completed;
  std::unordered_map< ar::Function*, std::unordered_set< std::uint64_t > >
      _sites;
  Names _covered;
  unsigned _budget = 8192;

  Value value(ar::Value* operand) const {
    if (auto* c = dyn_cast< ar::IntegerConstant >(operand)) {
      Value out;
      out.kind = Value::Integer;
      out.number = c->value().to_z_number();
      return out;
    }
    auto it = _values.find(operand);
    return it == _values.end() ? Value{} : it->second;
  }
  static bool in_range(const core::ZNumber& n, ar::IntegerType* t) {
    core::ZNumber lo = t->is_signed()
                           ? -(core::ZNumber(1) << (t->bit_width() - 1))
                           : core::ZNumber(0);
    core::ZNumber hi =
        (core::ZNumber(1) << (t->bit_width() - (t->is_signed() ? 1 : 0))) - 1;
    return n >= lo && n <= hi;
  }
  boost::optional< bool > comparison(ar::Comparison* c) const {
    auto a = value(c->left()), b = value(c->right());
    if (a.kind != Value::Integer || b.kind != Value::Integer)
      return boost::none;
    switch (c->predicate()) {
      case ar::Comparison::SIEQ:
      case ar::Comparison::UIEQ:
        return a.number == b.number;
      case ar::Comparison::SINE:
      case ar::Comparison::UINE:
        return a.number != b.number;
      case ar::Comparison::SILT:
      case ar::Comparison::UILT:
        return a.number < b.number;
      case ar::Comparison::SILE:
      case ar::Comparison::UILE:
        return a.number <= b.number;
      case ar::Comparison::SIGT:
      case ar::Comparison::UIGT:
        return a.number > b.number;
      case ar::Comparison::SIGE:
      case ar::Comparison::UIGE:
        return a.number >= b.number;
      default:
        return boost::none;
    }
  }
  bool slot(ar::Value* pointer, ar::Type* type, Slot& out) const {
    auto p = value(pointer);
    auto* integer = dyn_cast< ar::IntegerType >(type);
    if (p.kind != Value::Pointer || !p.base || !integer ||
        integer->bit_width() != _ctx.bundle->data_layout().pointers.bit_width)
      return false;
    auto it = _extents.find(p.base);
    auto width = _ctx.bundle->data_layout().store_size_in_bytes(type);
    if (it == _extents.end() || p.number < 0 || p.number + width > it->second ||
        p.number % width != 0)
      return false;
    out = {p.base, p.number};
    return true;
  }
  void snapshot(ar::Statement* s) {
    Names complete = _covered;
    for (const auto& i : _instances)
      if (!i.joined)
        complete.erase(i.function->name());
    auto it = _completed.find(s);
    if (it == _completed.end())
      _completed.emplace(s, std::move(complete));
    else
      for (auto n = it->second.begin(); n != it->second.end();) {
        if (!complete.count(*n))
          n = it->second.erase(n);
        else
          ++n;
      }
  }
  bool transfer(ar::Statement* s) {
    if (_budget-- == 0)
      return false;
    snapshot(s);
    if (auto* a = dyn_cast< ar::Assignment >(s)) {
      _values[a->result()] = value(a->operand());
      return true;
    }
    if (auto* a = dyn_cast< ar::Allocate >(s)) {
      auto* local = dyn_cast< ar::LocalVariable >(a->result());
      auto* n = dyn_cast< ar::IntegerConstant >(a->array_size());
      auto* type = a->allocated_type();
      if (auto* array = dyn_cast< ar::ArrayType >(type))
        type = array->element_type();
      auto* integer = dyn_cast< ar::IntegerType >(type);
      if (!local || !n || n->value().to_z_number() != 1 ||
          _extents.count(local) ||
          (local && local->has_alignment()
               ? core::ZNumber(local->alignment())
               : _ctx.bundle->data_layout().abi_alignment(
                     a->allocated_type())) <
              core::ZNumber(_ctx.bundle->data_layout().pointers.bit_width /
                            8) ||
          !integer ||
          integer->bit_width() != _ctx.bundle->data_layout().pointers.bit_width)
        return false;
      Value v;
      v.kind = Value::Pointer;
      v.base = local;
      _values[local] = v;
      _extents[local] =
          _ctx.bundle->data_layout().alloc_size_in_bytes(a->allocated_type());
      return true;
    }
    if (auto* p = dyn_cast< ar::PointerShift >(s)) {
      auto out = value(p->pointer());
      if (out.kind != Value::Pointer)
        return false;
      for (auto i = p->term_begin(); i != p->term_end(); ++i) {
        auto n = value((*i).second);
        if (n.kind != Value::Integer)
          return false;
        out.number += (*i).first.to_z_number() * n.number;
      }
      _values[p->result()] = out;
      return true;
    }
    if (auto* b = dyn_cast< ar::BinaryOperation >(s)) {
      auto a = value(b->left()), c = value(b->right());
      Value out;
      if (a.kind == Value::Handle || c.kind == Value::Handle ||
          a.kind == Value::Pointer || c.kind == Value::Pointer)
        return false;
      auto* type = dyn_cast< ar::IntegerType >(b->result()->type());
      if (!type)
        return false;
      if (a.kind == Value::Integer && c.kind == Value::Integer) {
        switch (b->op()) {
          case ar::BinaryOperation::SAdd:
          case ar::BinaryOperation::UAdd:
            out.number = a.number + c.number;
            break;
          case ar::BinaryOperation::SSub:
          case ar::BinaryOperation::USub:
            out.number = a.number - c.number;
            break;
          case ar::BinaryOperation::SMul:
          case ar::BinaryOperation::UMul:
            out.number = a.number * c.number;
            break;
          default:
            return false;
        }
        if (!in_range(out.number, type))
          return false;
        out.kind = Value::Integer;
      }
      _values[b->result()] = out;
      return true;
    }
    if (auto* u = dyn_cast< ar::UnaryOperation >(s)) {
      if (u->op() == ar::UnaryOperation::SExt ||
          u->op() == ar::UnaryOperation::ZExt) {
        auto a = value(u->operand());
        auto* type = dyn_cast< ar::IntegerType >(u->result()->type());
        if (a.kind != Value::Integer || !type ||
            (u->op() == ar::UnaryOperation::ZExt && a.number < 0) ||
            !in_range(a.number, type))
          return false;
        _values[u->result()] = a;
        return true;
      }
      if (u->op() != ar::UnaryOperation::Bitcast ||
          !((u->operand()->type()->is_pointer() &&
             u->result()->type()->is_pointer()) ||
            (u->operand()->type()->is_integer() &&
             u->result()->type()->is_integer())))
        return false;
      auto a = value(u->operand());
      if (u->operand()->type()->is_pointer() !=
          u->result()->type()->is_pointer())
        return false;
      if (u->result()->type()->is_integer() &&
          cast< ar::IntegerType >(u->operand()->type())->bit_width() !=
              cast< ar::IntegerType >(u->result()->type())->bit_width())
        return false;
      if (a.kind == Value::Integer &&
          !in_range(a.number, cast< ar::IntegerType >(u->result()->type())))
        return false;
      _values[u->result()] = a;
      return true;
    }
    if (auto* c = dyn_cast< ar::Comparison >(s)) {
      auto result = comparison(c);
      return result && *result;
    }
    if (auto* l = dyn_cast< ar::Load >(s)) {
      Value out;
      Slot address;
      if (value(l->operand()).kind == Value::Pointer) {
        if (!slot(l->operand(), l->result()->type(), address))
          return false;
        auto i = _handles.find(address);
        if (i == _handles.end())
          return false;
        out.kind = Value::Handle;
        out.instance = i->second;
      } else if (!isa< ar::GlobalVariable >(l->operand()) ||
                 !l->result()->type()->is_integer())
        return false;
      _values[l->result()] = out;
      return true;
    }
    if (auto* call = dyn_cast< ar::CallBase >(s)) {
      auto* api = pthread_initialization_callee(call);
      if (!api || !api->is_declaration())
        return false;
      if (ar::is_pthread_create(api) || ar::is_pthread_join(api)) {
        auto* ret = dyn_cast< ar::IntegerType >(api->type()->return_type());
        if (!ret || ret->bit_width() != 32 || !ret->is_signed() ||
            api->type()->is_var_arg() ||
            api->num_parameters() != call->num_arguments())
          return false;
      }
      if (ar::is_pthread_create(api) && call->num_arguments() == 4) {
        auto* target =
            dyn_cast< ar::FunctionPointerConstant >(call->argument(2));
        Slot address;
        auto* output = dyn_cast< ar::PointerType >(call->argument(0)->type());
        if (!target || !target->function()->is_definition() || !output ||
            target->function()->num_parameters() != 1 ||
            !target->function()->param(0)->type()->is_pointer() ||
            !target->function()->type()->return_type()->is_pointer() ||
            !isa< ar::NullConstant >(call->argument(1)) ||
            !isa< ar::NullConstant >(call->argument(3)) ||
            !slot(call->argument(0), output->pointee(), address))
          return false;
        _handles[address] = _instances.size();
        _instances.push_back({target->function(), false});
      } else if (ar::is_pthread_join(api) &&
                 call->num_arguments() == 2) {
        auto h = value(call->argument(0));
        if (h.kind != Value::Handle ||
            !isa< ar::NullConstant >(call->argument(1)) ||
            h.instance >= _instances.size() || _instances[h.instance].joined)
          return false;
        _instances[h.instance].joined = true;
      } else if (api->intrinsic_id() != ar::Intrinsic::IkosNonDet ||
                 call->num_arguments() != 0)
        return false;
      if (call->has_result())
        _values[call->result()] = {};
      return true;
    }
    return isa< ar::ReturnValue >(s);
  }
  bool analyze(ar::Function* main) {
    // A private main array cannot be touched by children: no store, escape,
    // other call, or non-null child argument is accepted by transfer().
    // Registered sites outside main (including wrappers/once) cannot be
    // covered.
    for (auto* block : *main->body())
      for (auto* s : *block) {
        auto* call = dyn_cast< ar::CallBase >(s);
        auto* api = call ? pthread_initialization_callee(call) : nullptr;
        if (api && api->is_declaration() && ar::is_pthread_create(api) &&
            call->num_arguments() == 4) {
          auto* child =
              dyn_cast< ar::FunctionPointerConstant >(call->argument(2));
          if (!child || !child->function()->is_definition())
            return false;
          _sites[child->function()].insert(_ctx.concurrent_env.site_id(call));
        }
      }
    for (const auto& f : _sites)
      if (f.second == _ctx.concurrent_env.get_func_instances(f.first->name()))
        _covered.insert(f.first->name());
    if (_covered.empty())
      return false;
    auto* block = main->body()->entry_block();
    while (block) {
      if (_budget-- == 0)
        return false;
      bool returned = false;
      for (auto* s : *block) {
        if (!transfer(s))
          return false;
        if (isa< ar::ReturnValue >(s))
          returned = true;
      }
      if (returned)
        return block->num_successors() == 0;
      if (block->num_successors() == 1) {
        block = *block->successor_begin();
        continue;
      }
      ar::BasicBlock* chosen = nullptr;
      for (auto i = block->successor_begin(); i != block->successor_end();
           ++i) {
        bool guarded = false, admissible = true;
        for (auto* s : **i) {
          auto* c = dyn_cast< ar::Comparison >(s);
          if (!c)
            break;
          guarded = true;
          auto truth = comparison(c);
          if (!truth)
            return false;
          admissible &= *truth;
        }
        if (!guarded)
          return false;
        if (admissible) {
          if (chosen)
            return false;
          chosen = *i;
        }
      }
      if (!chosen)
        return false;
      block = chosen;
    }
    return false;
  }

public:
  explicit ConstantThreadCompletion(Context& ctx) : _ctx(ctx) {
    auto* main = ctx.bundle->function_or_null("main");
    if (!main || !main->is_definition() || ctx.opts.entry_points.size() != 1 ||
        ctx.opts.entry_points.front() != main ||
        ctx.bundle->global_or_null("ar.global_ctors") ||
        !ctx.opts.no_init_globals.empty() ||
        !ctx.concurrent_env.function_executes_at_most_once(main))
      return;
    if (!main->has_frontend())
      return;
    auto* frontend = main->frontend< llvm::Function >();
    if (!frontend)
      return;
    const llvm::Triple triple(frontend->getParent()->getTargetTriple());
    const auto bits = ctx.bundle->data_layout().pointers.bit_width;
    if (!triple.isOSLinux() || triple.getEnvironment() != llvm::Triple::GNU ||
        !((triple.getArch() == llvm::Triple::x86 && bits == 32) ||
          (triple.getArch() == llvm::Triple::x86_64 && bits == 64)))
      return;
    // Reject cancellation/detach and nested joins instead of assuming their
    // failure and ownership behavior in a completion certificate.
    for (auto i = ctx.bundle->function_begin(); i != ctx.bundle->function_end();
         ++i)
      if (auto* code = (*i)->body_or_null())
        for (auto* b : *code)
          for (auto* s : *b)
            if (auto* call = dyn_cast< ar::CallBase >(s)) {
              auto* api = pthread_initialization_callee(call);
              if (api && (api->name() == "pthread_detach" ||
                          api->name() == "pthread_cancel" ||
                          ((*i) != main && ar::is_pthread_join(api))))
                return;
            }
    for (auto i = ctx.bundle->global_begin(); i != ctx.bundle->global_end();
         ++i)
      if (auto* code = (*i)->initializer_or_null())
        for (auto* b : *code)
          for (auto* s : *b)
            if (isa< ar::CallBase >(s))
              return;
    if (!analyze(main))
      _completed.clear();
  }
  Names inactive_before(const AccessRecord& access) const {
    if (access.thread_id != "main" || !access.call_context ||
        !access.call_context->empty())
      return {};
    auto i = _completed.find(access.stmt);
    return i == _completed.end() ? Names{} : i->second;
  }
};

} // namespace analyzer
} // namespace ikos
