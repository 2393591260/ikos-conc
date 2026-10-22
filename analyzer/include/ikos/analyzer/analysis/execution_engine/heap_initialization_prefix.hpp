// Copyright and license: see numerical.hpp.
#pragma once

#include <ikos/analyzer/analysis/execution_engine/stable_publication.hpp>
#include <ikos/analyzer/analysis/execution_engine/allocation_continuation.hpp>

namespace ikos {
namespace analyzer {

struct HeapInitializationDescription {
  struct Write {
    ar::Store* statement;
    core::ZNumber offset;
    core::ZNumber width;
    bool helper;
  };
  ar::Statement* completion = nullptr;
  ar::CallBase* helper = nullptr;
  std::map< ar::Value*, core::ZNumber > aliases;
  std::set< ar::GlobalVariable* > relays;
  std::vector< Write > writes;
};

// This certifies initialized fields, never a unique or private allocation site.
class HeapInitializationPrefix {
  using Addresses = std::map< ar::Value*, core::ZNumber >;
  Context const& _ctx;
  core::ZNumber _extent;
  core::ZNumber _field;
  core::ZNumber _width;
  unsigned _budget = 4096;
  HeapInitializationDescription _description;
  bool _in_helper = false;

  bool spend() { return _budget != 0 && (--_budget, true); }

  bool definition(ar::Statement* statement) {
    auto* result = dyn_cast_or_null< ar::InternalVariable >(statement->result_or_null());
    if (!result) return !statement->result_or_null();
    auto* function = statement->code()->function_or_null();
    if (!function || unique_def(result) != statement) return false;
    for (auto it = function->param_begin(); it != function->param_end(); ++it)
      if (*it == result) return false;
    return true;
  }

  bool address(ar::Statement* statement, Addresses& addresses) {
    if (auto* copy = dyn_cast< ar::Assignment >(statement)) {
      auto source = addresses.find(copy->operand());
      if (source == addresses.end()) return false;
      addresses.emplace(copy->result(), source->second);
      return true;
    }
    if (auto* cast = dyn_cast< ar::UnaryOperation >(statement)) {
      auto source = addresses.find(cast->operand());
      if (cast->op() != ar::UnaryOperation::Bitcast || source == addresses.end() ||
          !cast->result()->type()->is_pointer()) return false;
      addresses.emplace(cast->result(), source->second);
      return true;
    }
    if (auto* shift = dyn_cast< ar::PointerShift >(statement)) {
      auto source = addresses.find(shift->pointer());
      if (source == addresses.end()) return false;
      auto offset = source->second;
      for (auto it = shift->term_begin(); it != shift->term_end(); ++it) {
        auto* index = dyn_cast< ar::IntegerConstant >((*it).second);
        if (!index) return false;
        offset += (*it).first.to_z_number() * index->value().to_z_number();
      }
      if (offset < 0 || offset > _extent) return false;
      addresses.emplace(shift->result(), offset);
      return true;
    }
    return false;
  }

  bool scalar(ar::Statement* statement) {
    if (!isa< ar::Assignment >(statement) && !isa< ar::UnaryOperation >(statement) &&
        !isa< ar::BinaryOperation >(statement)) return false;
    auto* result = statement->result_or_null();
    if (!result || (!result->type()->is_integer() && !result->type()->is_float())) return false;
    for (auto it = statement->op_begin(); it != statement->op_end(); ++it)
      if (!(*it)->type()->is_integer() && !(*it)->type()->is_float()) return false;
    return true;
  }

  bool nondet(ar::CallBase* call) {
    auto* target = dyn_cast< ar::FunctionPointerConstant >(call->called());
    return target && target->function()->is_declaration() &&
        target->function()->intrinsic_id() == ar::Intrinsic::IkosNonDet &&
        call->num_arguments() == 0 && call->has_result() && call->result()->type()->is_integer();
  }

  bool store(ar::Store* statement, const Addresses& addresses, bool& initialized) {
    auto target = addresses.find(statement->pointer());
    if (target == addresses.end() || statement->is_volatile() || statement->is_atomic()) return false;
    auto* type = statement->value()->type();
    auto size = _ctx.bundle->data_layout().store_size_in_bytes(type);
    const auto& offset = target->second;
    if (offset < 0 || offset + size > _extent) return false;
    if (type->is_pointer()) {
      if (!isa< ar::NullConstant >(statement->value()) || size != _width) return false;
    } else if (!type->is_integer() && !type->is_float()) return false;
    if (offset < _field + _width && _field < offset + size)
      initialized = offset == _field && size == _width && isa< ar::NullConstant >(statement->value());
    _description.writes.push_back({statement, offset, size, _in_helper});
    return true;
  }

