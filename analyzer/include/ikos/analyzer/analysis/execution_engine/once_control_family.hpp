// Copyright and license: see numerical.hpp.
#pragma once

#include <cstdlib>
#include <map>
#include <set>
#include <vector>
#include <llvm/ADT/Triple.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/IntrinsicInst.h>
#include <ikos/analyzer/analysis/source_call_binding.hpp>

namespace ikos { namespace analyzer {

// Complete immutable source/AR use closure. No actual choice, nonnull fact or
// callback completion is implied by a MAY set. Cyclic pointer definitions and
// escaped/address-arithmetic slots deliberately remain unsupported.
struct OnceControlFamily {
  bool valid = false;
  std::set< ar::GlobalVariable* > controls;
  std::set< ar::GlobalVariable* > query_controls;
  std::map< ar::CallBase*, std::set< ar::GlobalVariable* > > calls;
  std::set< ar::Value* > slots;
  std::set< ar::Statement* > statements;
};

namespace once_control_detail {
class Closure {
  using Objects = std::set< ar::GlobalVariable* >;
  ar::CallBase* query;
  ar::Bundle* bundle;
  llvm::Module* module = nullptr;
  llvm::Function* main = nullptr;
  unsigned remaining = 16384;
  bool ok = true;
  OnceControlFamily result;
  std::map< llvm::Value*, std::vector< ar::Statement* > > imported;
  std::map< llvm::GlobalVariable*, ar::GlobalVariable* > globals;
  std::map< ar::Value*, std::vector< ar::Statement* > > definitions;
  std::vector< ar::Statement* > all;
  std::map< llvm::Value*, Objects > native_may;
  std::map< ar::Value*, Objects > ar_may;
  std::set< llvm::Value* > native_active, seen;
  std::set< ar::Value* > ar_active;
  std::map< llvm::Value*, ar::Value* > slots;
  std::set< llvm::Value* > slot_active;
  std::map< ar::Value*, std::vector< ar::Value* > > slot_values;
  std::vector< llvm::Value* > pending;

  bool spend() { if (!remaining) { ok = false; return false; } --remaining; return ok; }
  Objects bad() { ok = false; return {}; }
  bool pointer(llvm::Type* t) const {
    auto* p = llvm::dyn_cast< llvm::PointerType >(t);
    return p && !p->isOpaque() && p->getAddressSpace() == 0 &&
           p->getPointerElementType()->isIntegerTy(32);
  }
  bool ar_pointer(ar::Type* t) const {
    auto* p = dyn_cast< ar::PointerType >(t);
    auto* i = p ? dyn_cast< ar::IntegerType >(p->pointee()) : nullptr;
    return i && i->bit_width() == 32;
  }
  ar::Statement* one(llvm::Value* n) {
    auto it = imported.find(n);
    return it != imported.end() && it->second.size() == 1 ? it->second.front() : nullptr;
  }
  bool bound_value(ar::Value* a, llvm::Value* n) const {
    auto* variable = dyn_cast< ar::Variable >(a);
    return variable && variable->has_frontend() && variable->frontend< llvm::Value >() == n;
  }
  std::set< llvm::BasicBlock* > source_predecessor(ar::BasicBlock* start) {
    std::set< ar::BasicBlock* > visited;
    std::set< llvm::BasicBlock* > anchors;
    std::vector< ar::BasicBlock* > todo{start};
    while (!todo.empty()) {
      auto* block = todo.back(); todo.pop_back();
      if (!visited.insert(block).second) continue;
      if (!spend() || visited.size() > 32) { ok = false; return {}; }
      if (block->has_frontend()) {
        auto* native = llvm::dyn_cast< llvm::BasicBlock >(block->frontend< llvm::Value >());
        if (!native) { ok = false; return {}; }
        anchors.insert(native);
      } else for (auto it = block->predecessor_begin(); it != block->predecessor_end(); ++it) todo.push_back(*it);
    }
    return anchors;
  }
  bool zero_control(llvm::GlobalVariable* n, ar::GlobalVariable*& a) {
    auto it = globals.find(n);
    if (!n || it == globals.end() || !n->getValueType()->isIntegerTy(32) ||
        !n->hasInitializer() || n->isThreadLocal() || n->isConstant() ||
        n->isExternallyInitialized() || n->getAddressSpace() != 0) return false;
    auto* z = llvm::dyn_cast< llvm::ConstantInt >(n->getInitializer());
    a = it->second;
    if (!z || !z->isZero() || !a->is_definition() || a->is_thread_local() ||
        a->alignment() < 4 || !ar_pointer(a->type()) ||
        bundle->data_layout().alloc_size_in_bytes(a->type()->pointee()) != 4) return false;
    unsigned count = 0;
    for (auto* b : *a->initializer()) for (auto* s : *b) {
      auto* st = dyn_cast< ar::Store >(s);
      auto* value = st ? dyn_cast< ar::IntegerConstant >(st->value()) : nullptr;
      if (!st || st->pointer() != a || !value || value->value().to_z_number() != 0 ||
          !value->type()->is_integer() ||
          cast< ar::IntegerType >(value->type())->bit_width() != 32) return false;
      ++count;
    }
    return count == 1;
  }

