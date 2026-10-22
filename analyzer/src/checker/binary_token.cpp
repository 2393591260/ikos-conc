// Copyright and license: see data_race.hpp.
#include <ikos/analyzer/checker/binary_token.hpp>

#include <ikos/analyzer/checker/data_race.hpp>
#include <ikos/analyzer/analysis/posix_semaphore.hpp>
#include <ikos/analyzer/analysis/source_call_binding.hpp>
#include <ikos/analyzer/checker/normal_mutex.hpp>
#include <ikos/analyzer/analysis/execution_engine/concurrent_semantics.hpp>
#include <deque>
#include <functional>
#include <map>
#include <set>

namespace ikos {
namespace analyzer {
namespace {
llvm::Function* callee(llvm::CallBase* c) {
  return llvm::dyn_cast< llvm::Function >(c->getCalledOperand()->stripPointerCasts());
}
struct Token {
  llvm::GlobalVariable* source = nullptr;
  bool semaphore = false;
  llvm::GlobalVariable* issued = nullptr;
  llvm::Function* issuer = nullptr;
  std::map< llvm::CallBase*, bool > operations;
  std::set< llvm::Instruction* > transition_effects;
  std::map< llvm::Instruction*, bool > held;
};
struct Transition {
  bool supported = false;
  bool returned = false;
  unsigned state = 0;
  unsigned writes = 0;
};
} // namespace

class BinaryTokenOwnership::Impl {
  Context& ctx;
  std::map< llvm::Value*, ar::Statement* > originals;
  std::map< llvm::GlobalVariable*, ar::GlobalVariable* > globals;
  std::map< llvm::Function*, ar::Function* > functions;
  std::vector< llvm::Function* > order, roots;
  std::map< std::pair< llvm::Function*, llvm::GlobalVariable* >, int > kinds;
  std::map< llvm::CallBase*, NormalMutex > mutex_calls;
  std::vector< Token > tokens;
  unsigned budget = 262144;
  bool spend() { if (!budget) return false; --budget; return true; }
  ar::Statement* original(llvm::Value* v) const {
    auto it = originals.find(v); return it == originals.end() ? nullptr : it->second;
  }
  ar::CallBase* binding(llvm::CallBase* c) const {
    return source_call_binding(c, original(c));
  }

