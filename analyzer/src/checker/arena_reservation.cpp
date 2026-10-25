// Copyright and license: see data_race.hpp.
#include <ikos/analyzer/checker/arena_reservation.hpp>
#include <ikos/analyzer/checker/binary_token.hpp>

#include <llvm/IR/Dominators.h>
#include <llvm/IR/IntrinsicInst.h>
#include <ikos/analyzer/analysis/execution_engine/native_thread_handles.hpp>
#include <ikos/analyzer/analysis/execution_engine/concurrent_semantics.hpp>
#include <ikos/analyzer/checker/data_race.hpp>
#include <ikos/analyzer/checker/normal_mutex.hpp>
#include <deque>
#include <functional>
#include <map>
#include <set>

namespace ikos {
namespace analyzer {
namespace {
using core::ZNumber;
llvm::Function* callee(llvm::CallBase* c) {
  return llvm::dyn_cast< llvm::Function >(c->getCalledOperand()->stripPointerCasts());
}
bool plain_call(llvm::CallBase* c, llvm::Function* f) {
  if (!f || !llvm::isa< llvm::CallInst >(c) || c->isInlineAsm() ||
      c->hasOperandBundles() || c->getFunctionType() != f->getFunctionType() ||
      c->getCallingConv() != llvm::CallingConv::C ||
      f->getCallingConv() != llvm::CallingConv::C || f->isVarArg() ||
      c->arg_size() != f->arg_size()) return false;
  for (unsigned i = 0; i < c->arg_size(); ++i)
    for (auto a : {llvm::Attribute::ByVal, llvm::Attribute::StructRet,
                   llvm::Attribute::InAlloca, llvm::Attribute::Preallocated})
      if (c->paramHasAttr(i, a) || f->hasParamAttribute(i, a)) return false;
  return true;
}
bool signed32(const ZNumber& n) { return n >= -(ZNumber(1) << 31) && n < (ZNumber(1) << 31); }
bool constant(llvm::Value* v, ZNumber& n) {
  auto* c = llvm::dyn_cast< llvm::ConstantInt >(v);
  if (!c || !c->getType()->isIntegerTy(32)) return false;
  n = ZNumber(c->getSExtValue()); return true;
}
bool follows(llvm::Instruction* from, llvm::Instruction* to) {
  if (from->getParent() == to->getParent() && from->comesBefore(to)) return true;
  std::vector< llvm::BasicBlock* > todo;
  for (auto* b : llvm::successors(from->getParent())) todo.push_back(b);
  std::set< llvm::BasicBlock* > seen;
  while (!todo.empty()) {
    auto* b = todo.back(); todo.pop_back();
    if (b == to->getParent()) return true;
    if (seen.insert(b).second)
      for (auto* next : llvm::successors(b)) todo.push_back(next);
  }
  return false;
}
struct Counter {
  llvm::GlobalVariable* global = nullptr;
  llvm::Function* allocator = nullptr;
  llvm::GlobalVariable* serializer = nullptr;
  bool binary_token = false;
  // Complete reachable counter states; each success reserves [start, end).
  std::vector< std::pair< ZNumber, ZNumber > > slices;
};
struct SerializationOperation {
  llvm::GlobalVariable* source = nullptr;
  bool acquire = false;
  bool token = false;
};
struct Proof {
  llvm::GlobalVariable* arena = nullptr;
  llvm::GlobalVariable* counter = nullptr;
};
} // namespace

class ArenaReservation::Impl {
  Context& ctx;
  SymbolicArenaReservation symbolic;
  const BinaryTokenOwnership& binary_tokens;
  std::map< llvm::Value*, ar::Statement* > originals;
  std::map< llvm::GlobalVariable*, ar::GlobalVariable* > globals;
  std::map< llvm::Function*, ar::Function* > functions;
  std::vector< llvm::Function* > function_order;
  std::vector< Counter > counters;
  std::map< llvm::GlobalVariable*, bool > closed_arenas;
  std::map< llvm::CallBase*, NormalMutex > mutex_calls;
  std::map< llvm::GlobalVariable*, bool > mutex_protocols;
  std::map< llvm::Instruction*, Proof > proofs;
  unsigned budget = 131072;
  bool spend() { if (!budget) return false; --budget; return true; }
  ar::Statement* original(llvm::Value* v) const {
    auto it = originals.find(v); return it == originals.end() ? nullptr : it->second;
  }
  ar::CallBase* call_binding(llvm::CallBase* c) const {
    auto* call = dyn_cast_or_null< ar::CallBase >(original(c));
    auto* target = call ? native_handle_callee(call) : nullptr;
    auto* f = callee(c);
    const bool identity = f && target && (f->isDeclaration()
        ? target->is_declaration()
        : target->is_definition() && target->has_frontend() && target->frontend< llvm::Value >() == f);
    // Library intrinsics have synthetic AR functions; their exact source ABI
    // and effect contract are checked by module_closed, not by a name alias.
    return identity && plain_call(c, f) && call->num_arguments() == c->arg_size() ? call : nullptr;
  }
  NormalMutex mutex_call(llvm::CallBase* c) {
    auto it = mutex_calls.find(c);
    if (it != mutex_calls.end()) return it->second;
    auto p = normal_mutex(ctx, call_binding(c));
    mutex_calls.emplace(c, p); return p;
  }
  SerializationOperation serialization_call(llvm::CallBase* call) {
    auto native = mutex_call(call);
    if (native.source) return {native.source, native.lock, false};
    auto token = binary_tokens.event(call);
    return {token.source, token.acquire, true};
  }
  bool unused_freeze(llvm::CallBase* c) const {
    auto* a = call_binding(c);
    auto* f = a ? native_handle_callee(a) : nullptr;
    auto* source = callee(c);
    return a && f && source->isDeclaration() && f->intrinsic_id() == ar::Intrinsic::IkosNonDet &&
        c->use_empty() && c->arg_size() == 0 && c->getMetadata("ikos.freeze_uninit") &&
        ((source->getName() == "__ikos_nondet_int" && c->getType()->isIntegerTy(32)) ||
         (source->getName() == "__ikos_nondet_int64" && c->getType()->isIntegerTy(64)));
  }
  bool mutex_protocol(llvm::GlobalVariable* g) {
    auto cached = mutex_protocols.find(g);
    if (cached != mutex_protocols.end()) return cached->second;
    mutex_protocols.emplace(g, false);
    std::set< llvm::CallBase* > operations;
    std::vector< llvm::Value* > todo{g}; std::set< llvm::Value* > seen;
    while (!todo.empty()) {
      auto* p = todo.back(); todo.pop_back();
      if (!spend()) return false;
      if (!seen.insert(p).second) continue;
      for (auto* u : p->users()) {
        if (llvm::isa< llvm::BitCastOperator >(u)) {
          if (u->getType()->getPointerAddressSpace() != 0) return false;
          todo.push_back(u); continue;
        }
        auto* c = llvm::dyn_cast< llvm::CallBase >(u);
        if (!c || c->arg_size() != 1 || c->getArgOperand(0) != p || mutex_call(c).source != g) return false;
        operations.insert(c);
      }
    }
    if (operations.empty()) return false;
    std::map< llvm::Function*, bool > touches;
    std::set< llvm::Function* > active;
    std::function< bool(llvm::Function*) > check = [&](llvm::Function* f) {
      if (touches.count(f)) return true;
      if (active.size() == 64 || !active.insert(f).second) return false;
      bool effect = false;
      for (auto& b : *f) for (auto& i : b) {
        if (!spend()) return false;
        auto* c = llvm::dyn_cast< llvm::CallBase >(&i);
        if (!c || llvm::isa< llvm::DbgInfoIntrinsic >(c)) continue;
        if (operations.count(c)) effect = true;
        auto* child = callee(c);
        if (child && !child->isDeclaration()) {
          if (!check(child)) return false;
          effect |= touches.at(child);
        }
      }
      // MAY ownership: joins are union, so both lock and unlock require a
      // singleton precondition. Helpers that touch L have net-zero ownership.
      if (effect) {
        std::map< llvm::BasicBlock*, unsigned > inputs;
        std::deque< llvm::BasicBlock* > work{&f->getEntryBlock()};
        inputs[&f->getEntryBlock()] = 1; // U=1, H=2, U|H=3
        while (!work.empty()) {
          auto* b = work.front(); work.pop_front(); auto state = inputs.at(b);
          for (auto& i : *b) {
            if (!spend()) return false;
            if (auto* c = llvm::dyn_cast< llvm::CallBase >(&i)) {
              if (operations.count(c)) {
                const bool lock = mutex_call(c).lock;
                if (state != (lock ? 1u : 2u)) return false;
                state = lock ? 2 : 1;
              } else if (auto* child = callee(c)) {
                if (!child->isDeclaration() && touches.at(child) && state != 1) return false;
              }
            }
            if (llvm::isa< llvm::ReturnInst >(i) && state != 1) return false;
          }
          for (auto* next : llvm::successors(b)) {
            auto old = inputs[next]; inputs[next] |= state;
            if (old != inputs[next]) work.push_back(next);
          }
        }
      }
      active.erase(f); touches.emplace(f, effect); return true;
    };
    for (auto* f : function_order) if (!check(f)) return false;
    mutex_protocols.at(g) = true; return true;
  }
  bool module_closed(llvm::Module& module) {
    // Module assembly can hide writers and startup entries from IR users.
    if (!module.getModuleInlineAsm().empty() || !mutex_effect_pointer_representations_closed(module)) return false;
    for (auto& f : module) {
      if (f.isDeclaration()) continue;
      if (!functions.count(&f)) return false;
      std::vector< llvm::Value* > todo{&f}; std::set< llvm::Value* > seen;
      while (!todo.empty()) {
        auto* v = todo.back(); todo.pop_back();
        if (!spend()) return false;
        if (!seen.insert(v).second) continue;
        for (auto* u : v->users()) {
          if (llvm::isa< llvm::BitCastOperator >(u)) { todo.push_back(u); continue; }
          auto* c = llvm::dyn_cast< llvm::CallBase >(u);
          if (!c) return false;
          if (c->getCalledOperand() == v && call_binding(c)) continue;
          auto* ar_call = call_binding(c);
          if (!ar_call || (!ctx.native_handle_effect_certificates().count(ar_call) && !unread_private_thread_output(ar_call)) ||
              !native_handle_api(c, true) || c->getArgOperand(2) != v) return false;
        }
      }
      for (auto& b : f) for (auto& i : b) {
        if (!spend()) return false;
        auto* c = llvm::dyn_cast< llvm::CallBase >(&i);
        if (!c || llvm::isa< llvm::DbgInfoIntrinsic >(c)) continue;
        auto* ar_call = call_binding(c);
        auto* target = ar_call ? native_handle_callee(ar_call) : nullptr;
        auto* source = callee(c);
        if (!ar_call || !target) return false;
        if (target->is_definition()) continue;
        if (ctx.native_handle_effect_certificates().count(ar_call) ||
            ctx.native_handle_joins.count(ar_call) ||
            ctx.mutex_effect_certificates().count(ar_call)) continue;
        if (native_handle_api(c, true) && unread_private_thread_output(ar_call)) continue;
        const auto id = target->intrinsic_id();
        if ((source->getName() == "__VERIFIER_atomic_begin" || source->getName() == "__VERIFIER_atomic_end") &&
            c->arg_size() == 0 && c->getType()->isVoidTy() &&
            target->name() == source->getName().str() && target->num_parameters() == 0 &&
            target->type()->return_type()->is_void()) continue;
        if ((id == ar::Intrinsic::PthreadMutexLock || id == ar::Intrinsic::PthreadMutexUnlock) &&
            c->arg_size() == 1 && c->getArgOperand(0)->getType()->isPointerTy() &&
            c->getType()->isIntegerTy(32) &&
            source->getName() == (id == ar::Intrinsic::PthreadMutexLock ? "pthread_mutex_lock" : "pthread_mutex_unlock")) continue;
        if (id == ar::Intrinsic::LibcAbort && source->getName() == "abort" && c->arg_size() == 0 && c->getType()->isVoidTy()) continue;
        if (constant_assertion_effect(ar_call) && source->getName() == "__assert_fail" && c->getType()->isVoidTy() && c->arg_size() == 4 &&
            c->getArgOperand(0)->getType()->isPointerTy() && c->getArgOperand(1)->getType()->isPointerTy() &&
            c->getArgOperand(2)->getType()->isIntegerTy(32) && c->getArgOperand(3)->getType()->isPointerTy()) continue;
        if (id == ar::Intrinsic::IkosNonDet && c->arg_size() == 0 && c->getType()->isIntegerTy() &&
            c->getMetadata("ikos.freeze_uninit") &&
            ((source->getName() == "__ikos_nondet_int" && c->getType()->isIntegerTy(32)) ||
             (source->getName() == "__ikos_nondet_int64" && c->getType()->isIntegerTy(64)))) continue;
        if (source->getName() == "__VERIFIER_nondet_int" && c->arg_size() == 0 && c->getType()->isIntegerTy(32)) continue;
        return false;
      }
    }
    return true;
  }
  bool integer_effect(llvm::Instruction* i, llvm::Value* pointer, ar::Value* ar_pointer) const {
    if (auto* l = llvm::dyn_cast< llvm::LoadInst >(i)) {
      auto* a = dyn_cast_or_null< ar::Load >(original(i));
      auto* t = a ? dyn_cast< ar::IntegerType >(a->result()->type()) : nullptr;
      return a && t && t->bit_width() == 32 && a->operand() == ar_pointer &&
          l->getPointerOperand() == pointer && l->getType()->isIntegerTy(32) && !l->isVolatile() && !l->isAtomic();
    }
    auto* s = llvm::dyn_cast< llvm::StoreInst >(i);
    auto* a = dyn_cast_or_null< ar::Store >(original(i));
    auto* t = a ? dyn_cast< ar::IntegerType >(a->value()->type()) : nullptr;
    return s && a && t && t->bit_width() == 32 && a->pointer() == ar_pointer &&
        s->getPointerOperand() == pointer && s->getValueOperand()->getType()->isIntegerTy(32) &&
        !s->isVolatile() && !s->isAtomic();
  }
  bool transaction(Counter& proof, const ZNumber& entry, ZNumber& next, ZNumber& output) {
    auto* f = proof.allocator;
    next = entry; unsigned writes = 0, outputs = 0;
    bool held = false; unsigned acquisitions = 0, releases = 0;
    std::map< llvm::Value*, ZNumber > values;
    auto value = [&](llvm::Value* v, ZNumber& n) {
      if (constant(v, n)) return true;
      auto it = values.find(v); if (it == values.end()) return false;
      n = it->second; return true;
    };
    auto* block = &f->getEntryBlock();
    llvm::BasicBlock* predecessor = nullptr;
    std::set< llvm::BasicBlock* > visited;
    while (visited.insert(block).second) {
      bool branched = false;
      // Phi inputs are read simultaneously from the actual predecessor.
      std::vector< std::pair< llvm::Value*, ZNumber > > phis;
      for (auto& i : *block) if (auto* phi = llvm::dyn_cast< llvm::PHINode >(&i)) {
        ZNumber n(0);
        if (!spend() || !predecessor || !phi->getType()->isIntegerTy(32) ||
            !value(phi->getIncomingValueForBlock(predecessor), n)) return false;
        phis.emplace_back(phi, n);
      }
      for (const auto& p : phis) values[p.first] = p.second;
      for (auto& i : *block) {
        if (!spend()) return false;
        if (llvm::isa< llvm::DbgInfoIntrinsic >(i) || llvm::isa< llvm::PHINode >(i)) continue;
        if (auto* l = llvm::dyn_cast< llvm::LoadInst >(&i)) {
          if (proof.serializer && !held) return false;
          if (l->getPointerOperand() == proof.global) values[&i] = next;
          else return false;
        } else if (auto* s = llvm::dyn_cast< llvm::StoreInst >(&i)) {
          if (proof.serializer && !held) return false;
          ZNumber n(0); if (!value(s->getValueOperand(), n)) return false;
          if (s->getPointerOperand() == proof.global) { next = n; ++writes; }
          else if (!proof.serializer && s->getPointerOperand() == f->getArg(0)) { output = n; ++outputs; }
          else return false;
        } else if (auto* c = llvm::dyn_cast< llvm::CallBase >(&i)) {
          if (unused_freeze(c)) continue;
          auto operation = serialization_call(c);
          if (!proof.serializer || operation.source != proof.serializer || operation.token != proof.binary_token) return false;
          if (operation.acquire) {
            if (held || acquisitions++) return false;
            held = true;
          } else {
            if (!held || releases++) return false;
            held = false;
          }
        } else if (llvm::isa< llvm::BitCastInst >(i)) {
          // Preflight admits only pointer casts of this actual serializer.
          if (!proof.serializer) return false;
        } else if (auto* op = llvm::dyn_cast< llvm::BinaryOperator >(&i)) {
          ZNumber a(0), b(0), n(0);
          if (!op->getType()->isIntegerTy(32) || !value(op->getOperand(0), a) || !value(op->getOperand(1), b)) return false;
          switch (op->getOpcode()) {
            case llvm::Instruction::Add: n = a + b; break;
            case llvm::Instruction::Sub: n = a - b; break;
            case llvm::Instruction::Mul: n = a * b; break;
            default: return false;
          }
          // Check at this instruction, before any later guard. Never assume nsw.
          if (!signed32(n)) return false;
          values[&i] = n;
        } else if (auto* cmp = llvm::dyn_cast< llvm::ICmpInst >(&i)) {
          ZNumber a(0), b(0); bool r;
          if (!cmp->getOperand(0)->getType()->isIntegerTy(32) ||
              !value(cmp->getOperand(0), a) || !value(cmp->getOperand(1), b)) return false;
          if (cmp->isUnsigned()) { if (a < 0) a += ZNumber(1) << 32; if (b < 0) b += ZNumber(1) << 32; }
          switch (cmp->getPredicate()) {
            case llvm::CmpInst::ICMP_EQ: r = a == b; break;
            case llvm::CmpInst::ICMP_NE: r = a != b; break;
            case llvm::CmpInst::ICMP_SLT: case llvm::CmpInst::ICMP_ULT: r = a < b; break;
            case llvm::CmpInst::ICMP_SLE: case llvm::CmpInst::ICMP_ULE: r = a <= b; break;
            case llvm::CmpInst::ICMP_SGT: case llvm::CmpInst::ICMP_UGT: r = a > b; break;
            case llvm::CmpInst::ICMP_SGE: case llvm::CmpInst::ICMP_UGE: r = a >= b; break;
            default: return false;
          }
          values[&i] = ZNumber(r ? 1 : 0);
        } else if (auto* br = llvm::dyn_cast< llvm::BranchInst >(&i)) {
          unsigned edge = 0;
          if (br->isConditional()) {
            auto found = values.find(br->getCondition()); if (found == values.end()) return false;
            edge = found->second == 0 ? 1 : 0;
          }
          predecessor = block; block = br->getSuccessor(edge); branched = true; break;
        } else if (auto* ret = llvm::dyn_cast< llvm::ReturnInst >(&i)) {
          if (proof.serializer) {
            if (held || acquisitions != 1 || releases != 1 || !ret->getReturnValue() ||
                !value(ret->getReturnValue(), output)) return false;
            outputs = 1;
          } else if (ret->getReturnValue()) return false;
          return outputs == 1 &&
              ((output == 0 && next == entry && writes == 0) ||
               (output == entry && next > entry && writes == 1));
        } else return false;
      }
      if (!branched) return false;
    }
    return false;
  }
  Counter certify_counter(llvm::Function* f) {
    Counter p;
    auto* ar_f = functions.at(f);
    const bool returns_value = f->arg_size() == 0 && f->getReturnType()->isIntegerTy(32);
    const bool atomic = f->arg_size() == 1 && f->getReturnType()->isVoidTy() &&
        f->getFunctionType()->getParamType(0)->isPointerTy() &&
        f->getFunctionType()->getParamType(0)->getPointerAddressSpace() == 0 &&
        f->getFunctionType()->getParamType(0)->getPointerElementType()->isIntegerTy(32) &&
        classify_atomic_intrinsic(ar_f->name()) == AtomicKind::SingleCall;
    if (f->getName() != ar_f->name() || f->isVarArg() || (!returns_value && !atomic)) return {};
    std::vector< llvm::BitCastInst* > serializer_casts;
    // Inspect the complete body, not just the paths encountered during closure.
    for (auto& b : *f) for (auto& i : b) {
      if (!spend()) return {};
      if (llvm::isa< llvm::DbgInfoIntrinsic >(i)) continue;
      llvm::Value* pointer = nullptr;
      if (auto* l = llvm::dyn_cast< llvm::LoadInst >(&i)) pointer = l->getPointerOperand();
      if (auto* s = llvm::dyn_cast< llvm::StoreInst >(&i)) pointer = s->getPointerOperand();
      if (pointer) {
        if (!returns_value && pointer == f->getArg(0)) {
          if (!llvm::isa< llvm::StoreInst >(i) || !integer_effect(&i, pointer, ar_f->param(0))) return {};
        } else {
          auto* g = llvm::dyn_cast< llvm::GlobalVariable >(pointer);
          if (!g || !globals.count(g) || (p.global && p.global != g) ||
              !integer_effect(&i, g, globals.at(g))) return {};
          p.global = g;
        }
      } else if (auto* c = llvm::dyn_cast< llvm::CallBase >(&i)) {
        if (!returns_value) return {};
        if (unused_freeze(c)) continue;
        auto m = serialization_call(c);
        if (!m.source || (p.serializer && (p.serializer != m.source || p.binary_token != m.token))) return {};
        p.serializer = m.source; p.binary_token = m.token;
      } else if (auto* cast = llvm::dyn_cast< llvm::BitCastInst >(&i)) {
        if (!returns_value || !cast->getType()->isPointerTy() || cast->getType()->getPointerAddressSpace() != 0) return {};
        serializer_casts.push_back(cast);
      } else if (!llvm::isa< llvm::BinaryOperator >(i) && !llvm::isa< llvm::ICmpInst >(i) &&
                 !llvm::isa< llvm::BranchInst >(i) && !llvm::isa< llvm::ReturnInst >(i) &&
                 !(returns_value && llvm::isa< llvm::PHINode >(i))) return {};
    }
    if (returns_value) {
      if (!p.serializer) return {};
      for (auto* c : serializer_casts) if (c->getOperand(0)->stripPointerCasts() != p.serializer) return {};
      if (!p.binary_token && !mutex_protocol(p.serializer)) return {};
    }
    auto* g = p.global;
    ZNumber start(0);
    if (!g || g->isConstant() || g->isThreadLocal() || g->isExternallyInitialized() ||
        g->getAddressSpace() != 0 || !g->hasInitializer() || !constant(g->getInitializer(), start) || start <= 0) return {};
    auto* ar_g = globals.at(g);
    if (!ar_g->is_definition() || ar_g->is_thread_local()) return {};
    unsigned initializers = 0;
    for (auto* b : *ar_g->initializer()) for (auto* stmt : *b) {
      auto* s = dyn_cast< ar::Store >(stmt);
      auto* n = s ? dyn_cast< ar::IntegerConstant >(s->value()) : nullptr;
      if (!s || s->pointer() != ar_g || !n || n->value().to_z_number() != start) return {};
      ++initializers;
    }
    if (initializers != 1) return {};
    for (auto* u : g->users()) {
      auto* i = llvm::dyn_cast< llvm::Instruction >(u);
      if (!i || i->getFunction() != f || !integer_effect(i, g, ar_g)) return {};
    }
    p.allocator = f;
    for (unsigned n = 0; n < 4096; ++n) {
      ZNumber next(0), out(0);
      if (!transaction(p, start, next, out)) return {};
      if (out == 0) return p.slices.empty() ? Counter{} : p;
      p.slices.emplace_back(start, next); start = next;
    }
    // No terminal self-loop: no invariant certificate, regardless of the budget.
    return {};
  }
  llvm::CallBase* private_output(llvm::AllocaInst* slot, llvm::Function* allocator) {
    auto* one = llvm::dyn_cast< llvm::ConstantInt >(slot->getArraySize());
    auto* a = dyn_cast_or_null< ar::Allocate >(original(slot));
    auto* size = a ? dyn_cast< ar::IntegerConstant >(a->array_size()) : nullptr;
    auto* type = a ? dyn_cast< ar::IntegerType >(a->allocated_type()) : nullptr;
    if (!one || !one->isOne() || slot->getAddressSpace() != 0 ||
        slot->getParent() != &slot->getFunction()->getEntryBlock() ||
        !slot->getAllocatedType()->isIntegerTy(32) || !a || !type || type->bit_width() != 32 ||
        !size || size->value().to_z_number() != 1) return nullptr;
    llvm::CallBase* call = nullptr;
    for (auto* u : slot->users()) if (auto* c = llvm::dyn_cast< llvm::CallBase >(u)) {
      if (llvm::isa< llvm::DbgInfoIntrinsic >(c)) continue;
      auto* ar_call = call_binding(c);
      if (call || callee(c) != allocator || !ar_call || !isa< ar::FunctionPointerConstant >(ar_call->called()) || c->arg_size() != 1 ||
          c->getArgOperand(0) != slot || ar_call->argument(0) != a->result()) return nullptr;
      call = c;
    }
    if (!call || follows(call, call)) return nullptr;
    llvm::DominatorTree dom(*slot->getFunction());
    for (auto* u : slot->users()) {
      if (!spend()) return nullptr;
      auto* i = llvm::dyn_cast< llvm::Instruction >(u);
      if (!i || i->getFunction() != slot->getFunction()) return nullptr;
      if (i == call || llvm::isa< llvm::DbgInfoIntrinsic >(i)) continue;
      if (!integer_effect(i, slot, a->result())) return nullptr;
      if (llvm::isa< llvm::LoadInst >(i)) { if (!dom.dominates(call, i)) return nullptr; }
      else if (!dom.dominates(i, call) || follows(call, i)) return nullptr;
    }
    return call;
  }
  bool abort_call(llvm::CallBase* c) const {
    auto* a = call_binding(c);
    auto* f = a ? native_handle_callee(a) : nullptr;
    auto* source = callee(c);
    return f && source->isDeclaration() && source->getName() == "abort" &&
        f->intrinsic_id() == ar::Intrinsic::LibcAbort && c->arg_size() == 0 &&
        c->getType()->isVoidTy() && f->type()->return_type()->is_void();
  }
  bool terminating_zero_call(llvm::CallBase* call) {
    if (abort_call(call)) return true;
    auto* a = call_binding(call); auto* f = callee(call);
    auto* zero = call->arg_size() == 1 ? llvm::dyn_cast< llvm::ConstantInt >(call->getArgOperand(0)) : nullptr;
    auto* ar_zero = a && a->num_arguments() == 1 ? dyn_cast< ar::IntegerConstant >(a->argument(0)) : nullptr;
    if (!a || !f || f->isDeclaration() || !call->use_empty() || !f->getReturnType()->isVoidTy() ||
        f->arg_size() != 1 || !f->getArg(0)->getType()->isIntegerTy(32) ||
        !zero || !zero->getType()->isIntegerTy(32) || !zero->isZero() ||
        !ar_zero || ar_zero->value().to_z_number() != 0) return false;
    std::map< llvm::Value*, llvm::APInt > values;
    values[f->getArg(0)] = llvm::APInt(32, 0);
    auto value = [&](llvm::Value* v, llvm::APInt& n) {
      if (auto* c = llvm::dyn_cast< llvm::ConstantInt >(v)) { n = c->getValue(); return true; }
      auto it = values.find(v); if (it == values.end()) return false;
      n = it->second; return true;
    };
    auto* block = &f->getEntryBlock(); std::set< llvm::BasicBlock* > seen;
    while (seen.insert(block).second) {
      bool branched = false;
      for (auto& i : *block) {
        if (!spend()) return false;
        if (llvm::isa< llvm::DbgInfoIntrinsic >(i)) continue;
        if (auto* c = llvm::dyn_cast< llvm::CallBase >(&i)) {
          if (unused_freeze(c)) continue;
          return abort_call(c);
        }
        if (auto* cmp = llvm::dyn_cast< llvm::ICmpInst >(&i)) {
          llvm::APInt x, y;
          if (!value(cmp->getOperand(0), x) || !value(cmp->getOperand(1), y) ||
              x.getBitWidth() != y.getBitWidth()) return false;
          values[cmp] = llvm::APInt(1, llvm::ICmpInst::compare(x, y, cmp->getPredicate()));
        } else if (auto* br = llvm::dyn_cast< llvm::BranchInst >(&i)) {
          unsigned edge = 0; llvm::APInt condition;
          if (br->isConditional()) { if (!value(br->getCondition(), condition)) return false; edge = condition.isZero() ? 1 : 0; }
          block = br->getSuccessor(edge); branched = true; break;
        } else return false;
      }
      if (!branched) return false;
    }
    return false;
  }
  bool terminating_zero_block(llvm::BasicBlock* block, llvm::Instruction* access) {
    for (auto& i : *block) {
      if (!spend() || &i == access) return false;
      if (auto* c = llvm::dyn_cast< llvm::CallBase >(&i)) {
        if (llvm::isa< llvm::DbgInfoIntrinsic >(c) || unused_freeze(c)) continue;
        // No earlier call may re-enter the access before this termination.
        return terminating_zero_call(c);
      }
    }
    return false;
  }
  bool nonzero(llvm::Instruction* origin, llvm::Instruction* access, llvm::DominatorTree& dom) {
    for (auto& b : *origin->getFunction()) {
      auto* br = llvm::dyn_cast< llvm::BranchInst >(b.getTerminator());
      auto* cmp = br && br->isConditional() ? llvm::dyn_cast< llvm::ICmpInst >(br->getCondition()) : nullptr;
      if (!cmp || !cmp->isEquality()) continue;
      auto* load = llvm::dyn_cast< llvm::LoadInst >(cmp->getOperand(0));
      auto* zero = llvm::dyn_cast< llvm::ConstantInt >(cmp->getOperand(1));
      const bool same = llvm::isa< llvm::AllocaInst >(origin)
          ? load && load->getPointerOperand() == origin : cmp->getOperand(0) == origin;
      if (!same || !zero || !zero->isZero()) continue;
      unsigned edge = cmp->getPredicate() == llvm::CmpInst::ICMP_NE ? 0 : 1;
      if (dom.dominates(llvm::BasicBlockEdge(br->getParent(), br->getSuccessor(edge)), access->getParent())) return true;
      // The raw CFG retains a continuation after a defined assume helper.
      // Cut only a zero edge with an independently proved actual abort path.
      if (dom.dominates(br, access) && terminating_zero_block(br->getSuccessor(1 - edge), access)) return true;
    }
    return false;
  }
  bool index(llvm::Value* v, llvm::Instruction*& origin, ZNumber& delta,
             const Counter& counter, unsigned depth = 0) {
    if (depth == 16 || !spend()) return false;
    // Native LP64 GEP indices preserve the signed i32 reservation value.
    if (auto* extension = llvm::dyn_cast< llvm::SExtInst >(v)) {
      if (ctx.bundle->data_layout().pointers.bit_width != 64 ||
          !extension->getType()->isIntegerTy(64) ||
          !extension->getOperand(0)->getType()->isIntegerTy(32)) return false;
      return index(extension->getOperand(0), origin, delta, counter, depth + 1);
    }
    if (auto* l = llvm::dyn_cast< llvm::LoadInst >(v)) {
      if (counter.serializer) return false;
      auto* slot = llvm::dyn_cast< llvm::AllocaInst >(l->getPointerOperand()); delta = ZNumber(0);
      origin = slot; return slot && private_output(slot, counter.allocator);
    }
    if (auto* c = llvm::dyn_cast< llvm::CallBase >(v)) {
      auto* a = call_binding(c);
      auto* t = a && a->has_result() ? dyn_cast< ar::IntegerType >(a->result()->type()) : nullptr;
      origin = c; delta = ZNumber(0);
      return counter.serializer && callee(c) == counter.allocator && a && t && t->bit_width() == 32 &&
          isa< ar::FunctionPointerConstant >(a->called()) && !follows(c, c);
    }
    auto* op = llvm::dyn_cast< llvm::BinaryOperator >(v);
    ZNumber c(0);
    if (!op || !op->getType()->isIntegerTy(32) ||
        (op->getOpcode() != llvm::Instruction::Add && op->getOpcode() != llvm::Instruction::Sub) ||
        !constant(op->getOperand(1), c) || !index(op->getOperand(0), origin, delta, counter, depth + 1)) return false;
    if (op->getOpcode() == llvm::Instruction::Add) delta += c; else delta -= c;
    // Includes exhausted output0, since expression evaluation may precede its guard.
    if (!signed32(delta)) return false;
    for (const auto& slice : counter.slices) if (!signed32(slice.first + delta)) return false;
    return true;
  }
  bool arena_closed(llvm::GlobalVariable* global) {
    auto found = closed_arenas.find(global);
    if (found != closed_arenas.end()) return found->second;
    closed_arenas.emplace(global, false);
    std::vector< llvm::Value* > todo{global}; std::set< llvm::Value* > seen;
    while (!todo.empty()) {
      auto* p = todo.back(); todo.pop_back();
      if (!spend()) return false;
      if (!seen.insert(p).second) continue;
      for (auto* u : p->users()) {
        if (llvm::isa< llvm::BitCastOperator >(u) || llvm::isa< llvm::GEPOperator >(u)) {
          if (!u->getType()->isPointerTy() || u->getType()->getPointerAddressSpace() != 0) return false;
          todo.push_back(u);
        } else if (auto* l = llvm::dyn_cast< llvm::LoadInst >(u)) {
          if (l->getPointerOperand() != p || !l->getType()->isIntegerTy() || !dyn_cast_or_null< ar::Load >(original(l))) return false;
        } else if (auto* s = llvm::dyn_cast< llvm::StoreInst >(u)) {
          if (s->getPointerOperand() != p || !s->getValueOperand()->getType()->isIntegerTy() || !dyn_cast_or_null< ar::Store >(original(s))) return false;
        } else if (!llvm::isa< llvm::DbgInfoIntrinsic >(u)) return false;
      }
    }
    closed_arenas.at(global) = true; return true;
  }
  Proof certify(llvm::Instruction* access, uint64_t width) {
    llvm::Value* pointer = nullptr;
    if (auto* l = llvm::dyn_cast< llvm::LoadInst >(access)) { if (l->getType()->isIntegerTy()) pointer = l->getPointerOperand(); }
    if (auto* s = llvm::dyn_cast< llvm::StoreInst >(access)) { if (s->getValueOperand()->getType()->isIntegerTy()) pointer = s->getPointerOperand(); }
    if (!pointer || !width) return {};
    while (auto* cast = llvm::dyn_cast< llvm::BitCastInst >(pointer)) {
      if (cast->getType()->getPointerAddressSpace() != 0) return {};
      pointer = cast->getOperand(0);
    }
    auto* gep = llvm::dyn_cast< llvm::GetElementPtrInst >(pointer);
    auto* g = gep ? llvm::dyn_cast< llvm::GlobalVariable >(gep->getPointerOperand()) : nullptr;
    auto* array = g ? llvm::dyn_cast< llvm::ArrayType >(g->getValueType()) : nullptr;
    auto* zero = gep && gep->getNumIndices() == 2 ? llvm::dyn_cast< llvm::ConstantInt >(gep->getOperand(1)) : nullptr;
    if (!g || !globals.count(g) || !array || !array->getElementType()->isIntegerTy() ||
        !zero || !zero->isZero() || gep->getSourceElementType() != array || g->getAddressSpace() != 0 ||
        g->isConstant() || g->isThreadLocal() || g->isExternallyInitialized() || !g->hasInitializer() || !arena_closed(g)) return {};
    auto* ar_array = dyn_cast< ar::ArrayType >(globals.at(g)->type()->pointee());
    if (!ar_array || ar_array->num_elements() != ZNumber(array->getNumElements())) return {};
    const auto& layout = g->getParent()->getDataLayout();
    const auto stride = ZNumber(layout.getTypeAllocSize(array->getElementType()).getFixedValue());
    const auto extent = ZNumber(layout.getTypeAllocSize(array).getFixedValue());
    if (extent >= (ZNumber(1) << 31) ||
        ctx.bundle->data_layout().alloc_size_in_bytes(globals.at(g)->type()->pointee()) != extent) return {};
    for (const auto& counter : counters) {
      if (!counter.global) continue;
      llvm::Instruction* origin = nullptr; ZNumber delta(0);
      if (!index(gep->getOperand(2), origin, delta, counter)) continue;
      llvm::DominatorTree dom(*access->getFunction());
      if (origin->getFunction() != access->getFunction() || !dom.dominates(origin, access) || !nonzero(origin, access, dom)) continue;
      bool bounded = delta >= 0;
      for (const auto& slice : counter.slices) {
        const auto begin = (slice.first + delta) * stride;
        bounded &= begin >= 0 && begin + width <= slice.second * stride && slice.second * stride <= extent;
      }
      if (bounded) return {g, counter.global};
    }
    return {};
  }
  const Proof* access(const AccessRecord& r) const {
    if (!r.stmt->has_frontend() || !r.pts.is_set() || r.pts.size() != 1 || !r.width) return nullptr;
    auto* source = llvm::dyn_cast< llvm::Instruction >(r.stmt->frontend< llvm::Value >());
    auto it = proofs.find(source);
    auto* location = dyn_cast< GlobalMemoryLocation >(*r.pts.begin());
    if (it == proofs.end() || !it->second.arena || !location ||
        globals.at(it->second.arena) != location->global_var() || original(source) != r.stmt) return nullptr;
    auto* l = llvm::dyn_cast< llvm::LoadInst >(source);
    auto* s = llvm::dyn_cast< llvm::StoreInst >(source);
    if ((!l && !s) || (l && r.kind != AccessKind::Read) || (s && r.kind != AccessKind::Write)) return nullptr;
    const auto& layout = source->getModule()->getDataLayout();
    if (layout.getTypeStoreSize(l ? l->getType() : s->getValueOperand()->getType()).getFixedValue() != r.width) return nullptr;
    return &it->second;
  }
public:
  Impl(Context& context, const std::vector< AccessRecord >& records, const BinaryTokenOwnership& tokens)
      : ctx(context), symbolic(context), binary_tokens(tokens) {
    if (!ctx.opts.enable_thread_modular || ctx.opts.procedural == Procedural::Intraprocedural ||
        ctx.opts.entry_points.size() != 1 || ctx.opts.entry_points.front()->name() != "main" ||
        ctx.opts.entry_points.front()->num_parameters() != 0 || !ctx.opts.entry_points.front()->has_frontend() ||
        !ctx.opts.no_init_globals.empty() ||
        ctx.concurrent_env.is_dirty() || !ctx.concurrent_env.unmodeled_effects().empty() ||
        !ctx.concurrent_env.unmodeled_thread_creations().empty() ||
        ctx.bundle->global_or_null("ar.global_ctors") || ctx.bundle->global_or_null("ar.global_dtors")) return;
    auto* main = ctx.opts.entry_points.front()->frontend< llvm::Function >();
    if (!main->use_empty()) return;
    const auto abi = pthread_object_layout(ctx.bundle, main->getParent());
    if (!abi || main->getParent()->getDataLayout().getIndexSizeInBits(0) != abi.word * 8) return;
    for (auto it = ctx.bundle->global_begin(); it != ctx.bundle->global_end(); ++it)
      if ((*it)->has_frontend())
        if (auto* g = llvm::dyn_cast< llvm::GlobalVariable >((*it)->frontend< llvm::Value >())) globals.emplace(g, *it);
    for (auto it = ctx.bundle->function_begin(); it != ctx.bundle->function_end(); ++it) {
      auto* f = *it;
      if (!f->is_definition() || !f->has_frontend()) continue;
      functions.emplace(f->frontend< llvm::Function >(), f);
      function_order.push_back(f->frontend< llvm::Function >());
      for (auto* b : *f->body()) for (auto* s : *b) if (s->has_frontend()) {
        // Only kinds used by the certificate need unique source bindings.
        if (!isa< ar::CallBase >(s) && !isa< ar::Load >(s) && !isa< ar::Store >(s) && !isa< ar::Allocate >(s)) continue;
        auto inserted = originals.emplace(s->frontend< llvm::Value >(), s);
        if (!inserted.second) inserted.first->second = nullptr;
      }
    }
    // Avoid a whole-module preflight on programs with no possible allocator.
    bool potential = false;
    for (auto* f : function_order)
      if ((classify_atomic_intrinsic(functions.at(f)->name()) == AtomicKind::SingleCall &&
           f->arg_size() == 1 && f->getReturnType()->isVoidTy()) ||
          (f->arg_size() == 0 && f->getReturnType()->isIntegerTy(32))) potential = true;
    if (!potential || !module_closed(*main->getParent())) return;
    for (auto* f : function_order) {
      if (classify_atomic_intrinsic(functions.at(f)->name()) != AtomicKind::SingleCall &&
          !(f->arg_size() == 0 && f->getReturnType()->isIntegerTy(32))) continue;
      auto proof = certify_counter(f);
      if (!proof.global) continue;
      // Every actual allocator use has an activation-private, closed output.
      bool closed = !f->use_empty();
      for (auto* u : f->users()) {
        auto* c = llvm::dyn_cast< llvm::CallBase >(u);
        if (proof.serializer) {
          auto* a = c ? call_binding(c) : nullptr;
          if (!a || c->getCalledOperand() != f || !isa< ar::FunctionPointerConstant >(a->called())) { closed = false; break; }
          continue;
        }
        auto* slot = c && c->arg_size() == 1 ? llvm::dyn_cast< llvm::AllocaInst >(c->getArgOperand(0)) : nullptr;
        if (!slot || private_output(slot, f) != c) { closed = false; break; }
      }
      if (closed) counters.push_back(std::move(proof));
    }
    if (counters.empty()) return;
    for (const auto& r : records) if (r.stmt->has_frontend()) {
      auto* source = llvm::dyn_cast< llvm::Instruction >(r.stmt->frontend< llvm::Value >());
      if (source && !proofs.count(source)) proofs.emplace(source, certify(source, r.width));
    }
  }
  bool excludes(const AccessRecord& a, const AccessRecord& b) const {
    if (symbolic.excludes(a, b)) return true;
    auto* pa = access(a); auto* pb = access(b);
    return pa && pb && pa->arena == pb->arena && pa->counter == pb->counter;
  }
  JsonDict info(const AccessRecord& r) const {
    auto symbolic_info = symbolic.info(r);
    if (!symbolic_info.empty()) return symbolic_info;
    auto* p = access(r);
    for (const auto& counter : counters) if (p && counter.global == p->counter)
      return {{"safe_reason", "disjoint serialized arena reservations"},
              {"arena_reservation_contract", "closed-finite-counter-slices-v1"},
              {"arena", p->arena->getName().str()},
              {"counter", p->counter->getName().str()},
              {"counter_initial", counter.slices.front().first.str()},
              {"counter_exhausted", counter.slices.back().second.str()},
              {"successful_reservations", std::to_string(counter.slices.size())},
              {"serialization", counter.binary_token ? "continuous owned named-atomic token interval" : counter.serializer ? "continuous default-normal mutex transaction" : "complete defined SV-COMP named-atomic body"},
              {"reservation_mutex", counter.serializer && !counter.binary_token ? counter.serializer->getName().str() : ""},
              {"reservation_token", counter.binary_token ? counter.serializer->getName().str() : ""}};
    return {};
  }
};
ArenaReservation::ArenaReservation(Context& ctx, const std::vector< AccessRecord >& records,
                                   const BinaryTokenOwnership& tokens)
    : _impl(std::make_unique< Impl >(ctx, records, tokens)) {}
ArenaReservation::~ArenaReservation() = default;
bool ArenaReservation::excludes(const AccessRecord& a, const AccessRecord& b) const { return _impl->excludes(a, b); }
JsonDict ArenaReservation::info(const AccessRecord& r) const { return _impl->info(r); }
} // namespace analyzer
} // namespace ikos