  // Only exact typed addresses are slot addresses. Main owns every access and
  // its unique frame remains live throughout the physical once profile.
  ar::Value* slot(llvm::Value* n) {
    if (slots.count(n)) return slots.at(n);
    if (!spend() || !slot_active.insert(n).second) { ok = false; return nullptr; }
    ar::Value* a = nullptr;
    llvm::Type* value_type = nullptr;
    llvm::AllocaInst* allocation = llvm::dyn_cast< llvm::AllocaInst >(n);
    if (auto* g = llvm::dyn_cast< llvm::GlobalVariable >(n)) {
      if (!globals.count(g) || !g->hasInitializer() || g->isThreadLocal() ||
          g->isExternallyInitialized() || g->isConstant() || g->getAddressSpace() != 0 ||
          !globals.at(g)->is_definition()) { ok = false; return nullptr; }
      a = globals.at(g); value_type = g->getValueType();
      if (!pointer(value_type) || native_sources(g->getInitializer()).empty()) return nullptr;
      auto* ag = cast< ar::GlobalVariable >(a);
      unsigned count = 0;
      for (auto* b : *ag->initializer()) for (auto* s : *b) {
        auto* st = dyn_cast< ar::Store >(s);
        if (!st || st->pointer() != a || !ar_pointer(st->value()->type())) { ok = false; return nullptr; }
        slot_values[a].push_back(st->value()); ++count;
      }
      if (count != 1) { ok = false; return nullptr; }
    } else if (allocation) {
      auto* count = llvm::dyn_cast< llvm::ConstantInt >(allocation->getArraySize());
      auto* actual = dyn_cast_or_null< ar::Allocate >(one(allocation));
      if (allocation->getFunction() != main || allocation->getParent() != &main->getEntryBlock() ||
          allocation->getAddressSpace() != 0 || !count || !count->isOne() || !actual ||
          !pointer(allocation->getAllocatedType()) ||
          !ar_pointer(actual->allocated_type()) ||
          !dyn_cast< ar::IntegerConstant >(actual->array_size()) ||
          cast< ar::IntegerConstant >(actual->array_size())->value().to_z_number() != 1) {
        ok = false; return nullptr;
      }
      a = actual->result(); value_type = allocation->getAllocatedType();
    } else { ok = false; return nullptr; }
    if (!pointer(value_type)) { ok = false; return nullptr; }
    std::vector< llvm::LoadInst* > loads;
    std::vector< llvm::StoreInst* > stores;
    for (auto* u : n->users()) {
      if (!spend()) return nullptr;
      if (llvm::isa< llvm::DbgInfoIntrinsic >(u)) continue;
      if (auto* l = llvm::dyn_cast< llvm::LoadInst >(u)) {
        auto* actual = dyn_cast_or_null< ar::Load >(one(l));
        if (l->getFunction() != main || l->getPointerOperand() != n ||
            l->getType() != value_type || l->isAtomic() || l->isVolatile() || !actual ||
            actual->operand() != a || !ar_pointer(actual->result()->type()) ||
            actual->is_atomic() || actual->is_volatile()) { ok = false; return nullptr; }
        loads.push_back(l); result.statements.insert(actual);
      } else if (auto* st = llvm::dyn_cast< llvm::StoreInst >(u)) {
        auto* actual = dyn_cast_or_null< ar::Store >(one(st));
        if (st->getFunction() != main || st->getPointerOperand() != n ||
            st->getValueOperand()->getType() != value_type || st->isAtomic() || st->isVolatile() ||
            !actual || actual->pointer() != a || !ar_pointer(actual->value()->type()) ||
            actual->is_atomic() || actual->is_volatile() ||
            native_sources(st->getValueOperand()).empty()) { ok = false; return nullptr; }
        stores.push_back(st); slot_values[a].push_back(actual->value()); result.statements.insert(actual);
      } else { ok = false; return nullptr; }
    }
    if (allocation) {
      llvm::DominatorTree dt(*main);
      for (auto* l : loads) {
        bool initialized = false;
        for (auto* st : stores) initialized |= dt.dominates(st, l);
        if (!initialized) { ok = false; return nullptr; }
      }
    }
    slots.emplace(n, a); result.slots.insert(a); slot_active.erase(n);
    for (auto* l : loads) pending.push_back(l);
    return a;
  }