  NormalMutex mutex_call(llvm::CallBase* c) {
    auto it = mutex_calls.find(c);
    if (it != mutex_calls.end()) return it->second;
    auto m = normal_mutex(ctx, binding(c)); mutex_calls.emplace(c, m); return m;
  }
  bool unused_freeze(llvm::CallBase* c) const {
    auto* a = binding(c); auto* f = a ? native_handle_callee(a) : nullptr;
    auto* source = callee(c);
    return f && source->isDeclaration() && f->intrinsic_id() == ar::Intrinsic::IkosNonDet &&
        c->use_empty() && c->arg_size() == 0 && c->getMetadata("ikos.freeze_uninit") &&
        ((source->getName() == "__ikos_nondet_int" && c->getType()->isIntegerTy(32)) ||
         (source->getName() == "__ikos_nondet_int64" && c->getType()->isIntegerTy(64)));
  }
  bool create(llvm::CallBase* c) const {
    auto* a = binding(c);
    return a && native_handle_api(c, true) &&
        (ctx.native_handle_effect_certificates().count(a) || unread_private_thread_output(a) ||
         (ctx.posix_semaphore() && ctx.posix_semaphore()->opaque_handle_call(c)));
  }
  bool terminal(llvm::CallBase* c) const {
    auto* a = binding(c); auto* f = a ? native_handle_callee(a) : nullptr;
    auto* source = callee(c);
    return f && source->isDeclaration() &&
        ((f->intrinsic_id() == ar::Intrinsic::LibcAbort && source->getName() == "abort" &&
          c->arg_size() == 0 && c->getType()->isVoidTy()) ||
         (source->getName() == "__assert_fail" && constant_assertion_effect(a) &&
          c->getType()->isVoidTy() && c->arg_size() == 4));
  }
  bool external(llvm::CallBase* c) {
    auto* a = binding(c); auto* target = a ? native_handle_callee(a) : nullptr;
    auto* source = callee(c);
    if (!a || !target || !source->isDeclaration()) return false;
    if (create(c) || ctx.native_handle_joins.count(a) || ctx.mutex_effect_certificates().count(a)) return true;
    if (ctx.posix_semaphore() && (ctx.posix_semaphore()->effect(a) ||
        ctx.posix_semaphore()->opaque_handle_call(c) || ctx.posix_semaphore()->allocation_call(c))) return true;
    if (mutex_call(c).source || terminal(c)) return true;
    const auto id = target->intrinsic_id();
    if ((source->getName() == "__VERIFIER_atomic_begin" || source->getName() == "__VERIFIER_atomic_end") &&
        c->arg_size() == 0 && c->getType()->isVoidTy() && target->name() == source->getName().str() &&
        target->num_parameters() == 0 && target->type()->return_type()->is_void()) return true;
    if (id == ar::Intrinsic::IkosNonDet && c->arg_size() == 0 && c->getMetadata("ikos.freeze_uninit") &&
        ((source->getName() == "__ikos_nondet_int" && c->getType()->isIntegerTy(32)) ||
         (source->getName() == "__ikos_nondet_int64" && c->getType()->isIntegerTy(64)))) return true;
    return source->getName() == "__VERIFIER_nondet_int" && c->arg_size() == 0 && c->getType()->isIntegerTy(32);
  }
  bool inventory(llvm::Module& module, llvm::Function* main) {
    if (!module.getModuleInlineAsm().empty() || !mutex_effect_pointer_representations_closed(module)) return false;
    // Seal every function address before declaring an unused helper unreachable.
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
          if (!c || !binding(c)) return false;
          if (c->getCalledOperand() == v) continue;
          if (!create(c) || c->getArgOperand(2) != v || c->getFunction() != main) return false;
        }
      }
      if (&f != main && f.use_empty()) continue;
      for (auto& b : f) for (auto& i : b) {
        if (!spend()) return false;
        if (llvm::isa< llvm::InvokeInst >(i) || llvm::isa< llvm::CallBrInst >(i) ||
            llvm::isa< llvm::IndirectBrInst >(i) || llvm::isa< llvm::ResumeInst >(i) ||
            llvm::isa< llvm::AtomicRMWInst >(i) || llvm::isa< llvm::AtomicCmpXchgInst >(i)) return false;
        auto* c = llvm::dyn_cast< llvm::CallBase >(&i);
        if (!c || llvm::isa< llvm::DbgInfoIntrinsic >(c)) continue;
        auto* a = binding(c);
        if (!a || (callee(c)->isDeclaration() && !external(c))) return false;
        if (create(c)) {
          if (&f != main) return false;
          auto* callback = llvm::dyn_cast< llvm::Function >(c->getArgOperand(2)->stripPointerCasts());
          if (!callback || !functions.count(callback)) return false;
          if (std::find(roots.begin(), roots.end(), callback) == roots.end()) roots.push_back(callback);
        }
      }
    }
    return true;
  }
  bool token_object(llvm::GlobalVariable* g) const {
    if (!g || !globals.count(g) || g->isConstant() || g->isThreadLocal() || g->isExternallyInitialized() ||
        g->getAddressSpace() != 0 || !g->hasInitializer() ||
        !(g->getValueType()->isIntegerTy(8) || g->getValueType()->isIntegerTy(32) ||
          g->getValueType()->isIntegerTy(64))) return false;
    auto* n = llvm::dyn_cast< llvm::ConstantInt >(g->getInitializer());
    auto* a = globals.at(g); auto* type = dyn_cast< ar::IntegerType >(a->type()->pointee());
    if (!n || !n->isZero() || !a->is_definition() || a->is_thread_local() || !type ||
        type->bit_width() != n->getBitWidth() ||
        ctx.bundle->data_layout().alloc_size_in_bytes(type) != n->getBitWidth() / 8) return false;
    unsigned count = 0;
    for (auto* b : *a->initializer()) for (auto* s : *b) {
      auto* store = dyn_cast< ar::Store >(s);
      auto* v = store ? dyn_cast< ar::IntegerConstant >(store->value()) : nullptr;
      if (!store || store->pointer() != a || !v || v->type() != type || v->value().to_z_number() != 0) return false;
      ++count;
    }
    return count == 1;
  }
  bool token_effect(llvm::Instruction* i, llvm::Function* f, llvm::GlobalVariable* source) const {
    llvm::Value* pointer = source ? static_cast< llvm::Value* >(source) : f->getArg(0);
    ar::Value* ar_pointer = source ? static_cast< ar::Value* >(globals.at(source)) : functions.at(f)->param(0);
    unsigned bits = source ? source->getValueType()->getIntegerBitWidth() : 32;
    if (auto* l = llvm::dyn_cast< llvm::LoadInst >(i)) {
      auto* a = dyn_cast_or_null< ar::Load >(original(i));
      auto* t = a ? dyn_cast< ar::IntegerType >(a->result()->type()) : nullptr;
      return a && t && t->bit_width() == bits &&
          (source ? exact_handle_slot(a->operand(), globals.at(source)) : a->operand() == ar_pointer) &&
          l->getPointerOperand() == pointer && l->getType()->isIntegerTy(bits) && !l->isAtomic();
    }
    auto* s = llvm::dyn_cast< llvm::StoreInst >(i);
    auto* a = dyn_cast_or_null< ar::Store >(original(i));
    auto* t = a ? dyn_cast< ar::IntegerType >(a->value()->type()) : nullptr;
    return s && a && t && t->bit_width() == bits &&
        (source ? exact_handle_slot(a->pointer(), globals.at(source)) : a->pointer() == ar_pointer) &&
        s->getPointerOperand() == pointer && s->getValueOperand()->getType()->isIntegerTy(bits) && !s->isAtomic();
  }
  bool atomic_signature(llvm::Function* f) const {
    auto found = functions.find(f);
    if (found == functions.end() || f->isVarArg() || f->arg_size() > 1 ||
        !f->getReturnType()->isVoidTy() || f->getCallingConv() != llvm::CallingConv::C) return false;
    if (f->arg_empty()) return found->second->num_parameters() == 0 &&
        found->second->type()->return_type()->is_void() && found->second->name() == f->getName().str() &&
        classify_atomic_intrinsic(found->second->name()) == AtomicKind::SingleCall;
    auto* p = llvm::dyn_cast< llvm::PointerType >(f->getArg(0)->getType());
    auto* a = found->second;
    auto* ar_p = a->num_parameters() == 1 ? dyn_cast< ar::PointerType >(a->param(0)->type()) : nullptr;
    auto* ar_i = ar_p ? dyn_cast< ar::IntegerType >(ar_p->pointee()) : nullptr;
    return p && p->getAddressSpace() == 0 && p->getPointerElementType()->isIntegerTy(32) &&
        ar_i && ar_i->bit_width() == 32 && a->type()->return_type()->is_void() && a->name() == f->getName().str() &&
        classify_atomic_intrinsic(a->name()) == AtomicKind::SingleCall;
  }
  Transition transition(llvm::Function* operation, unsigned entry, llvm::GlobalVariable* source, const llvm::APInt* actual = nullptr) {
    unsigned state = entry, writes = 0;
    std::set< llvm::Function* > active;
    std::function< Transition(llvm::Function*, const llvm::APInt*, bool) > run =
        [&](llvm::Function* f, const llvm::APInt* argument, bool root) -> Transition {
      if (active.size() == 32 || !active.insert(f).second) return {};
      std::map< llvm::Value*, llvm::APInt > values;
      if (argument) values[f->getArg(0)] = *argument;
      auto value = [&](llvm::Value* v, llvm::APInt& n) {
        if (auto* c = llvm::dyn_cast< llvm::ConstantInt >(v)) { n = c->getValue(); return true; }
        auto it = values.find(v); if (it == values.end()) return false;
        n = it->second; return true;
      };
      auto finish = [&](bool returned) { active.erase(f); return Transition{true, returned, state, writes}; };
      auto* block = &f->getEntryBlock(); std::set< llvm::BasicBlock* > seen;
      while (seen.insert(block).second) {
        bool branched = false;
        for (auto& i : *block) {
          if (!spend()) return {};
          if (llvm::isa< llvm::DbgInfoIntrinsic >(i)) continue;
          if (llvm::isa< llvm::LoadInst >(i)) {
            if (!root || !token_effect(&i, f, source)) return {};
            values[&i] = llvm::APInt(i.getType()->getIntegerBitWidth(), state);
          } else if (auto* s = llvm::dyn_cast< llvm::StoreInst >(&i)) {
            auto* n = llvm::dyn_cast< llvm::ConstantInt >(s->getValueOperand());
            auto* a = dyn_cast_or_null< ar::Store >(original(s));
            auto* ar_n = a ? dyn_cast< ar::IntegerConstant >(a->value()) : nullptr;
            if (!root || !token_effect(&i, f, source) || !n || (!n->isZero() && !n->isOne()) ||
                !ar_n || ar_n->value().to_z_number() != core::ZNumber(n->getZExtValue())) return {};
            state = n->getZExtValue(); ++writes;
          } else if (auto* cmp = llvm::dyn_cast< llvm::ICmpInst >(&i)) {
            llvm::APInt x, y;
            if (!value(cmp->getOperand(0), x) || !value(cmp->getOperand(1), y) || x.getBitWidth() != y.getBitWidth()) return {};
            values[&i] = llvm::APInt(1, llvm::ICmpInst::compare(x, y, cmp->getPredicate()));
          } else if (auto* z = llvm::dyn_cast< llvm::CastInst >(&i)) {
            llvm::APInt n;
            if (!z->getType()->isIntegerTy() || !z->getSrcTy()->isIntegerTy() ||
                !value(z->getOperand(0), n)) return {};
            unsigned bits = z->getType()->getIntegerBitWidth();
            if (bits > 64) return {};
            if (llvm::isa< llvm::ZExtInst >(z)) values[&i] = n.zext(bits);
            else if (llvm::isa< llvm::SExtInst >(z)) values[&i] = n.sext(bits);
            else if (llvm::isa< llvm::TruncInst >(z)) values[&i] = n.trunc(bits);
            else return {};
          } else if (auto* c = llvm::dyn_cast< llvm::CallBase >(&i)) {
            if (unused_freeze(c)) continue;
            auto* a = binding(c); auto* target = a ? native_handle_callee(a) : nullptr;
            auto* child = callee(c);
            if (!a || !target) return {};
            if (child->isDeclaration() && child->getName() == "abort" && c->arg_size() == 0 &&
                c->getType()->isVoidTy() && target->intrinsic_id() == ar::Intrinsic::LibcAbort &&
                target->type()->return_type()->is_void()) return finish(false);
            llvm::APInt n;
            if (child->isDeclaration() || c->arg_size() != 1 || !c->use_empty() || !c->getType()->isVoidTy() ||
                !child->getArg(0)->getType()->isIntegerTy(32) || !value(c->getArgOperand(0), n) || n.getBitWidth() != 32 ||
                classify_atomic_intrinsic(functions.at(child)->name()) != AtomicKind::None) return {};
            auto result = run(child, &n, false);
            if (!result.supported) return {};
            if (!result.returned) return finish(false);
          } else if (auto* br = llvm::dyn_cast< llvm::BranchInst >(&i)) {
            unsigned edge = 0; llvm::APInt n;
            if (br->isConditional()) { if (!value(br->getCondition(), n)) return {}; edge = n.isZero() ? 1 : 0; }
            block = br->getSuccessor(edge); branched = true; break;
          } else if (auto* ret = llvm::dyn_cast< llvm::ReturnInst >(&i)) {
            if (ret->getReturnValue()) return {};
            return finish(true);
          } else return {};
        }
        if (!branched) return {};
      }
      return {};
    };
    return run(operation, actual, actual == nullptr);
  }
  int kind(llvm::Function* f, llvm::GlobalVariable* source = nullptr) {
    auto key = std::make_pair(f, source);
    auto known = kinds.find(key); if (known != kinds.end()) return known->second;
    kinds.emplace(key, 0);
    if (!atomic_signature(f)) return 0;
    if (f->arg_empty() != (source != nullptr)) return 0;
    // Every pointer use is a complete direct access; no hidden alias/callback.
    if (!source) for (auto* u : f->getArg(0)->users()) {
      if (!spend()) return 0;
      auto* i = llvm::dyn_cast< llvm::Instruction >(u);
      if (!i || (!llvm::isa< llvm::DbgInfoIntrinsic >(i) && !token_effect(i, f, nullptr))) return 0;
    }
    auto zero = transition(f, 0, source); auto one = transition(f, 1, source);
    if (!zero.supported || !one.supported) return 0;
    int result = 0;
    if (zero.returned && zero.state == 1 && zero.writes == 1 && !one.returned && one.state == 1 && one.writes == 0) result = 1;
    if (!zero.returned && zero.state == 0 && zero.writes == 0 && one.returned && one.state == 0 && one.writes == 1) result = -1;
    kinds.at(key) = result; return result;
  }
  bool close_token(Token& token) {
    std::vector< llvm::Value* > todo{token.source}; std::set< llvm::Value* > seen;
    std::set< llvm::Function* > direct_operations;
    bool acquire = false, release = false;
    while (!todo.empty()) {
      auto* v = todo.back(); todo.pop_back();
      if (!spend()) return false;
      if (!seen.insert(v).second) continue;
      for (auto* u : v->users()) {
        if (llvm::isa< llvm::BitCastOperator >(u)) { todo.push_back(u); continue; }
        if (auto* i = llvm::dyn_cast< llvm::Instruction >(u)) {
          if (v == token.source && (llvm::isa< llvm::LoadInst >(i) || llvm::isa< llvm::StoreInst >(i))) {
            auto* f = i->getFunction();
            if (!functions.count(f) || !token_effect(i, f, token.source)) return false;
            if (atomic_signature(f) && f->arg_empty()) {
              if (!kind(f, token.source)) return false;
              direct_operations.insert(f); token.transition_effects.insert(i);
            } else if (!llvm::isa< llvm::LoadInst >(i)) return false;
            continue;
          }
        }
        auto* c = llvm::dyn_cast< llvm::CallBase >(u);
        auto* a = c ? binding(c) : nullptr;
        if (!token.source->getValueType()->isIntegerTy(32) ||
            !a || c->arg_size() != 1 || c->getArgOperand(0) != v || !c->use_empty() ||
            !isa< ar::FunctionPointerConstant >(a->called()) || !exact_handle_slot(a->argument(0), globals.at(token.source))) return false;
        int k = kind(callee(c)); if (!k) return false;
        token.operations.emplace(c, k == 1); acquire |= k == 1; release |= k == -1;
      }
    }
    // Global-form operations have no pointer actual. Inventory sealed every
    // function address; collect all actual calls, including helper/loop uses.
    if (!direct_operations.empty()) for (auto* f : order) for (auto& b : *f) for (auto& i : b) {
      if (!spend()) return false;
      auto* c = llvm::dyn_cast< llvm::CallBase >(&i);
      if (!c || !direct_operations.count(callee(c))) continue;
      if (!binding(c) || !c->arg_empty() || !c->use_empty()) return false;
      int k = kind(callee(c), token.source);
      token.operations.emplace(c, k == 1); acquire |= k == 1; release |= k == -1;
    }
    return acquire && release;
  }
  bool ownership(Token& token) {
    std::map< std::pair< llvm::Function*, unsigned >, unsigned > summaries;
    std::set< llvm::Function* > active;
    bool valid = true;
    std::function< unsigned(llvm::Function*, unsigned) > run = [&](llvm::Function* f, unsigned entry) {
      auto key = std::make_pair(f, entry); auto known = summaries.find(key);
      if (known != summaries.end()) return known->second;
      if (active.size() == 64 || !active.insert(f).second) { valid = false; return 0u; }
      std::map< llvm::BasicBlock*, unsigned > inputs;
      std::deque< llvm::BasicBlock* > work{&f->getEntryBlock()};
      inputs[&f->getEntryBlock()] = entry; unsigned exits = 0;
      while (valid && !work.empty()) {
        auto* b = work.front(); work.pop_front(); unsigned states = inputs.at(b);
        for (auto& i : *b) {
          if (!spend()) { valid = false; break; }
          if (!states) break;
          // U=1/H=2; observations from every reachable entry context use AND.
          if (llvm::isa< llvm::LoadInst >(i) || llvm::isa< llvm::StoreInst >(i)) {
            auto inserted = token.held.emplace(&i, states == 2);
            if (!inserted.second) inserted.first->second &= states == 2;
          }
          if (auto* c = llvm::dyn_cast< llvm::CallBase >(&i)) {
            if (llvm::isa< llvm::DbgInfoIntrinsic >(c)) continue;
            auto operation = token.operations.find(c);
            if (operation != token.operations.end()) {
              if (states != (operation->second ? 1u : 2u)) { valid = false; break; }
              states = operation->second ? 2 : 1;
            } else if (terminal(c)) states = 0;
            else if (!callee(c)->isDeclaration()) {
              unsigned next = 0;
              for (unsigned s : {1u, 2u}) if (states & s) next |= run(callee(c), s);
              states = next;
            }
          }
          if (llvm::isa< llvm::UnreachableInst >(i) && states) { valid = false; break; }
          // Return preserves H. Ending a root does not release its token.
          if (llvm::isa< llvm::ReturnInst >(i)) exits |= states;
        }
        for (auto* next : llvm::successors(b)) {
          auto old = inputs[next]; inputs[next] |= states;
          if (old != inputs[next]) work.push_back(next);
        }
      }
      active.erase(f); summaries.emplace(key, exits); return exits;
    };
    for (auto* root : roots) { run(root, 1); if (!valid) return false; }
    return valid;
  }
  ar::Value* ticket_base(ar::Value* v) const {
    for (unsigned depth = 0; depth < 32; ++depth) {
      if (isa< ar::GlobalVariable >(v) || isa< ar::LocalVariable >(v))
        return v;
      auto* iv = dyn_cast< ar::InternalVariable >(v);
      auto* d = iv ? unique_def(iv) : nullptr;
      if (auto* a = dyn_cast_or_null< ar::Assignment >(d))
        v = a->operand();
      else if (auto* a = dyn_cast_or_null< ar::UnaryOperation >(d)) {
        if (a->op() != ar::UnaryOperation::Bitcast)
          return v;
        v = a->operand();
      } else
        return v;
    }
    return nullptr;
  }
  bool same_ticket_value(ar::Value* a, llvm::Value* n) const {
    return source_integer_binding(a, n);
  }

  bool ticket_counter(llvm::GlobalVariable* g) const {
    if (!g || !g->getValueType()->isIntegerTy(32) || !token_object(g))
      return false;
    auto* t = dyn_cast< ar::IntegerType >(globals.at(g)->type()->pointee());
    return t && !t->is_signed() && t->bit_width() == 32;
  }
  bool ticket_memory(llvm::Instruction* i, llvm::Function* issuer) const {
    auto* l = llvm::dyn_cast< llvm::LoadInst >(i);
    auto* s = llvm::dyn_cast< llvm::StoreInst >(i);
    auto* a_l = dyn_cast_or_null< ar::Load >(original(i));
    auto* a_s = dyn_cast_or_null< ar::Store >(original(i));
    if ((!l && !s) || (l && (!a_l || l->isAtomic() || a_l->is_atomic())) ||
        (s && (!a_s || s->isAtomic() || a_s->is_atomic())))
      return false;
    auto* type = l ? l->getType() : s->getValueOperand()->getType();
    auto* at = l ? a_l->result()->type() : a_s->value()->type();
    if (!type->isIntegerTy() || type->getIntegerBitWidth() > 64 ||
        !at->is_integer() ||
        cast< ar::IntegerType >(at)->bit_width() !=
            type->getIntegerBitWidth() ||
        ctx.bundle->data_layout().store_size_in_bytes(at) !=
            i->getModule()
                ->getDataLayout()
                .getTypeStoreSize(type)
                .getFixedValue())
      return false;
    auto* p = (l ? l->getPointerOperand() : s->getPointerOperand())
                  ->stripPointerCasts();
    auto* a = ticket_base(l ? a_l->operand() : a_s->pointer());
    if (auto* g = llvm::dyn_cast< llvm::GlobalVariable >(p))
      return globals.count(g) && a == globals.at(g) &&
             g->getValueType() == type && !g->isThreadLocal() &&
             !g->isExternallyInitialized() && g->getAddressSpace() == 0 &&
             g->hasInitializer();
    if (auto* local = llvm::dyn_cast< llvm::AllocaInst >(p)) {
      auto* count = llvm::dyn_cast< llvm::ConstantInt >(local->getArraySize());
      auto* variable = dyn_cast_or_null< ar::LocalVariable >(a);
      return count && count->isOne() && local->getAllocatedType() == type &&
             local->getAddressSpace() == 0 && variable &&
             variable->has_frontend() &&
             variable->frontend< llvm::Value >() == local;
    }
    return s && i->getFunction() == issuer && p == issuer->getArg(0) &&
           a == functions.at(issuer)->param(0) && type->isIntegerTy(32);
  }
  bool ticket_scalar_guard(llvm::Function* f,
                           std::set< llvm::Function* >& active) {
    if (!functions.count(f) || !active.insert(f).second || active.size() > 32)
      return false;
    for (auto& b : *f)
      for (auto& i : b) {
        if (!spend())
          return false;
        if (llvm::isa< llvm::DbgInfoIntrinsic >(i))
          continue;
        if (llvm::isa< llvm::LoadInst >(i) || llvm::isa< llvm::StoreInst >(i) ||
            llvm::isa< llvm::AllocaInst >(i))
          return false;
        if (auto* c = llvm::dyn_cast< llvm::CallBase >(&i)) {
          if (!binding(c))
            return false;
          if (terminal(c) || unused_freeze(c))
            continue;
          auto* child = callee(c);
          if (child->isDeclaration() ||
              classify_atomic_intrinsic(child->getName().str()) !=
                  AtomicKind::None ||
              !ticket_scalar_guard(child, active))
            return false;
        }
      }
    active.erase(f);
    return true;
  }
  bool ticket_marker(llvm::Instruction* i, bool begin) const {
    auto* c = llvm::dyn_cast< llvm::CallBase >(i);
    auto* a = c ? binding(c) : nullptr;
    auto* f = c ? callee(c) : nullptr;
    auto* af = a ? native_handle_callee(a) : nullptr;
    const char* name =
        begin ? "__VERIFIER_atomic_begin" : "__VERIFIER_atomic_end";
    return f && af && f->isDeclaration() && af->is_declaration() &&
           f->getName() == name && af->name() == name && c->arg_empty() &&
           c->getType()->isVoidTy() && af->num_parameters() == 0 &&
           af->type()->return_type()->is_void();
  }
  std::vector< llvm::Instruction* > ticket_instructions(
      llvm::BasicBlock& b) const {
    std::vector< llvm::Instruction* > result;
    for (auto& i : b)
      if (!llvm::isa< llvm::DbgInfoIntrinsic >(i))
        result.push_back(&i);
    return result;
  }
  llvm::LoadInst* ticket_increment(llvm::StoreInst* store,
                                   llvm::GlobalVariable* g) const {
    if (!store || store->getPointerOperand() != g ||
        !store->getValueOperand()->getType()->isIntegerTy(32))
      return nullptr;
    auto* add =
        llvm::dyn_cast< llvm::BinaryOperator >(store->getValueOperand());
    if (!add || add->getOpcode() != llvm::Instruction::Add)
      return nullptr;
    auto* l = llvm::dyn_cast< llvm::LoadInst >(add->getOperand(0));
    auto* one = llvm::dyn_cast< llvm::ConstantInt >(add->getOperand(1));
    auto* a = dyn_cast_or_null< ar::Store >(original(store));
    auto* result = a ? dyn_cast< ar::InternalVariable >(a->value()) : nullptr;
    auto* operation =
        result ? dyn_cast_or_null< ar::BinaryOperation >(unique_def(result))
               : nullptr;
    return l && l->getPointerOperand() == g && l->getType()->isIntegerTy(32) &&
                   one && one->isOne() && operation &&
                   operation->op() == ar::BinaryOperation::UAdd &&
                   same_ticket_value(operation->left(), l) &&
                   same_ticket_value(operation->right(), one) &&
                   same_ticket_value(a->value(), add)
               ? l
               : nullptr;
  }
  bool ticket_ownership(llvm::Function* issuer, Token& token) {
    if (!atomic_signature(issuer) || issuer->arg_size() != 1 ||
        issuer->size() != 1)
      return false;
    llvm::StoreInst *output = nullptr, *update = nullptr;
    llvm::CallBase* guard = nullptr;
    for (auto& i : issuer->getEntryBlock()) {
      if (!spend())
        return false;
      if (llvm::isa< llvm::DbgInfoIntrinsic >(i))
        continue;
      if (auto* s = llvm::dyn_cast< llvm::StoreInst >(&i)) {
        if (s->getPointerOperand() == issuer->getArg(0)) {
          if (output)
            return false;
          output = s;
        } else {
          if (update)
            return false;
          update = s;
        }
      } else if (auto* c = llvm::dyn_cast< llvm::CallBase >(&i)) {
        if (guard)
          return false;
        guard = c;
      } else if (!llvm::isa< llvm::LoadInst >(i) &&
                 !llvm::isa< llvm::ICmpInst >(i) &&
                 !llvm::isa< llvm::CastInst >(i) &&
                 !llvm::isa< llvm::BinaryOperator >(i) &&
                 !llvm::isa< llvm::ReturnInst >(i))
        return false;
    }
    auto* next = update ? llvm::dyn_cast< llvm::GlobalVariable >(
                              update->getPointerOperand())
                        : nullptr;
    auto* old =
        output ? llvm::dyn_cast< llvm::LoadInst >(output->getValueOperand())
               : nullptr;
    auto* step = next ? ticket_increment(update, next) : nullptr;
    if (!next || !ticket_counter(next) || !old ||
        old->getPointerOperand() != next || !step || !guard ||
        !binding(guard) || !guard->comesBefore(output) ||
        !output->comesBefore(update) || !old->comesBefore(update) ||
        !step->comesBefore(update) || guard->arg_size() != 1 ||
        !guard->use_empty() || !guard->getType()->isVoidTy())
      return false;
    auto* argument = llvm::dyn_cast< llvm::ZExtInst >(guard->getArgOperand(0));
    auto* compare =
        argument ? llvm::dyn_cast< llvm::ICmpInst >(argument->getOperand(0))
                 : nullptr;
    if (!argument || !argument->getType()->isIntegerTy(32) || !compare ||
        compare->getPredicate() != llvm::CmpInst::ICMP_NE)
      return false;
    auto* check = llvm::dyn_cast< llvm::LoadInst >(compare->getOperand(0));
    auto* maximum = llvm::dyn_cast< llvm::ConstantInt >(compare->getOperand(1));
    if (!check || check->getPointerOperand() != next || !maximum ||
        !maximum->getType()->isIntegerTy(32) || !maximum->isMinusOne() ||
        !check->comesBefore(guard))
      return false;
    if (!same_ticket_value(binding(guard)->argument(0), argument))
      return false;
    auto* helper = callee(guard);
    std::set< llvm::Function* > active;
    if (helper->arg_size() != 1 ||
        !helper->getArg(0)->getType()->isIntegerTy(32) ||
        !ticket_scalar_guard(helper, active))
      return false;
    llvm::APInt zero(32, 0);
    auto rejected = transition(helper, 0, nullptr, &zero);
    if (!rejected.supported || rejected.returned)
      return false;
    auto* out = dyn_cast< ar::Store >(original(output));
    if (!out || !same_ticket_value(out->value(), old))
      return false;
    for (auto* u : issuer->getArg(0)->users())
      if (u != output && !llvm::isa< llvm::DbgInfoIntrinsic >(u))
        return false;
    for (auto& i : issuer->getEntryBlock())
      if (auto* l = llvm::dyn_cast< llvm::LoadInst >(&i))
        if (l->getPointerOperand() != next)
          return false;
    std::set< llvm::Instruction* > next_effects;
    for (auto* u : next->users()) {
      auto* i = llvm::dyn_cast< llvm::Instruction >(u);
      if (!i || i->getFunction() != issuer || !ticket_memory(i, issuer) ||
          (llvm::isa< llvm::StoreInst >(i) && i != update))
        return false;
      next_effects.insert(i);
    }
    std::map< llvm::Instruction*, unsigned > rules;
    std::set< llvm::Instruction* > markers, served_effects;
    std::set< llvm::AllocaInst* > slots;
    llvm::GlobalVariable* served = nullptr;
    unsigned callers = 0;
    for (auto* f : order)
      for (auto& b : *f)
        for (auto& i : b) {
          auto* c = llvm::dyn_cast< llvm::CallBase >(&i);
          if (!c || callee(c) != issuer)
            continue;
          auto* a = binding(c);
          auto* slot = llvm::dyn_cast< llvm::AllocaInst >(c->getArgOperand(0));
          auto* count =
              slot ? llvm::dyn_cast< llvm::ConstantInt >(slot->getArraySize())
                   : nullptr;
          auto* ar_slot =
              a ? dyn_cast< ar::LocalVariable >(ticket_base(a->argument(0)))
                : nullptr;
          if (!a || c->arg_size() != 1 || !slot || !count || !count->isOne() ||
              !slot->getAllocatedType()->isIntegerTy(32) ||
              slot->getAddressSpace() != 0 || !ar_slot ||
              !ar_slot->has_frontend() ||
              ar_slot->frontend< llvm::Value >() != slot ||
              !slots.insert(slot).second)
            return false;
          auto source = ticket_instructions(b);
          auto position = std::find(source.begin(), source.end(), c);
          if (position == source.end() || source.end() - position != 3 ||
              !ticket_marker(position[1], true))
            return false;
          auto* into = llvm::dyn_cast< llvm::BranchInst >(position[2]);
          if (!into || !into->isUnconditional())
            return false;
          auto* header = into->getSuccessor(0);
          auto spin = ticket_instructions(*header);
          if (spin.size() != 4)
            return false;
          auto* ticket = llvm::dyn_cast< llvm::LoadInst >(spin[0]);
          auto* service = llvm::dyn_cast< llvm::LoadInst >(spin[1]);
          auto* equality = llvm::dyn_cast< llvm::ICmpInst >(spin[2]);
          auto* edge = llvm::dyn_cast< llvm::BranchInst >(spin[3]);
          auto* cell = service ? llvm::dyn_cast< llvm::GlobalVariable >(
                                     service->getPointerOperand())
                               : nullptr;
          if (!ticket || ticket->getPointerOperand() != slot || !cell ||
              cell == next || !cell->getValueType()->isIntegerTy(32) ||
              !ticket_counter(cell) || (served && served != cell) ||
              !equality || !edge || !edge->isConditional() ||
              edge->getCondition() != equality ||
              (equality->getPredicate() != llvm::CmpInst::ICMP_NE &&
               equality->getPredicate() != llvm::CmpInst::ICMP_EQ) ||
              !((equality->getOperand(0) == ticket &&
                 equality->getOperand(1) == service) ||
                (equality->getOperand(0) == service &&
                 equality->getOperand(1) == ticket)))
            return false;
          served = cell;
          unsigned equal_edge =
              equality->getPredicate() == llvm::CmpInst::ICMP_EQ ? 0 : 1;
          auto* exit = edge->getSuccessor(equal_edge);
          auto* back = edge->getSuccessor(equal_edge ^ 1);
          auto retry = ticket_instructions(*back);
          auto after = ticket_instructions(*exit);
          auto* repeat = retry.size() == 1
                             ? llvm::dyn_cast< llvm::BranchInst >(retry[0])
                             : nullptr;
          if (!repeat || !repeat->isUnconditional() ||
              repeat->getSuccessor(0) != header || after.empty() ||
              !ticket_marker(after[0], false) ||
              exit->getSinglePredecessor() != header)
            return false;
          std::set< llvm::BasicBlock* > predecessors(llvm::pred_begin(header),
                                                     llvm::pred_end(header));
          if (predecessors != std::set< llvm::BasicBlock* >{&b, back})
            return false;
          for (auto* u : slot->users()) {
            if (u == c || u == ticket || llvm::isa< llvm::DbgInfoIntrinsic >(u))
              continue;
            auto* store = llvm::dyn_cast< llvm::StoreInst >(u);
            if (!store || store->getPointerOperand() != slot ||
                !store->getValueOperand()->getType()->isIntegerTy(32))
              return false;
            rules
                .emplace(store,
                         4); // Reset is legal only before a reservation exists.
          }
          rules.emplace(c, 1);
          rules.emplace(after[0], 2);
          markers.insert(position[1]);
          markers.insert(after[0]);
          served_effects.insert(service);
          ++callers;
        }
    if (!callers || !served)
      return false;
    unsigned releases = 0;
    for (auto* u : served->users()) {
      auto* store = llvm::dyn_cast< llvm::StoreInst >(u);
      if (!store)
        continue;
      auto* load = ticket_increment(store, served);
      if (!load)
        return false;
      auto sequence = ticket_instructions(*store->getParent());
      auto at = std::find(sequence.begin(), sequence.end(), store);
      if (at - sequence.begin() < 3 || sequence.end() - at < 2 ||
          !ticket_marker(at[-3], true) || at[-2] != load ||
          at[-1] != store->getValueOperand() || !ticket_marker(at[1], false))
        return false;
      rules.emplace(store, 3);
      markers.insert(at[-3]);
      markers.insert(at[1]);
      served_effects.insert(load);
      served_effects.insert(store);
      ++releases;
    }
    if (!releases)
      return false;
    for (auto* u : served->users())
      if (!llvm::isa< llvm::Instruction >(u) ||
          !served_effects.count(llvm::cast< llvm::Instruction >(u)))
        return false;
    // Both LLVM and AR must be wholly inside this scalar/private-storage frame.
    for (const auto& item : functions) {
      for (auto& b : *item.first)
        for (auto& i : b) {
          if (!spend())
            return false;
          if (llvm::isa< llvm::LoadInst >(i) ||
              llvm::isa< llvm::StoreInst >(i)) {
            if (!ticket_memory(&i, issuer))
              return false;
          } else if (auto* local = llvm::dyn_cast< llvm::AllocaInst >(&i)) {
            auto* count =
                llvm::dyn_cast< llvm::ConstantInt >(local->getArraySize());
            if (!count || !count->isOne() ||
                !local->getAllocatedType()->isIntegerTy() ||
                local->getAllocatedType()->getIntegerBitWidth() > 64 ||
                local->getAddressSpace() != 0)
              return false;
          } else if (auto* c = llvm::dyn_cast< llvm::CallBase >(&i)) {
            if ((ticket_marker(c, true) || ticket_marker(c, false)) &&
                !markers.count(c))
              return false;
          } else if (!llvm::isa< llvm::DbgInfoIntrinsic >(i) &&
                     !llvm::isa< llvm::BinaryOperator >(i) &&
                     !llvm::isa< llvm::CastInst >(i) &&
                     !llvm::isa< llvm::CmpInst >(i) &&
                     !llvm::isa< llvm::PHINode >(i) &&
                     !llvm::isa< llvm::SelectInst >(i) &&
                     !llvm::isa< llvm::GetElementPtrInst >(i) &&
                     !llvm::isa< llvm::BranchInst >(i) &&
                     !llvm::isa< llvm::SwitchInst >(i) &&
                     !llvm::isa< llvm::ReturnInst >(i) &&
                     !llvm::isa< llvm::UnreachableInst >(i) &&
                     !llvm::isa< llvm::FreezeInst >(i))
            return false;
        }
      for (auto* b : *item.second->body())
        for (auto* s : *b) {
          if (isa< ar::Load >(s) || isa< ar::Store >(s) ||
              isa< ar::CallBase >(s)) {
            if (!s->has_frontend() ||
                original(s->frontend< llvm::Value >()) != s)
              return false;
          } else if (auto* a = dyn_cast< ar::Allocate >(s)) {
            auto* local = a->has_frontend()
                              ? llvm::dyn_cast< llvm::AllocaInst >(
                                    a->frontend< llvm::Value >())
                              : nullptr;
            auto* count = dyn_cast< ar::IntegerConstant >(a->array_size());
            if (!local || !count || count->value().to_z_number() != 1 ||
                !a->allocated_type()->is_integer() ||
                cast< ar::IntegerType >(a->allocated_type())->bit_width() !=
                    local->getAllocatedType()->getIntegerBitWidth())
              return false;
          } else if (!isa< ar::Assignment >(s) &&
                     !isa< ar::UnaryOperation >(s) &&
                     !isa< ar::BinaryOperation >(s) &&
                     !isa< ar::Comparison >(s) && !isa< ar::PointerShift >(s) &&
                     !isa< ar::ReturnValue >(s) && !isa< ar::Unreachable >(s))
            return false;
        }
    }
    // Issued rights are unique in [0,T); retired rights are [0,S). An H right
    // has k=S<T, so its sole release cannot wrap or coexist with another H.
    bool valid = true;
    std::map< std::pair< llvm::Function*, unsigned >, unsigned > summaries;
    std::set< llvm::Function* > entering;
    std::function< unsigned(llvm::Function*, unsigned) > run =
        [&](llvm::Function* f, unsigned entry) {
          auto key = std::make_pair(f, entry);
          auto cache = summaries.find(key);
          if (cache != summaries.end())
            return cache->second;
          if (!entering.insert(f).second || entering.size() > 32) {
            valid = false;
            return 0u;
          }
          std::map< llvm::BasicBlock*, unsigned > inputs;
          std::deque< llvm::BasicBlock* > work{&f->getEntryBlock()};
          inputs[&f->getEntryBlock()] = entry;
          unsigned exits = 0;
          while (valid && !work.empty()) {
            auto* block = work.front();
            work.pop_front();
            auto states = inputs.at(block);
            for (auto& i : *block) {
              if (!spend()) {
                valid = false;
                break;
              }
              if (!states)
                break;
              if (llvm::isa< llvm::LoadInst >(i) ||
                  llvm::isa< llvm::StoreInst >(i)) {
                auto old = token.held.emplace(&i, states == 4);
                if (!old.second)
                  old.first->second &= states == 4;
              }
              auto rule = rules.find(&i);
              if (rule != rules.end()) {
                unsigned need = rule->second == 1 || rule->second == 4 ? 1
                                : rule->second == 2                    ? 2
                                                                       : 4;
                if (states != need) {
                  valid = false;
                  break;
                }
                states = rule->second == 1 ? 2 : rule->second == 2 ? 4 : 1;
              } else if (auto* c = llvm::dyn_cast< llvm::CallBase >(&i)) {
                if (llvm::isa< llvm::DbgInfoIntrinsic >(c))
                  continue;
                if (terminal(c))
                  states = 0;
                else if (!callee(c)->isDeclaration()) {
                  unsigned next = 0;
                  for (unsigned state : {1u, 2u, 4u})
                    if (states & state)
                      next |= run(callee(c), state);
                  states = next;
                }
              }
              if (llvm::isa< llvm::UnreachableInst >(i) && states) {
                valid = false;
                break;
              }
              if (llvm::isa< llvm::ReturnInst >(i)) {
                if (std::find(roots.begin(), roots.end(), f) != roots.end() &&
                    states != 1) {
                  valid = false;
                  break;
                }
                exits |= states;
              }
            }
            for (auto* successor : llvm::successors(block)) {
              auto old = inputs[successor];
              inputs[successor] |= states;
              if (old != inputs[successor])
                work.push_back(successor);
            }
          }
          entering.erase(f);
          summaries.emplace(key, exits);
          return exits;
        };
    for (auto* f : roots) {
      run(f, 1);
      if (!valid)
        return false;
    }
    token.source = served;
    token.issued = next;
    token.issuer = issuer;
    return true;
  }

  llvm::Instruction* access(const AccessRecord& r) const {
    if (!r.stmt->has_frontend() || !r.pts.is_set() || r.pts.size() != 1 || !r.width) return nullptr;
    auto* i = llvm::dyn_cast< llvm::Instruction >(r.stmt->frontend< llvm::Value >());
    auto* p = dyn_cast< GlobalMemoryLocation >(*r.pts.begin());
    if (!i || original(i) != r.stmt || !p || !p->global_var()->has_frontend()) return nullptr;
    auto* g = llvm::dyn_cast< llvm::GlobalVariable >(p->global_var()->frontend< llvm::Value >());
    if (!g || !globals.count(g) || globals.at(g) != p->global_var()) return nullptr;
    auto* l = llvm::dyn_cast< llvm::LoadInst >(i); auto* s = llvm::dyn_cast< llvm::StoreInst >(i);
    auto* pointer = l ? l->getPointerOperand() : s ? s->getPointerOperand() : nullptr;
    auto* type = l ? l->getType() : s ? s->getValueOperand()->getType() : nullptr;
    if (pointer != g || !type || !type->isIntegerTy() || type != g->getValueType() ||
        (l && (r.kind != AccessKind::Read || l->isAtomic())) ||
        (s && (r.kind != AccessKind::Write || s->isAtomic()))) return nullptr;
    auto* a_l = dyn_cast< ar::Load >(r.stmt); auto* a_s = dyn_cast< ar::Store >(r.stmt);
    auto* ar_type = dyn_cast_or_null< ar::IntegerType >(l && a_l ? a_l->result()->type() : s && a_s ? a_s->value()->type() : nullptr);
    if (!ar_type || ar_type->bit_width() != type->getIntegerBitWidth() ||
        (l && (!a_l || !exact_handle_slot(a_l->operand(), p->global_var()))) ||
        (s && (!a_s || !exact_handle_slot(a_s->pointer(), p->global_var())))) return nullptr;
    return i->getModule()->getDataLayout().getTypeStoreSize(type).getFixedValue() == r.width ? i : nullptr;
  }
  const Token* pair(const AccessRecord& a, const AccessRecord& b) const {
    auto* x = access(a); auto* y = access(b);
    if (!x || !y) return nullptr;
    for (const auto& t : tokens) {
      auto p = t.held.find(x); auto q = t.held.find(y);
      if (p != t.held.end() && q != t.held.end() && p->second && q->second) return &t;
      // A complete atomic transition cannot write the token concurrently with
      // a different holder's ordinary token read. Never exempt an unheld read.
      auto held_read = [&](llvm::Instruction* i) {
        auto h = t.held.find(i);
        auto* load = llvm::dyn_cast< llvm::LoadInst >(i);
        return load && load->getPointerOperand() == t.source && h != t.held.end() && h->second;
      };
      if ((llvm::isa< llvm::StoreInst >(x) && t.transition_effects.count(x) && held_read(y)) ||
          (llvm::isa< llvm::StoreInst >(y) && t.transition_effects.count(y) && held_read(x))) return &t;
    }
    return nullptr;
  }
