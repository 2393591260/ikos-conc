#pragma once

#include <z3++.h>
#include "unroll.hpp"

namespace ikos { namespace analyzer { namespace bmc {

// Logical function identities are not C addresses. Only concrete function/null
// sources enter this encoding; byte/pointer-int conversions stay unsupported.
class FunctionValues {
public:
  using Key = std::pair< ar::InternalVariable*, std::size_t >;
  using Cache = std::map< Key, z3::expr >;
  Cache caches[3]; // startup, A, B; separate dynamic thread instances.

private:
  z3::context& _ctx;
  z3::solver& _solver;
  const std::vector< BmcEvent >* _paths[3];
  const std::vector< z3::expr >& _clock;
  std::vector< ar::Function* > _functions;
  std::function< bool(const BmcEvent&, const void*) > _library_alias;

  z3::expr token(ar::Function* function) const {
    auto i = std::find(_functions.begin(), _functions.end(), function);
    ikos_assert(i != _functions.end());
    return _ctx.int_val(static_cast< unsigned >(i - _functions.begin() + 1));
  }

public:
  FunctionValues(z3::context& ctx, z3::solver& solver, ar::Bundle* bundle,
                 const std::vector< BmcEvent >& startup,
                 const std::vector< BmcEvent >& a, const std::vector< BmcEvent >& b,
                 const std::vector< z3::expr >& clock,
                 std::function< bool(const BmcEvent&, const void*) > library_alias)
      : _ctx(ctx), _solver(solver), _paths{&startup, &a, &b}, _clock(clock),
        _library_alias(std::move(library_alias)) {
    for (auto i = bundle->function_begin(); i != bundle->function_end(); ++i) _functions.push_back(*i);
    std::sort(_functions.begin(), _functions.end(), [](auto* a, auto* b) {return a->name() < b->name();});
  }

  bool encode(ar::Value* value, unsigned path_id, std::size_t before,
              z3::expr& out, unsigned depth = 0) {
    if (depth >= 64 || !value->type()->is_pointer()) return false;
    if (auto* constant = core::dyn_cast< ar::FunctionPointerConstant >(value)) {
      out = token(constant->function()); return true;
    }
    if (core::isa< ar::NullConstant >(value)) {out = _ctx.int_val(0); return true;}
    auto* variable = core::dyn_cast< ar::InternalVariable >(value);
    if (variable == nullptr) return false;
    const auto& path = *_paths[path_id];
    std::size_t index = 0;
    auto* definition = reaching_definition(variable, path, before, index);
    if (definition == nullptr) {
      auto* actual = formal_argument(variable, path, before, index);
      return actual != nullptr && encode(actual, path_id, index, out, depth + 1);
    }
    auto key = Key{variable, index};
    auto cached = caches[path_id].find(key);
    if (cached != caches[path_id].end()) {out = cached->second; return true;}
    if (auto* assignment = core::dyn_cast< ar::Assignment >(definition)) {
      if (!encode(assignment->operand(), path_id, index, out, depth + 1)) return false;
    } else if (auto* cast = core::dyn_cast< ar::UnaryOperation >(definition)) {
      if (cast->op() != ar::UnaryOperation::Bitcast ||
          !encode(cast->operand(), path_id, index, out, depth + 1)) return false;
    } else if (core::isa< ar::Load >(definition)) {
      const auto& access = path[index];
      if (access.base == 0 || access.var_id != 0 || access.const_off != 0 ||
          _library_alias(access, &path)) return false;
      auto* global = reinterpret_cast< ar::GlobalVariable* >(access.base);
      if (!global->is_definition() || global->is_thread_local() ||
          !global->type()->pointee()->is_pointer() || !global->has_frontend() ||
          access.width != global->bundle()->data_layout().pointers.bit_width / 8) return false;
      out = _ctx.int_const(("function_" + std::to_string(path_id) + "_" + std::to_string(index)).c_str());
      caches[path_id].emplace(key, out);
      z3::expr initial(_ctx);
      bool initialized = false;
      auto* source = global->frontend< llvm::GlobalVariable >();
      if (source->hasInitializer()) {
        auto* constant = source->getInitializer();
        if (constant->isNullValue()) {initial = _ctx.int_val(0); initialized = true;}
        else if (auto* f = llvm::dyn_cast< llvm::Function >(constant->stripPointerCasts())) {
          if (auto* target = global->bundle()->function_or_null(f->getName().str())) {
            initial = token(target); initialized = true;
          }
        }
      }
      auto matches = [&](const BmcEvent& event) {
        return event.kind == EvKind::Write && event.base == access.base && !abstract_initializer(event.stmt);
      };
      auto stored = [&](const BmcEvent& event, unsigned id, std::size_t j, z3::expr& result) {
        return event.var_id == 0 && event.const_off == 0 && event.width == access.width &&
               encode(core::cast< ar::Store >(event.stmt)->value(), id, j, result, depth + 1);
      };
      const auto& startup = *_paths[0];
      for (std::size_t j = 0, end = path_id == 0 ? index : startup.size(); j < end; ++j) {
        if (!matches(startup[j])) continue;
        if (!stored(startup[j], 0, j, initial)) return false;
        initialized = true;
      }
      if (path_id == 0) {
        if (!initialized) return false;
        _solver.add(out == initial);
      } else {
        struct Write {z3::expr time, value;};
        std::vector< Write > writes;
        for (unsigned id : {1u, 2u}) {
          const auto& events = *_paths[id];
          for (std::size_t j = 0; j < events.size(); ++j) {
            if (!matches(events[j]) || (id == path_id && j >= index)) continue;
            z3::expr encoded(_ctx);
            if (!stored(events[j], id, j, encoded)) return false;
            writes.push_back({_clock[(id == 1 ? 0 : _paths[1]->size()) + j], encoded});
          }
        }
        const auto time = _clock[(path_id == 1 ? 0 : _paths[1]->size()) + index];
        auto choice = _ctx.bool_val(false), first = _ctx.bool_val(true);
        for (std::size_t j = 0; j < writes.size(); ++j) {
          auto latest = writes[j].time < time;
          first = first && time < writes[j].time;
          for (std::size_t k = 0; k < writes.size(); ++k)
            if (k != j) latest = latest && !(writes[j].time < writes[k].time && writes[k].time < time);
          choice = choice || (latest && out == writes[j].value);
        }
        if (initialized) choice = choice || (first && out == initial);
        if (writes.empty() && !initialized) return false;
        _solver.add(choice);
      }
      return true;
    } else return false;
    caches[path_id].emplace(key, out);
    return true;
  }

  bool bind(ar::CallBase* call, ar::Function* target, unsigned id, std::size_t before) {
    z3::expr function(_ctx);
    if (!compatible_indirect_callee(call, target) || !encode(call->called(), id, before, function)) return false;
    _solver.add(function == token(target));
    return true;
  }

  JsonDict identity(const z3::expr& value, z3::model& model) const {
    auto number = model.eval(value, true).get_numeral_uint();
    if (number == 0) return JsonDict{{"kind", "null"}};
    ikos_assert(number <= _functions.size());
    return JsonDict{{"kind", "function"}, {"id", _functions[number - 1]->name()}};
  }
};

} } }
