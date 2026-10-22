// Copyright and license: see numerical.hpp.
#pragma once

#include <functional>
#include <map>
#include <set>
#include <llvm/ADT/Triple.h>
#include <llvm/IR/IntrinsicInst.h>
#include <ikos/analyzer/analysis/context.hpp>
#include <ikos/analyzer/analysis/source_program_facts.hpp>
#include <ikos/analyzer/analysis/execution_engine/native_thread_handles.hpp>

namespace ikos {
namespace analyzer {

// A finite source/effect closure, not an evaluator. All alternatives remain MAY
// writers; the seal only permits exclusion within one unchanged once control.
inline std::vector< ar::Function* > closed_once_callbacks(Context& ctx,
                                                          ar::CallBase* query) {
  std::vector< ar::Function* > result;
  if (ctx.opts.entry_points.size() != 1 ||
      ctx.opts.entry_points.front()->name() != "main" ||
      !ctx.opts.no_init_globals.empty() || !query->has_frontend()) return result;
  auto* source = llvm::dyn_cast< llvm::CallInst >(query->frontend< llvm::Value >());
  if (!source || source->arg_size() != 2) return result;
  auto* module = source->getModule();
  llvm::Triple triple(module->getTargetTriple());
  auto bits = ctx.bundle->data_layout().pointers.bit_width;
  if ((bits != 32 && bits != 64) || module->getDataLayout().getPointerSizeInBits() != bits ||
      (triple.getArch() != llvm::Triple::x86 && triple.getArch() != llvm::Triple::x86_64) ||
      !triple.isOSLinux() || triple.getEnvironment() != llvm::Triple::GNU ||
      !module->getModuleInlineAsm().empty() || !module->alias_empty() ||
      module->getGlobalVariable("llvm.global_ctors") ||
      module->getGlobalVariable("llvm.global_dtors")) return result;
  auto& facts = source_program_facts(ctx);
  if (auto* cached = facts.once_callbacks(query)) return *cached;
  // Only this source closure is cached; the caller still performs every transfer.
  auto compute = [&]() {
  unsigned budget = 16384;
  auto spend = [&]() { return budget && --budget; };
  if (!facts.index(budget) || !facts.memory_calls().unique) return result;
  budget -= facts.statement_count();
  const auto& originals = facts.memory_calls().statements;
  const auto& functions = facts.functions();
  const auto& globals = facts.globals();
  auto original = [&](llvm::Value* v) -> ar::Statement* {
    auto it = originals.find(v); return it == originals.end() ? nullptr : it->second;
  };
  auto peel = [](ar::Value* v) {
    for (unsigned n = 0; n < 16; ++n) {
      auto* iv = dyn_cast< ar::InternalVariable >(v);
      auto* un = iv ? dyn_cast_or_null< ar::UnaryOperation >(unique_def(iv)) : nullptr;
      if (!un || un->op() != ar::UnaryOperation::Bitcast) break;
      v = un->operand();
    }
    return v;
  };
  auto function = [&](llvm::Value* v) -> ar::Function* {
    auto* f = llvm::dyn_cast< llvm::Function >(v->stripPointerCasts());
    auto it = functions.find(f);
    if (!f || f->getAddressSpace() != 0 || it == functions.end() || !it->second->is_definition() ||
        f->isVarArg() || !f->arg_empty() || !f->getReturnType()->isVoidTy() ||
        f->getCallingConv() != llvm::CallingConv::C ||
        it->second->num_parameters() != 0 || !it->second->type()->return_type()->is_void()) return nullptr;
    return it->second;
  };
  auto same_function = [&](ar::Value* a, llvm::Value* n) {
    auto* f = dyn_cast< ar::FunctionPointerConstant >(peel(a));
    return f && f->function() == function(n);
  };
  auto global = [&](llvm::GlobalVariable* g) {
    return g && globals.count(g) && g->hasInitializer() && !g->isThreadLocal() &&
        !g->isExternallyInitialized() && g->getAddressSpace() == 0 &&
        globals.at(g)->is_definition() && !globals.at(g)->is_thread_local();
  };
  std::map< llvm::GlobalVariable*, std::set< ar::Function* > > slots;
  auto slot_targets = [&](llvm::GlobalVariable* slot) {
    if (slots.count(slot)) return true;
    if (!global(slot) || !slot->getValueType()->isPointerTy()) return false;
    auto* initial = function(slot->getInitializer());
    if (!initial) return false;
    auto* a = globals.at(slot);
    unsigned count = 0;
    for (auto* b : *a->initializer()) for (auto* s : *b) {
      if (auto* un = dyn_cast< ar::UnaryOperation >(s)) {
        if (un->op() != ar::UnaryOperation::Bitcast ||
            !same_function(un->operand(), slot->getInitializer())) return false;
        continue;
      }
      auto* store = dyn_cast< ar::Store >(s);
      if (!store || peel(store->pointer()) != a ||
          !same_function(store->value(), slot->getInitializer())) return false;
      ++count;
    }
    if (count != 1) return false;
    std::set< ar::Function* > targets{initial};
    for (auto* use : slot->users()) {
      if (!spend()) return false;
      if (auto* load = llvm::dyn_cast< llvm::LoadInst >(use)) {
        auto* actual = dyn_cast_or_null< ar::Load >(original(load));
        if (load->getPointerOperand() != slot || load->isAtomic() || load->isVolatile() ||
            load->getType() != slot->getValueType() || !actual ||
            actual->is_atomic() || actual->is_volatile() || peel(actual->operand()) != a)
          return false;
      } else if (auto* store = llvm::dyn_cast< llvm::StoreInst >(use)) {
        auto* actual = dyn_cast_or_null< ar::Store >(original(store));
        auto* f = function(store->getValueOperand());
        if (store->getPointerOperand() != slot || store->isAtomic() || store->isVolatile() ||
            store->getValueOperand()->getType() != slot->getValueType() || !f || !actual ||
            actual->is_atomic() || actual->is_volatile() || peel(actual->pointer()) != a ||
            !same_function(actual->value(), store->getValueOperand())) return false;
        targets.insert(f);
      } else return false;
    }
    slots.emplace(slot, std::move(targets)); return true;
  };
  auto callbacks = [&](llvm::CallBase* call, ar::CallBase* actual,
                       std::set< ar::Function* >& targets) {
    auto* operand = call->getArgOperand(1)->stripPointerCasts();
    if (auto* f = function(operand)) {
      if (!same_function(actual->argument(1), operand)) return false;
      targets.insert(f); return true;
    }
    auto* load = llvm::dyn_cast< llvm::LoadInst >(operand);
    auto* slot = load ? llvm::dyn_cast< llvm::GlobalVariable >(load->getPointerOperand()) : nullptr;
    auto* iv = dyn_cast< ar::InternalVariable >(peel(actual->argument(1)));
    if (!slot || !iv || unique_def(iv) != original(load) || !slot_targets(slot)) return false;
    targets.insert(slots.at(slot).begin(), slots.at(slot).end()); return true;
  };
  auto* control = llvm::dyn_cast< llvm::GlobalVariable >(source->getArgOperand(0)->stripPointerCasts());
  if (!global(control) || !control->getValueType()->isIntegerTy(32)) return result;
  auto* zero = llvm::dyn_cast< llvm::ConstantInt >(control->getInitializer());
  if (!zero || !zero->isZero()) return result;
  unsigned count = 0;
  auto* ar_control = globals.at(control);
  for (auto* b : *ar_control->initializer()) for (auto* s : *b) {
    auto* store = dyn_cast< ar::Store >(s);
    auto* z = store ? dyn_cast< ar::IntegerConstant >(store->value()) : nullptr;
    if (!store || peel(store->pointer()) != ar_control || !z ||
        z->value().to_z_number() != 0 || !z->type()->is_integer() ||
        cast< ar::IntegerType >(z->type())->bit_width() != 32) return result;
    ++count;
  }
  if (count != 1 || peel(query->argument(0)) != ar_control) return result;
  std::set< ar::Function* > all, selected;
  std::vector< llvm::Value* > pending{control}; std::set< llvm::Value* > seen;
  bool found = false;
  while (!pending.empty()) {
    auto* value = pending.back(); pending.pop_back();
    if (!seen.insert(value).second) continue;
    for (auto* use : value->users()) {
      if (!spend()) return result;
      if (llvm::isa< llvm::BitCastOperator >(use)) { pending.push_back(use); continue; }
      auto* call = llvm::dyn_cast< llvm::CallInst >(use);
      auto* f = call ? call->getCalledFunction() : nullptr;
      auto* actual = call ? dyn_cast_or_null< ar::CallBase >(original(call)) : nullptr;
      auto* target = actual ? native_handle_callee(actual) : nullptr;
      auto* control_type = f && f->arg_size() == 2
          ? llvm::dyn_cast< llvm::PointerType >(f->getArg(0)->getType()) : nullptr;
      auto* callback_type = f && f->arg_size() == 2
          ? llvm::dyn_cast< llvm::PointerType >(f->getArg(1)->getType()) : nullptr;
      auto* signature = callback_type
          ? llvm::dyn_cast< llvm::FunctionType >(callback_type->getPointerElementType()) : nullptr;
      if (!control_type || control_type->getAddressSpace() != 0 ||
          !control_type->getPointerElementType()->isIntegerTy(32) ||
          !callback_type || callback_type->getAddressSpace() != 0 || !signature ||
          !signature->getReturnType()->isVoidTy() || signature->getNumParams() != 0 ||
          signature->isVarArg()) return result;
      auto* ar_ctrl = target && target->num_parameters() == 2
          ? dyn_cast< ar::PointerType >(target->type()->param_type(0)) : nullptr;
      auto* ar_fn = target && target->num_parameters() == 2
          ? dyn_cast< ar::PointerType >(target->type()->param_type(1)) : nullptr;
      auto* ar_int = ar_ctrl ? dyn_cast< ar::IntegerType >(ar_ctrl->pointee()) : nullptr;
      auto* ar_signature = ar_fn ? dyn_cast< ar::FunctionType >(ar_fn->pointee()) : nullptr;
      auto* ar_return = target ? dyn_cast< ar::IntegerType >(target->type()->return_type()) : nullptr;
      if (!ar_int || ar_int->bit_width() != 32 || !ar_signature ||
          ar_signature->num_parameters() != 0 || ar_signature->is_var_arg() ||
          !ar_signature->return_type()->is_void() || !ar_return ||
          ar_return->bit_width() != 32 || target->is_var_arg()) return result;
      if (!f || !f->isDeclaration() || f->getName() != "pthread_once" || f->isVarArg() ||
          f->arg_size() != 2 || call->arg_size() != 2 || call->getArgOperand(0) != value ||
          call->getFunctionType() != f->getFunctionType() || !call->getType()->isIntegerTy(32) ||
          call->getCallingConv() != llvm::CallingConv::C || f->getCallingConv() != llvm::CallingConv::C ||
          call->hasOperandBundles() || !target || !target->has_frontend() ||
          target->frontend< llvm::Value >() != f || actual->num_arguments() != 2 ||
          peel(actual->argument(0)) != ar_control) return result;
      std::set< ar::Function* > targets;
      if (!callbacks(call, actual, targets)) return result;
      all.insert(targets.begin(), targets.end());
      if (actual == query) { found = true; selected = targets; }
    }
  }
  if (!found || selected.empty()) return result;
  // A loaded callback address stays inside once calls; its slot accesses remain
  // ordinary application accesses and can still race with concurrent stores.
  for (const auto& slot : slots) for (auto* use : slot.first->users()) {
    auto* load = llvm::dyn_cast< llvm::LoadInst >(use);
    if (!load) continue;
    std::vector< llvm::Value* > todo{load}; std::set< llvm::Value* > values;
    while (!todo.empty()) {
      auto* v = todo.back(); todo.pop_back();
      if (!values.insert(v).second) continue;
      for (auto* u : v->users()) {
        if (!spend()) return result;
        if (llvm::isa< llvm::BitCastOperator >(u)) { todo.push_back(u); continue; }
        auto* c = llvm::dyn_cast< llvm::CallInst >(u);
        auto* f = c ? c->getCalledFunction() : nullptr;
        if (!f || !f->isDeclaration() || f->getName() != "pthread_once" ||
            c->arg_size() != 2 || c->getArgOperand(1) != v) return result;
      }
    }
  }
  // Ordinary direct calls retain their actual thread root. Other callback
  // address uses must remain inside the validated slots or once API.
  for (auto* target : all) {
    auto* native = target->frontend< llvm::Function >();
    std::vector< llvm::Value* > todo{native}; std::set< llvm::Value* > values;
    while (!todo.empty()) {
      auto* v = todo.back(); todo.pop_back();
      if (!values.insert(v).second) continue;
      for (auto* u : v->users()) {
        if (!spend()) return result;
        if (llvm::isa< llvm::BitCastOperator >(u)) { todo.push_back(u); continue; }
        if (auto* g = llvm::dyn_cast< llvm::GlobalVariable >(u)) {
          if (!slots.count(g) || g->getInitializer() != v) return result;
          continue;
        }
        if (auto* store = llvm::dyn_cast< llvm::StoreInst >(u)) {
          auto* g = llvm::dyn_cast< llvm::GlobalVariable >(store->getPointerOperand());
          if (!g || !slots.count(g) || store->getValueOperand() != v) return result;
          continue;
        }
        auto* call = llvm::dyn_cast< llvm::CallInst >(u);
        if (!call) return result;
        if (call->getCalledOperand() == v && call->getFunctionType() == native->getFunctionType() &&
            call->getCallingConv() == llvm::CallingConv::C) continue;
        auto* f = call->getCalledFunction();
        if (!f || !f->isDeclaration() || f->getName() != "pthread_once" ||
            call->arg_size() != 2 || call->getArgOperand(1) != v) return result;
      }
    }
  }
  std::set< llvm::Function* > active, done;
  std::function< bool(llvm::Function*) > effects = [&](llvm::Function* f) {
    if (done.count(f)) return true;
    if (!active.insert(f).second || active.size() > 32) return false;
    for (auto& b : *f) for (auto& i : b) {
      if (!spend() || llvm::isa< llvm::InvokeInst >(i) || llvm::isa< llvm::CallBrInst >(i) ||
          llvm::isa< llvm::IndirectBrInst >(i) || llvm::isa< llvm::ResumeInst >(i)) return false;
      auto* call = llvm::dyn_cast< llvm::CallBase >(&i);
      if (!call || llvm::isa< llvm::DbgInfoIntrinsic >(call)) continue;
      auto* child = call->getCalledFunction();
      auto* actual = dyn_cast_or_null< ar::CallBase >(original(call));
      auto* target = actual ? native_handle_callee(actual) : nullptr;
      if (!child || !target || !target->has_frontend() || target->frontend< llvm::Value >() != child ||
          call->hasOperandBundles() || call->getFunctionType() != child->getFunctionType() ||
          call->getCallingConv() != llvm::CallingConv::C || child->getCallingConv() != llvm::CallingConv::C)
        return false;
      if (!child->isDeclaration()) { if (!effects(child)) return false; }
      else if (!(call->use_empty() && call->arg_empty() && call->getMetadata("ikos.freeze_uninit") &&
                 target->intrinsic_id() == ar::Intrinsic::IkosNonDet &&
                 (child->getName() == "__ikos_nondet_int" || child->getName() == "__ikos_nondet_int64")))
        return false;
    }
    active.erase(f); done.insert(f); return true;
  };
  for (auto* f : all)
    if (!effects(f->frontend< llvm::Function >())) return result;
  result.assign(selected.begin(), selected.end()); return result;
  };
  auto targets = compute();
  facts.remember_once_callbacks(query, targets);
  return targets;
}

} // namespace analyzer
} // namespace ikos