public:
  Impl(Context& context, const std::vector< AccessRecord >&) : ctx(context) {
    if (!ctx.opts.enable_thread_modular || ctx.opts.procedural == Procedural::Intraprocedural ||
        ctx.opts.entry_points.size() != 1 || ctx.opts.entry_points.front()->name() != "main" ||
        ctx.opts.entry_points.front()->num_parameters() != 0 || !ctx.opts.entry_points.front()->has_frontend() ||
        ctx.bundle->data_layout().pointers.bit_width != 32 || !ctx.opts.no_init_globals.empty() ||
        ctx.concurrent_env.is_dirty() || !ctx.concurrent_env.unmodeled_effects().empty() ||
        !ctx.concurrent_env.unmodeled_thread_creations().empty() ||
        ctx.bundle->global_or_null("ar.global_ctors") || ctx.bundle->global_or_null("ar.global_dtors")) return;
    auto* main = ctx.opts.entry_points.front()->frontend< llvm::Function >();
    if (!main->use_empty()) return;
    llvm::Triple target(main->getParent()->getTargetTriple());
    if (target.getArch() != llvm::Triple::x86 || !target.isOSLinux() || target.getEnvironment() != llvm::Triple::GNU) return;
    for (auto it = ctx.bundle->global_begin(); it != ctx.bundle->global_end(); ++it)
      if ((*it)->has_frontend())
        if (auto* g = llvm::dyn_cast< llvm::GlobalVariable >((*it)->frontend< llvm::Value >())) globals.emplace(g, *it);
    for (auto it = ctx.bundle->function_begin(); it != ctx.bundle->function_end(); ++it) {
      auto* f = *it;
      if (!f->is_definition() || !f->has_frontend()) continue;
      auto* source = f->frontend< llvm::Function >(); functions.emplace(source, f); order.push_back(source);
      for (auto* b : *f->body()) for (auto* s : *b) if (s->has_frontend()) {
        if (!isa< ar::CallBase >(s) && !isa< ar::Load >(s) && !isa< ar::Store >(s)) continue;
        auto inserted = originals.emplace(s->frontend< llvm::Value >(), s);
        if (!inserted.second) inserted.first->second = nullptr;
      }
    }
    bool potential = ctx.posix_semaphore() && !ctx.posix_semaphore()->tokens().empty();
    for (auto* f : order) if (atomic_signature(f)) potential = true;
    roots.push_back(main);
    if (!potential || !inventory(*main->getParent(), main)) return;
    for (auto it = ctx.bundle->global_begin(); it != ctx.bundle->global_end(); ++it) {
      auto* a = *it;
      auto* g = a->has_frontend() ? llvm::dyn_cast< llvm::GlobalVariable >(a->frontend< llvm::Value >()) : nullptr;
      if (!token_object(g)) continue;
      Token t; t.source = g;
      if (close_token(t) && ownership(t)) tokens.push_back(std::move(t));
    }
    for (auto* f : order) {
      Token t;
      if (ticket_ownership(f, t)) tokens.push_back(std::move(t));
    }
    if (ctx.posix_semaphore()) for (const auto& sem : ctx.posix_semaphore()->tokens()) {
      Token t; t.source = sem.source; t.semaphore = true; t.operations = sem.operations;
      if (ownership(t)) tokens.push_back(std::move(t));
    }
  }
  BinaryTokenEvent event(llvm::CallBase* call) const {
    for (const auto& token : tokens) {
      if (token.semaphore) continue;
      auto found = token.operations.find(call);
      if (found != token.operations.end()) return {token.source, found->second};
    }
    return {};
  }
  bool excludes(const AccessRecord& a, const AccessRecord& b) const { return pair(a, b); }
  JsonDict info(const AccessRecord& a, const AccessRecord& b) const {
    auto* t = pair(a, b);
    if (t && t->semaphore)
      return {{"safe_reason", "both accesses must hold the unique closed GNU semaphore permit"},
              {"semaphore_ownership_contract", "closed-gnu-binary-semaphore-ownership-v1"},
              {"token", t->source->getName().str()}};
    if (t && t->issued)
      return {{"safe_reason",
               "both accesses own the unique current nonwrapping ticket"},
              {"ticket_ownership_contract",
               "closed-monotone-ticket-ownership-v1"},
              {"issued", t->issued->getName().str()},
              {"served", t->source->getName().str()},
              {"issuer", t->issuer->getName().str()},
              {"counter_bits", 32}};
    const bool observation = t && (t->transition_effects.count(access(a)) ||
                                   t->transition_effects.count(access(b)));
    return t ? JsonDict{{"safe_reason",
                         observation ? "atomic token transition ordered with "
                                       "holder observation"
                                     : "both accesses must own the same closed "
                                       "atomic token"},
                        {"binary_token_contract",
                         observation ? "closed-binary-token-held-observation-v1"
                                     : "closed-binary-token-ownership-v1"},
                        {"token", t->source->getName().str()}}
             : JsonDict{};
  }
};
BinaryTokenOwnership::BinaryTokenOwnership(Context& ctx, const std::vector< AccessRecord >& records)
    : _impl(std::make_unique< Impl >(ctx, records)) {}
BinaryTokenOwnership::~BinaryTokenOwnership() = default;
BinaryTokenEvent BinaryTokenOwnership::event(llvm::CallBase* call) const { return _impl->event(call); }
bool BinaryTokenOwnership::excludes(const AccessRecord& a, const AccessRecord& b) const { return _impl->excludes(a, b); }
JsonDict BinaryTokenOwnership::info(const AccessRecord& a, const AccessRecord& b) const { return _impl->info(a, b); }
} // namespace analyzer
} // namespace ikos