  Objects native_sources(llvm::Value* n) {
    if (native_may.count(n)) return native_may.at(n);
    if (!spend() || !n || !pointer(n->getType()) || !native_active.insert(n).second) return bad();
    Objects objects;
    auto add = [&](llvm::Value* v) {
      auto values = native_sources(v); objects.insert(values.begin(), values.end());
    };
    if (auto* g = llvm::dyn_cast< llvm::GlobalVariable >(n)) {
      ar::GlobalVariable* a = nullptr;
      if (!zero_control(g, a)) return bad();
      objects.insert(a); result.controls.insert(a);
    } else if (auto* l = llvm::dyn_cast< llvm::LoadInst >(n)) {
      auto* address = l->getPointerOperand();
      if (!slot(address)) return bad();
      if (auto* g = llvm::dyn_cast< llvm::GlobalVariable >(address)) add(g->getInitializer());
      for (auto* u : address->users())
        if (auto* st = llvm::dyn_cast< llvm::StoreInst >(u)) add(st->getValueOperand());
    } else if (auto* phi = llvm::dyn_cast< llvm::PHINode >(n)) {
      for (auto& v : phi->incoming_values()) add(v.get());
    } else if (auto* select = llvm::dyn_cast< llvm::SelectInst >(n)) {
      add(select->getTrueValue()); add(select->getFalseValue());
    } else if (auto* bitcast = llvm::dyn_cast< llvm::BitCastOperator >(n)) add(bitcast->getOperand(0));
    else return bad();
    if (!ok || objects.empty() || result.controls.size() > 8) return bad();
    native_active.erase(n); native_may.emplace(n, objects); pending.push_back(n);
    return objects;
  }