  bool helper(ar::CallBase* call, const Addresses& caller) {
    auto* target = dyn_cast< ar::FunctionPointerConstant >(call->called());
    auto* function = target ? target->function() : nullptr;
    auto* source = call->has_frontend() ? llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >()) : nullptr;
    auto* original = source ? llvm::dyn_cast< llvm::Function >(source->getCalledOperand()->stripPointerCasts()) : nullptr;
    if (!function || !function->is_definition() || function->type()->is_var_arg() ||
        !function->type()->return_type()->is_void() || function->num_parameters() != call->num_arguments() ||
        !function->has_frontend() || !original || original->isDeclaration() || original->isVarArg() ||
        function->frontend< llvm::Value >() != original || !original->getReturnType()->isVoidTy() ||
        source->getFunctionType() != original->getFunctionType() || source->arg_size() != call->num_arguments() ||
        source->getCallingConv() != llvm::CallingConv::C || original->getCallingConv() != llvm::CallingConv::C) return false;
    Addresses addresses;
    for (unsigned i = 0; i < call->num_arguments(); ++i) {
      auto* formal = function->param(i);
      if (formal->type() != call->argument(i)->type()) return false;
      for (auto attribute : {llvm::Attribute::ByVal, llvm::Attribute::StructRet,
                             llvm::Attribute::InAlloca, llvm::Attribute::Preallocated})
        if (original->hasParamAttribute(i, attribute) || source->paramHasAttr(i, attribute)) return false;
      if (formal->type()->is_pointer()) {
        auto origin = caller.find(call->argument(i));
        if (!addresses.empty() || origin == caller.end() || origin->second != 0) return false;
        addresses.emplace(formal, core::ZNumber(0));
      } else if (!formal->type()->is_integer() && !formal->type()->is_float()) return false;
    }
    if (addresses.size() != 1) return false;
    auto* code = function->body();
    for (auto* block : *code) if (block != code->entry_block()) return false;
    if (code->entry_block()->successor_begin() != code->entry_block()->successor_end()) return false;
    _in_helper = true;
    bool initialized = false, returned = false;
    for (auto* statement : *code->entry_block()) {
      if (!spend() || returned || !definition(statement)) return false;
      if (auto* call = dyn_cast< ar::CallBase >(statement)) {
        if (!nondet(call)) return false;
      } else if (auto* write = dyn_cast< ar::Store >(statement)) {
        if (!store(write, addresses, initialized)) return false;
      } else if (auto* ret = dyn_cast< ar::ReturnValue >(statement)) {
        if (ret->has_operand()) return false;
        returned = true;
      } else if (!address(statement, addresses) && !scalar(statement)) return false;
    }
    _in_helper = false;
    return returned && initialized;
  }

  bool startup(ar::CallBase* allocation, ar::Statement* completion) {
    if (_ctx.opts.entry_points.size() != 1 || !_ctx.opts.no_init_globals.empty()) return false;
    auto* main = _ctx.opts.entry_points.front();
    if (main->name() != "main" || main->num_parameters() != 0 || !main->has_frontend() ||
        allocation->code() != main->body() || !main->frontend< llvm::Function >()->use_empty()) return false;
    std::set< ar::BasicBlock* > seen;
    std::vector< ar::BasicBlock* > pending(allocation->parent()->successor_begin(), allocation->parent()->successor_end());
    while (!pending.empty()) {
      if (!spend()) return false;
      auto* block = pending.back(); pending.pop_back();
      if (block == allocation->parent()) return false;
      if (!seen.insert(block).second) continue;
      pending.insert(pending.end(), block->successor_begin(), block->successor_end());
    }
    // This also closes creates in helpers and alternate paths before the seed.
    for (auto it = _ctx.bundle->function_begin(); it != _ctx.bundle->function_end(); ++it) {
      if (!(*it)->is_definition()) continue;
      for (auto* block : *(*it)->body()) for (auto* statement : *block) {
        if (!spend()) return false;
        auto* call = dyn_cast< ar::CallBase >(statement);
        if (!call) continue;
        auto* target = dyn_cast< ar::FunctionPointerConstant >(call->called());
        auto* source = call->has_frontend() ? llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >()) : nullptr;
        auto* original = source ? llvm::dyn_cast< llvm::Function >(source->getCalledOperand()->stripPointerCasts()) : nullptr;
        if ((target && (target->function()->name() == "pthread_create" ||
                        target->function()->intrinsic_id() == ar::Intrinsic::PthreadCreate)) ||
            (original && original->getName() == "pthread_create"))
          if (call->code() != main->body() || !handle_statement_dominates(completion, call)) return false;
      }
    }
    return true;
  }

public:
  HeapInitializationPrefix(const Context& ctx, core::ZNumber extent, core::ZNumber field)
      : _ctx(ctx), _extent(std::move(extent)), _field(std::move(field)),
        _width(ctx.bundle->data_layout().pointers.bit_width / 8) {}

  bool prove(ar::CallBase* allocation, HeapInitializationDescription* description = nullptr) {
    auto* target = dyn_cast< ar::FunctionPointerConstant >(allocation->called());
    auto* source = allocation->has_frontend() ? llvm::dyn_cast< llvm::CallBase >(allocation->frontend< llvm::Value >()) : nullptr;
    auto* function = source ? llvm::dyn_cast< llvm::Function >(source->getCalledOperand()->stripPointerCasts()) : nullptr;
    if (!target || !target->function()->is_declaration() || target->function()->intrinsic_id() != ar::Intrinsic::LibcMalloc ||
        !function || !function->isDeclaration() || function->getName() != "malloc" || function->isVarArg() ||
        source->arg_size() != 1 || function->arg_size() != 1 ||
        !source->getType()->isPointerTy() || llvm::cast< llvm::PointerType >(source->getType())->getAddressSpace() != 0 ||
        !source->getArgOperand(0)->getType()->isIntegerTy(_ctx.bundle->data_layout().pointers.bit_width) ||
        source->getFunctionType() != function->getFunctionType() ||
        source->getCallingConv() != llvm::CallingConv::C || function->getCallingConv() != llvm::CallingConv::C ||
        _extent <= 0 || _field < 0 || _field + _width > _extent || !definition(allocation) ||
        _ctx.bundle->global_or_null("ar.global_ctors") || _ctx.bundle->global_or_null("ar.global_dtors") ||
        !mutex_effect_pointer_representations_closed(*function->getParent())) return false;
    Addresses addresses{{allocation->result(), core::ZNumber(0)}};
    std::map< ar::GlobalVariable*, core::ZNumber > relays;
    bool started = false;
    auto* block = allocation->parent();
    ar::Comparison* guard = nullptr;
    std::set< ar::BasicBlock* > visited;
    while (block && visited.insert(block).second) {
      for (auto* statement : *block) {
        if (!spend()) return false;
        if (statement == allocation) { started = true; continue; }
        if (!started || statement == guard) continue;
        if (!definition(statement)) return false;
        if (auto* call = dyn_cast< ar::CallBase >(statement)) {
          if (nondet(call)) continue;
          if (!helper(call, addresses)) return false;
          if (!relays.empty() && !startup(allocation, call)) return false;
          _description.completion = call;
          _description.helper = call;
          _description.aliases = addresses;
          for (const auto& relay : relays) _description.relays.insert(relay.first);
          if (description) *description = std::move(_description);
          return true;
        }
        if (auto* write = dyn_cast< ar::Store >(statement)) {
          bool initialized = false;
          if (addresses.count(write->pointer())) {
            if (!store(write, addresses, initialized)) return false;
            if (initialized) {
              if (!relays.empty() && !startup(allocation, write)) return false;
              _description.completion = write;
              _description.aliases = addresses;
              for (const auto& relay : relays) _description.relays.insert(relay.first);
              if (description) *description = std::move(_description);
              return true;
            }
            continue;
          }
          auto* global = dyn_cast< ar::GlobalVariable >(write->pointer());
          auto value = addresses.find(write->value());
          if (!global || !global->is_definition() || global->is_thread_local() || write->is_atomic() || write->is_volatile() ||
              !global->type()->pointee()->is_pointer() || !write->value()->type()->is_pointer() ||
              value == addresses.end() || value->second != 0 ||
              _ctx.bundle->data_layout().store_size_in_bytes(write->value()->type()) != _width) return false;
          relays[global] = value->second;
        } else if (auto* load = dyn_cast< ar::Load >(statement)) {
          auto* global = dyn_cast< ar::GlobalVariable >(load->operand());
          auto relay = relays.find(global);
          if (!global || relay == relays.end() || !load->result()->type()->is_pointer() || load->is_atomic() || load->is_volatile() ||
              _ctx.bundle->data_layout().store_size_in_bytes(load->result()->type()) != _width) return false;
          addresses.emplace(load->result(), relay->second);
        } else if (!address(statement, addresses) && !scalar(statement)) return false;
      }
    block = heap_initialization_successor(block, guard);
    }
    return false;
  }
};

inline bool prove_heap_initialization_prefix(ar::CallBase* allocation, const core::ZNumber& field,
                                             const Context& ctx,
                                             HeapInitializationDescription* description = nullptr) {
  if (!allocation->code()->is_function_body() || allocation->num_arguments() != 1) return false;
  auto* size = dyn_cast< ar::IntegerConstant >(allocation->argument(0));
  if (!size) return false;
  return HeapInitializationPrefix(ctx, size->value().to_z_number(), field).prove(allocation, description);
}

} // namespace analyzer
} // namespace ikos