  Objects ar_sources(ar::Value* value) {
    if (ar_may.count(value)) return ar_may.at(value);
    if (!spend() || !value || !ar_pointer(value->type()) || !ar_active.insert(value).second) return bad();
    Objects objects;
    auto add = [&](ar::Value* v) { auto values = ar_sources(v); objects.insert(values.begin(), values.end()); };
    if (auto* global = dyn_cast< ar::GlobalVariable >(value)) {
      if (!result.controls.count(global)) return bad();
      objects.insert(global);
    } else {
      auto it = definitions.find(value);
      if (it == definitions.end() || it->second.empty()) return bad();
      for (auto* s : it->second) {
        if (auto* a = dyn_cast< ar::Assignment >(s)) add(a->operand());
        else if (auto* u = dyn_cast< ar::UnaryOperation >(s)) {
          if (u->op() != ar::UnaryOperation::Bitcast) return bad();
          add(u->operand());
        } else if (auto* l = dyn_cast< ar::Load >(s)) {
          if (!result.slots.count(l->operand()) || l->is_atomic() || l->is_volatile()) return bad();
          for (auto* v : slot_values[l->operand()]) add(v);
        } else return bad();
        result.statements.insert(s);
      }
    }
    if (!ok || objects.empty()) return bad();
    ar_active.erase(value); ar_may.emplace(value, objects); return objects;
  }

public:
  explicit Closure(ar::CallBase* q) : query(q), bundle(q->bundle()) {}
  OnceControlFamily run() {
    auto* source = query->has_frontend() ? llvm::dyn_cast< llvm::CallInst >(query->frontend< llvm::Value >()) : nullptr;
    if (!source || source->arg_size() != 2) return {};
    module = source->getModule(); main = module->getFunction("main");
    llvm::Triple triple(module->getTargetTriple());
    const auto& dl = module->getDataLayout();
    const auto bits = bundle->data_layout().pointers.bit_width;
    if (!main || main->isDeclaration() || !main->arg_empty() || !main->use_empty() ||
        !triple.isOSLinux() || triple.getEnvironment() != llvm::Triple::GNU ||
        !((bits == 32 && triple.getArch() == llvm::Triple::x86) ||
          (bits == 64 && triple.getArch() == llvm::Triple::x86_64)) ||
        dl.getPointerSizeInBits(0) != bits || dl.getIndexSizeInBits(0) != bits || !dl.isLittleEndian() ||
        !module->getModuleInlineAsm().empty() || !module->alias_empty() ||
        module->getGlobalVariable("llvm.global_ctors") || module->getGlobalVariable("llvm.global_dtors")) return {};
    for (auto it = bundle->global_begin(); it != bundle->global_end(); ++it)
      if ((*it)->has_frontend()) globals.emplace((*it)->frontend< llvm::GlobalVariable >(), *it);
    for (auto it = bundle->function_begin(); it != bundle->function_end(); ++it) {
      if (!(*it)->is_definition()) continue;
      for (auto* b : *(*it)->body()) for (auto* s : *b) {
        if (!spend()) return {};
        all.push_back(s);
        if (s->has_result()) definitions[s->result()].push_back(s);
        if (s->has_frontend()) imported[s->frontend< llvm::Value >()].push_back(s);
      }
    }
    // An externally visible pointer slot must not be modified by an opaque
    // declaration that can name it without receiving its address. Keep this
    // deliberately small effect inventory instead of assuming address-use
    // closure is a whole-program unknown-call proof.
    for (auto* statement : all) {
      auto* call = dyn_cast< ar::CallBase >(statement);
      if (!call) continue;
      auto* callee = native_handle_callee(call);
      if (!callee) return {};
      if (!callee->is_declaration()) continue;
      const auto& name = callee->name();
      bool nondet = callee->intrinsic_id() == ar::Intrinsic::IkosNonDet;
      if (name == "__VERIFIER_nondet_int" || name == "__VERIFIER_nondet_uint") {
        auto* ret = dyn_cast< ar::IntegerType >(callee->type()->return_type());
        auto* source_call = call->has_frontend() ? llvm::dyn_cast< llvm::CallInst >(call->frontend< llvm::Value >()) : nullptr;
        auto* declaration = source_call ? llvm::dyn_cast< llvm::Function >(
            source_call->getCalledOperand()->stripPointerCasts()) : nullptr;
        nondet = ret && ret->bit_width() == 32 && callee->num_parameters() == 0 &&
            call->num_arguments() == 0 && call->has_result() &&
            call->result()->type() == ret && declaration && declaration->isDeclaration() &&
            declaration->arg_empty() && declaration->getReturnType()->isIntegerTy(32) &&
            source_call_binding(source_call, call) == call;
      }
      const auto intrinsic = callee->intrinsic_id();
      const bool pthread_effect = intrinsic == ar::Intrinsic::PthreadCreate ||
          intrinsic == ar::Intrinsic::PthreadJoin ||
          intrinsic == ar::Intrinsic::PthreadMutexLock ||
          intrinsic == ar::Intrinsic::PthreadMutexUnlock;
      if (!nondet && !pthread_effect && name != "pthread_once" && name != "pthread_create" &&
          name != "pthread_join" && name != "pthread_mutex_lock" && name != "pthread_mutex_unlock") return {};
    }
    // A syntactically unconnected integer/unknown pointer write could still
    // overwrite an externally visible slot. Accept only named whole-object
    // store addresses in this small profile; no cast-int or graph write frame.
    for (auto& function : *module) for (auto& block : function) for (auto& instruction : block) {
      if (!spend()) return {};
      if (auto* store = llvm::dyn_cast< llvm::StoreInst >(&instruction)) {
        auto* address = store->getPointerOperand();
        if (!llvm::isa< llvm::GlobalVariable >(address) && !llvm::isa< llvm::AllocaInst >(address)) return {};
        auto* actual = dyn_cast_or_null< ar::Store >(one(store));
        if (!actual || !bound_value(actual->pointer(), address)) return {};
      } else if (instruction.mayWriteToMemory() && !llvm::isa< llvm::CallBase >(&instruction)) return {};
    }
    native_sources(source->getArgOperand(0));
    for (std::size_t i = 0; ok && i < pending.size(); ++i) {
      auto* value = pending[i];
      if (!seen.insert(value).second) continue;
      native_sources(value);
      for (auto* user : value->users()) {
        if (!spend()) return {};
        if (llvm::isa< llvm::DbgInfoIntrinsic >(user)) continue;
        if (llvm::isa< llvm::BitCastOperator >(user) || llvm::isa< llvm::PHINode >(user) ||
            llvm::isa< llvm::SelectInst >(user)) { native_sources(user); continue; }
        if (auto* global = llvm::dyn_cast< llvm::GlobalVariable >(user)) {
          if (global->getInitializer() != value || !slot(global)) return {};
          continue;
        }
        if (auto* store = llvm::dyn_cast< llvm::StoreInst >(user)) {
          if (store->getValueOperand() != value || !slot(store->getPointerOperand())) return {};
          continue;
        }
        auto* call = llvm::dyn_cast< llvm::CallInst >(user);
        auto* api = call ? call->getCalledFunction() : nullptr;
        auto* actual = dyn_cast_or_null< ar::CallBase >(one(call));
        if (!api || !api->isDeclaration() || api->getName() != "pthread_once" ||
            call->arg_size() != 2 || call->getArgOperand(0) != value || !actual ||
            actual->num_arguments() != 2 || actual->code()->function_or_null() == nullptr) return {};
        result.calls[actual] = native_sources(value); result.statements.insert(actual);
      }
    }
    if (!ok || !result.calls.count(query)) return {};
    // Validate imported definitions (including every lowered PHI arm) separately.
    for (const auto& entry : native_may) {
      if (llvm::isa< llvm::GlobalVariable >(entry.first)) continue;
      auto it = imported.find(entry.first);
      if (it == imported.end() || it->second.empty()) return {};
      for (auto* s : it->second) {
        if (!s->has_result() || !bound_value(s->result(), entry.first) || ar_sources(s->result()) != entry.second) return {};
        if (auto* bitcast = llvm::dyn_cast< llvm::BitCastOperator >(entry.first)) {
          auto* actual = dyn_cast< ar::UnaryOperation >(s);
          if (!actual || actual->op() != ar::UnaryOperation::Bitcast ||
              !bound_value(actual->operand(), bitcast->getOperand(0))) return {};
        }
      }
    }
    for (const auto& item : slots) {
      auto* native = item.first;
      if (auto* g = llvm::dyn_cast< llvm::GlobalVariable >(native)) {
        const auto& values = slot_values[item.second];
        if (values.empty() || !bound_value(values.front(), g->getInitializer())) return {};
      }
      for (auto* user : native->users()) {
        auto* store = llvm::dyn_cast< llvm::StoreInst >(user);
        if (!store) continue;
        auto* actual = dyn_cast_or_null< ar::Store >(one(store));
        if (!actual || !bound_value(actual->value(), store->getValueOperand()) ||
            ar_sources(actual->value()) != native_sources(store->getValueOperand())) return {};
      }
    }
    for (const auto& entry : native_may) {
      auto* phi = llvm::dyn_cast< llvm::PHINode >(entry.first);
      if (!phi) continue;
      auto it = imported.find(phi);
      if (it == imported.end() || it->second.size() != phi->getNumIncomingValues()) return {};
      std::set< unsigned > matched;
      for (auto* actual : it->second) {
        ar::Value* operand = nullptr;
        if (auto* copy = dyn_cast< ar::Assignment >(actual)) operand = copy->operand();
        else if (auto* cast = dyn_cast< ar::UnaryOperation >(actual))
          if (cast->op() == ar::UnaryOperation::Bitcast) operand = cast->operand();
        if (!operand || !bound_value(actual->result(), phi)) return {};
        auto anchors = source_predecessor(actual->parent());
        bool found = false;
        for (unsigned j = 0; j < phi->getNumIncomingValues(); ++j)
          if (!matched.count(j) && anchors == std::set< llvm::BasicBlock* >{phi->getIncomingBlock(j)} &&
              bound_value(operand, phi->getIncomingValue(j))) { matched.insert(j); found = true; break; }
        if (!found) return {};
      }
    }
    for (const auto& entry : result.calls)
      if (ar_sources(entry.first->argument(0)) != entry.second ||
          !bound_value(entry.first->argument(0), entry.first->frontend< llvm::CallBase >()->getArgOperand(0))) return {};
    // No retained AR-only alias, raw access or hidden worker slot use is covered.
    std::set< ar::Value* > tainted;
    for (auto* g : result.controls) tainted.insert(g);
    for (const auto& entry : ar_may) tainted.insert(entry.first);
    bool changed = true;
    while (changed && ok) {
      changed = false;
      for (auto* s : all) {
        if (!spend()) return {};
        bool affected = false;
        for (auto it = s->op_begin(); it != s->op_end(); ++it)
          affected |= tainted.count(*it) || result.slots.count(*it);
        if (!affected) continue;
        if (auto* st = dyn_cast< ar::Store >(s)) {
          if (!result.slots.count(st->pointer()) || !tainted.count(st->value()) ||
              st->is_atomic() || st->is_volatile() || st->code()->function()->name() != "main") return {};
        } else if (auto* load = dyn_cast< ar::Load >(s)) {
          if (!result.slots.count(load->operand()) || load->is_atomic() || load->is_volatile() ||
              load->code()->function()->name() != "main") return {};
          if (ar_sources(load->result()).empty()) return {};
          changed |= tainted.insert(load->result()).second;
        } else if (auto* a = dyn_cast< ar::Assignment >(s)) {
          if (!tainted.count(a->operand()) || ar_sources(a->result()).empty()) return {};
          changed |= tainted.insert(a->result()).second;
        } else if (auto* u = dyn_cast< ar::UnaryOperation >(s)) {
          if (u->op() != ar::UnaryOperation::Bitcast || !tainted.count(u->operand()) ||
              ar_sources(u->result()).empty()) return {};
          changed |= tainted.insert(u->result()).second;
        } else if (auto* call = dyn_cast< ar::CallBase >(s)) {
          if (!result.calls.count(call) || !tainted.count(call->argument(0))) return {};
          for (unsigned j = 1; j < call->num_arguments(); ++j)
            if (tainted.count(call->argument(j)) || result.slots.count(call->argument(j))) return {};
        } else return {};
        result.statements.insert(s);
      }
    }
    if (!ok) return {};
    result.query_controls = result.calls.at(query); result.valid = true;
    return result;
  }
};
} // namespace once_control_detail
inline OnceControlFamily once_control_family(ar::CallBase* call) {
  if (std::getenv("IKOS_DISABLE_FINITE_ONCE_CONTROLS") != nullptr) return {};
  return once_control_detail::Closure(call).run();
}
} } // namespace ikos::analyzer
