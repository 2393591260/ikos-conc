// Copyright and license: see data_race.hpp.
#include <ikos/analyzer/checker/sc_protocol.hpp>

#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <algorithm>
#include <array>
#include <deque>
#include <ikos/analyzer/analysis/execution_engine/pthread_initialization.hpp>
#include <ikos/analyzer/checker/data_race.hpp>
#include <ikos/analyzer/checker/normal_mutex.hpp>
#include <ikos/analyzer/util/log.hpp>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace ikos {
namespace analyzer {
namespace {
llvm::Function* callee(llvm::CallBase* c) {
  return llvm::dyn_cast< llvm::Function >(c->getCalledOperand()->stripPointerCasts());
}
bool plain_call(llvm::CallBase* c, llvm::Function* f) {
  if (!f || !llvm::isa< llvm::CallInst >(c) || c->isInlineAsm() || c->hasOperandBundles() ||
      f->isVarArg() || c->getFunctionType() != f->getFunctionType() ||
      c->getCallingConv() != llvm::CallingConv::C || f->getCallingConv() != llvm::CallingConv::C ||
      c->arg_size() != f->arg_size()) return false;
  for (unsigned i = 0; i < c->arg_size(); ++i)
    for (auto a : {llvm::Attribute::ByVal, llvm::Attribute::StructRet,
                   llvm::Attribute::InAlloca, llvm::Attribute::Preallocated})
      if (c->paramHasAttr(i, a) || f->hasParamAttribute(i, a)) return false;
  return true;
}
// Unknown scalar values do not constrain a branch or atomic write.
struct Value {
  bool known = false;
  uint64_t bits = 0;
  bool operator<(const Value& o) const { return std::tie(known, bits) < std::tie(o.known, o.bits); }
  bool operator==(const Value& o) const { return known == o.known && bits == o.bits; }
};
struct State {
  std::array<unsigned, 2> pc{{0, 0}};
  std::vector<Value> locals;
  std::vector<uint64_t> atomics;
  std::vector<unsigned> releases;
  std::array<unsigned, 2> causal{{0, 0}};
  std::vector< int > mutex_owner;
  std::vector< unsigned > mutex_release;
  unsigned executed = 0;
  bool operator<(const State& o) const {
    return std::tie(pc,
                    locals,
                    atomics,
                    releases,
                    causal,
                    mutex_owner,
                    mutex_release,
                    executed) < std::tie(o.pc,
                                         o.locals,
                                         o.atomics,
                                         o.releases,
                                         o.causal,
                                         o.mutex_owner,
                                         o.mutex_release,
                                         o.executed);
  }
};
} // namespace

class ScProtocol::Impl {
  Context& ctx;
  bool protected_mode;
  std::vector< llvm::GlobalVariable* > mutexes;
  std::map< llvm::CallBase*, std::pair< unsigned, bool > > mutex_events;
  std::map< llvm::GlobalVariable*, unsigned > guard_protectors;
  std::map< llvm::GlobalVariable*, unsigned > all_access_locks;
  std::map< llvm::CallBase*, std::pair< llvm::GlobalVariable*, bool > >
      lifetimes;

  std::map<llvm::Value*, ar::Statement*> originals;
  std::map<llvm::Function*, ar::Function*> functions;
  llvm::Function* main = nullptr;
  std::vector<llvm::Function*> roots;
  std::map<llvm::GlobalVariable*, unsigned> atomic_slots;
  std::map<llvm::GlobalVariable*, bool> atomic_kind;
  std::vector<uint64_t> initial;
  std::map<llvm::Instruction*, unsigned> slots, pcs;
  std::vector<llvm::Instruction*> instructions{nullptr};
  std::map<std::pair<llvm::Instruction*, llvm::Instruction*>, bool> proofs;
  bool valid = false;
  unsigned budget = 262144;
  bool spend() { if (!budget) return false; --budget; return true; }
  ar::Statement* original(llvm::Value* v) const {
    auto it = originals.find(v); return it == originals.end() ? nullptr : it->second;
  }
  ar::CallBase* binding(llvm::CallBase* c) const {
    auto* a = dyn_cast_or_null< ar::CallBase >(original(c));
    auto* target = a ? native_handle_callee(a) : nullptr;
    auto* f = callee(c);
    const bool mapped_mutex =
        protected_mode && f && f->isDeclaration() && target &&
        ((f->getName() == "pthread_mutex_lock" &&
          target->intrinsic_id() == ar::Intrinsic::PthreadMutexLock) ||
         (f->getName() == "pthread_mutex_unlock" &&
          target->intrinsic_id() == ar::Intrinsic::PthreadMutexUnlock));
    if (!f || !target || !isa< ar::Call >(a) || !plain_call(c, f) ||
        (!target->has_frontend() && !mapped_mutex) ||
        (target->has_frontend() && target->frontend< llvm::Value >() != f) ||
        a->num_arguments() != c->arg_size() ||
        target->num_parameters() != a->num_arguments() ||
        target->type()->is_var_arg() ||
        !(f->isDeclaration() ? target->is_declaration()
                             : target->is_definition()))
      return nullptr;
    for (unsigned i = 0; i < a->num_arguments(); ++i)
      if (a->argument(i)->type() != target->type()->param_type(i)) return nullptr;
    return a;
  }
  ar::Variable* address_base(ar::Value* value) const {
    for (unsigned depth = 0; depth < 32; ++depth) {
      if (isa< ar::GlobalVariable >(value) || isa< ar::LocalVariable >(value)) return cast< ar::Variable >(value);
      auto* v = dyn_cast< ar::InternalVariable >(value); auto* def = v ? unique_def(v) : nullptr;
      if (auto* copy = dyn_cast_or_null< ar::Assignment >(def)) value = copy->operand();
      else if (auto* cast = dyn_cast_or_null< ar::UnaryOperation >(def)) {
        if (cast->op() != ar::UnaryOperation::Bitcast) return nullptr;
        value = cast->operand();
      } else if (auto* shift = dyn_cast_or_null< ar::PointerShift >(def)) {
        for (auto it = shift->term_begin(); it != shift->term_end(); ++it) {
          auto* n = dyn_cast< ar::IntegerConstant >((*it).second);
          if (!n || !n->value().is_zero()) return nullptr;
        }
        value = shift->pointer();
      } else return nullptr;
    }
    return nullptr;
  }
  bool pure_call(llvm::CallBase* c) const {
    auto* a = binding(c); auto* f = a ? native_handle_callee(a) : nullptr;
    auto* n = callee(c);
    if (!a || !f || !n) return false;
    if (!n->isDeclaration()) return n != main && std::find(roots.begin(), roots.end(), n) == roots.end();
    if (constant_assertion_effect(a)) return true;
    if (n->getName() == "abort" && f->intrinsic_id() == ar::Intrinsic::LibcAbort &&
        c->arg_empty() && c->getType()->isVoidTy()) return true;
    return c->arg_empty() && c->getMetadata("ikos.freeze_uninit") &&
        f->intrinsic_id() == ar::Intrinsic::IkosNonDet &&
        ((n->getName() == "__ikos_nondet_int" && c->getType()->isIntegerTy(32)) ||
         (n->getName() == "__ikos_nondet_int64" && c->getType()->isIntegerTy(64)));
  }
  bool cyclic(llvm::BasicBlock* start) {
    std::vector<llvm::BasicBlock*> work(llvm::succ_begin(start), llvm::succ_end(start));
    std::set<llvm::BasicBlock*> seen;
    while (!work.empty()) {
      if (!spend()) return true;
      auto* b = work.back(); work.pop_back();
      if (b == start) return true;
      if (seen.insert(b).second) work.insert(work.end(), llvm::succ_begin(b), llvm::succ_end(b));
    }
    return false;
  }
  bool create(llvm::CallBase* c, std::set<llvm::AllocaInst*>& outputs) {
    auto* a = binding(c);
    if (!a || !native_handle_api(c, true) || c->getFunction() != main || cyclic(c->getParent()) ||
        !isa<ar::NullConstant>(a->argument(1)) || !isa<ar::NullConstant>(a->argument(3))) return false;
    auto* slot = llvm::dyn_cast<llvm::AllocaInst>(c->getArgOperand(0)->stripPointerCasts());
    auto* count = slot ? llvm::dyn_cast<llvm::ConstantInt>(slot->getArraySize()) : nullptr;
    auto* child = llvm::dyn_cast<llvm::Function>(c->getArgOperand(2)->stripPointerCasts());
    auto* entry = dyn_cast<ar::FunctionPointerConstant>(a->argument(2));
    auto* output = address_base(a->argument(0));
    if (!slot || !count || !count->isOne() || !slot->getAllocatedType()->isIntegerTy(32) ||
        !output || !isa<ar::LocalVariable>(output) || !output->has_frontend() ||
        output->frontend<llvm::Value>() != slot || !child || !entry ||
        !entry->function()->has_frontend() || entry->function()->frontend<llvm::Value>() != child ||
        !functions.count(child) || std::find(roots.begin(), roots.end(), child) != roots.end() ||
        !outputs.insert(slot).second) return false;
    roots.push_back(child); return true;
  }
  bool handles(const std::set<llvm::AllocaInst*>& outputs) {
    llvm::DominatorTree dominance(*main);
    for (auto* slot : outputs) {
      llvm::CallBase* creator = nullptr;
      for (auto& block : *main) for (auto& i : block) {
        auto* c = llvm::dyn_cast<llvm::CallBase>(&i);
        if (c && native_handle_api(c, true) && c->getArgOperand(0)->stripPointerCasts() == slot) {
          if (creator) return false;
          creator = c;
        }
      }
      if (!creator) return false;
      unsigned joins = 0;
      std::vector<llvm::Value*> work{slot}; std::set<llvm::Value*> seen;
      while (!work.empty()) {
        auto* v = work.back(); work.pop_back();
        if (!seen.insert(v).second) continue;
        if (seen.size() > 1024) return false;
        for (auto* u : v->users()) {
          if (llvm::isa<llvm::BitCastOperator>(u)) { work.push_back(u); continue; }
          if (auto* gep = llvm::dyn_cast<llvm::GEPOperator>(u)) {
            if (gep->getPointerOperand() != v || !gep->hasAllZeroIndices()) return false;
            work.push_back(u); continue;
          }
          if (auto* store = llvm::dyn_cast<llvm::StoreInst>(u)) {
            if (store->getPointerOperand() != v || !store->getValueOperand()->getType()->isIntegerTy(32) ||
                store->getFunction() != main || !dominance.dominates(store, creator)) return false;
            continue;
          }
          if (auto* load = llvm::dyn_cast<llvm::LoadInst>(u)) {
            if (load->getPointerOperand() != v || !load->getType()->isIntegerTy(32) ||
                load->getFunction() != main || !dominance.dominates(creator, load) || cyclic(load->getParent()) || load->user_empty()) return false;
            for (auto* use : load->users()) {
              auto* join = llvm::dyn_cast<llvm::CallBase>(use);
              auto* a = join ? binding(join) : nullptr;
              if (!join || !native_handle_api(join, false) || join->getArgOperand(0) != load ||
                  join->getFunction() != main || cyclic(join->getParent()) || ++joins > 1 || !a || !isa<ar::NullConstant>(a->argument(1))) return false;
            }
            continue;
          }
          if (u != creator || creator->getArgOperand(0) != v) return false;
        }
      }
    }
    // A join of an arbitrary integer is outside this object's effect frame.
    for (auto& b : *main) for (auto& i : b) if (auto* c = llvm::dyn_cast<llvm::CallBase>(&i)) {
      if (!native_handle_api(c, false)) continue;
      auto* load = llvm::dyn_cast<llvm::LoadInst>(c->getArgOperand(0));
      auto* slot = load ? llvm::dyn_cast<llvm::AllocaInst>(load->getPointerOperand()->stripPointerCasts()) : nullptr;
      if (!slot || !outputs.count(slot)) return false;
    }
    return true;
  }

  bool terminal(llvm::CallBase* call) const {
    auto* a = binding(call);
    auto* f = a ? native_handle_callee(a) : nullptr;
    auto* n = callee(call);
    return f && n->isDeclaration() &&
           (constant_assertion_effect(a) ||
            (n->getName() == "abort" && call->arg_empty() &&
             call->getType()->isVoidTy() &&
             f->intrinsic_id() == ar::Intrinsic::LibcAbort));
  }
  bool mutex_call(llvm::CallBase* call, bool root) {
    auto* a = binding(call);
    auto* f = callee(call);
    if (!a || !f->isDeclaration())
      return false;
    if (root) {
      auto m = normal_mutex(ctx, a);
      if (!m.source)
        return false;
      auto it = std::find(mutexes.begin(), mutexes.end(), m.source);
      unsigned index = it - mutexes.begin();
      if (it == mutexes.end())
        mutexes.push_back(m.source);
      if (mutexes.size() > 4)
        return false;
      mutex_events.emplace(call, std::make_pair(index, m.lock));
      return true;
    }
    if (call->getFunction() != main || !call->use_empty() ||
        !call->getType()->isIntegerTy(32))
      return false;
    bool init = f->getName() == "pthread_mutex_init";
    if ((!init && f->getName() != "pthread_mutex_destroy") ||
        call->arg_size() != (init ? 2u : 1u) ||
        (init &&
         (!llvm::isa< llvm::ConstantPointerNull >(call->getArgOperand(1)) ||
          !isa< ar::NullConstant >(a->argument(1)) ||
          !pthread_initialization_signature(a))))
      return false;
    if (!call->getArgOperand(0)->getType()->isPointerTy() ||
        !a->argument(0)->type()->is_pointer())
      return false;
    auto* g = llvm::dyn_cast< llvm::GlobalVariable >(
        call->getArgOperand(0)->stripPointerCasts());
    auto* ag = pthread_initialization_object(a->argument(0), true);
    if (!g || !ag || !ag->has_frontend() ||
        ag->frontend< llvm::Value >() != g || !g->hasInitializer() ||
        !g->getInitializer()->isNullValue())
      return false;
    lifetimes.emplace(call, std::make_pair(g, init));
    return true;
  }
  bool protected_frame() {
    if (mutexes.empty() || !atomic_slots.empty())
      return false;
    llvm::Triple triple(main->getParent()->getTargetTriple());
    if (triple.getArch() != llvm::Triple::x86 || !triple.isOSLinux() ||
        triple.getEnvironment() != llvm::Triple::GNU)
      return false;
    for (auto& b : *main)
      if (cyclic(&b))
        return false;
    llvm::DominatorTree dom(*main);
    std::map< llvm::CallBase*, llvm::CallBase* > completed;
    for (auto& b : *main)
      for (auto& i : b) {
        auto* join = llvm::dyn_cast< llvm::CallBase >(&i);
        if (!join || !native_handle_api(join, false))
          continue;
        auto* load = llvm::dyn_cast< llvm::LoadInst >(join->getArgOperand(0));
        auto* slot = load ? llvm::dyn_cast< llvm::AllocaInst >(
                                load->getPointerOperand()->stripPointerCasts())
                          : nullptr;
        llvm::CallBase* create = nullptr;
        for (auto& bb : *main)
          for (auto& ii : bb)
            if (auto* c = llvm::dyn_cast< llvm::CallBase >(&ii))
              if (native_handle_api(c, true) &&
                  c->getArgOperand(0)->stripPointerCasts() == slot) {
                if (create)
                  return false;
                create = c;
              }
        if (!create || !dom.dominates(create, join) ||
            !completed.emplace(create, join).second)
          return false;
      }
    if (completed.size() != 2)
      return false;
    for (auto* g : mutexes) {
      llvm::CallBase* init = nullptr;
      llvm::CallBase* destroy = nullptr;
      for (const auto& life : lifetimes)
        if (life.second.first == g) {
          auto*& one = life.second.second ? init : destroy;
          if (one)
            return false;
          one = life.first;
        }
      if (!init || !destroy)
        return false;
      for (const auto& finish : completed)
        if (!dom.dominates(init, finish.first) ||
            !dom.dominates(finish.second, destroy))
          return false;
      std::vector< llvm::Value* > pending{g};
      std::set< llvm::Value* > seen;
      while (!pending.empty()) {
        if (!spend())
          return false;
        auto* v = pending.back();
        pending.pop_back();
        if (!seen.insert(v).second)
          continue;
        for (auto* u : v->users()) {
          if (llvm::isa< llvm::BitCastOperator >(u)) {
            pending.push_back(u);
            continue;
          }
          auto* c = llvm::dyn_cast< llvm::CallBase >(u);
          if (!c || c->getArgOperand(0) != v ||
              (!mutex_events.count(c) && c != init && c != destroy))
            return false;
        }
      }
    }
    if (lifetimes.size() != mutexes.size() * 2)
      return false;
    // Main's reads participate in the closure: only quiescent reads are omitted
    // from the independent concurrent ALL-access lock intersection.
    for (auto& b : *main)
      for (auto& i : b)
        if (auto* l = llvm::dyn_cast< llvm::LoadInst >(&i)) {
          if (!llvm::isa< llvm::GlobalVariable >(
                  l->getPointerOperand()->stripPointerCasts()))
            continue;
          bool before = true, after = true;
          for (const auto& finish : completed) {
            before &= dom.dominates(l, finish.first);
            after &= dom.dominates(finish.second, l);
          }
          if (!before && !after)
            return false;
        }
    // Value-independent preproof: cover every CFG branch and normal return.
    for (auto* f : roots) {
      std::deque< std::pair< llvm::BasicBlock*, unsigned > > todo{
          {&f->getEntryBlock(), 0}};
      std::set< std::pair< llvm::BasicBlock*, unsigned > > seen;
      while (!todo.empty()) {
        if (!spend())
          return false;
        auto item = todo.front();
        todo.pop_front();
        if (!seen.insert(item).second)
          continue;
        unsigned held = item.second;
        bool stopped = false;
        for (auto& i : *item.first) {
          if (!spend())
            return false;
          auto* l = llvm::dyn_cast< llvm::LoadInst >(&i);
          auto* s = llvm::dyn_cast< llvm::StoreInst >(&i);
          if (l || s) {
            auto* g = llvm::dyn_cast< llvm::GlobalVariable >(
                (l ? l->getPointerOperand() : s->getPointerOperand())
                    ->stripPointerCasts());
            if (g) {
              auto old = all_access_locks.emplace(g, held);
              if (!old.second)
                old.first->second &= held;
            }
          }
          if (auto* c = llvm::dyn_cast< llvm::CallBase >(&i)) {
            auto event = mutex_events.find(c);
            if (event != mutex_events.end()) {
              unsigned bit = 1u << event->second.first;
              if (event->second.second) {
                if (held & bit)
                  return false;
                held |= bit;
              } else {
                if (!(held & bit))
                  return false;
                held &= ~bit;
              }
            }
            if (terminal(c)) {
              stopped = true;
              break;
            }
          }
          if (llvm::isa< llvm::ReturnInst >(i) && held)
            return false;
        }
        if (!stopped)
          for (auto* next : llvm::successors(item.first))
            todo.emplace_back(next, held);
      }
    }
    for (auto& g : main->getParent()->globals()) {
      auto found = all_access_locks.find(&g);
      if (found == all_access_locks.end() || !found->second)
        continue;
      unsigned mutex = 0;
      while (!(found->second & (1u << mutex)))
        ++mutex;
      auto* native = llvm::dyn_cast< llvm::ConstantInt >(g.getInitializer());
      if (!native)
        return false;
      ar::GlobalVariable* ag = nullptr;
      for (auto it = ctx.bundle->global_begin(); it != ctx.bundle->global_end();
           ++it)
        if ((*it)->has_frontend() && (*it)->frontend< llvm::Value >() == &g)
          ag = *it;
      if (!ag || !ag->is_definition() || ag->is_thread_local())
        return false;
      auto* type = dyn_cast< ar::IntegerType >(ag->type()->pointee());
      if (!type || type->bit_width() != native->getBitWidth())
        return false;
      unsigned count = 0;
      for (auto* b : *ag->initializer())
        for (auto* st : *b) {
          auto* store = dyn_cast< ar::Store >(st);
          auto* v =
              store ? dyn_cast< ar::IntegerConstant >(store->value()) : nullptr;
          if (!store || store->pointer() != ag || !v ||
              v->value().bit_width() != native->getBitWidth() ||
              v->value().to_z_number() !=
                  integer(native->getValue(), type->is_signed()))
            return false;
          ++count;
        }
      if (count != 1)
        return false;
      bool literals = true;
      for (auto* f : roots)
        for (auto& b : *f)
          for (auto& i : b)
            if (auto* store = llvm::dyn_cast< llvm::StoreInst >(&i)) {
              if (store->getPointerOperand()->stripPointerCasts() != &g)
                continue;
              auto* v =
                  llvm::dyn_cast< llvm::ConstantInt >(store->getValueOperand());
              auto* a = dyn_cast< ar::Store >(original(store));
              auto* av = dyn_cast< ar::IntegerConstant >(a->value());
              auto* at = cast< ar::IntegerType >(a->value()->type());
              if (!v || !av || av->value().bit_width() != v->getBitWidth() ||
                  av->value().to_z_number() !=
                      integer(v->getValue(), at->is_signed()))
                literals = false;
            }
      if (!literals)
        continue;
      if (initial.size() == 8)
        return false;
      guard_protectors.emplace(&g, mutex);
      atomic_slots.emplace(&g, initial.size());
      initial.push_back(native->getZExtValue());
    }
    return !guard_protectors.empty();
  }

  core::ZNumber integer(const llvm::APInt& n, bool sign) const {
    llvm::SmallString<32> text; n.toString(text, 10, sign);
    return core::ZNumber::from_string(text.str().str());
  }
  bool memory(llvm::Instruction& i, bool root) {
    auto* l = llvm::dyn_cast<llvm::LoadInst>(&i); auto* s = llvm::dyn_cast<llvm::StoreInst>(&i);
    auto* address = (l ? l->getPointerOperand() : s->getPointerOperand())->stripPointerCasts();
    auto* type = l ? l->getType() : s->getValueOperand()->getType();
    auto* a = original(&i); auto* al = dyn_cast_or_null<ar::Load>(a); auto* as = dyn_cast_or_null<ar::Store>(a);
    auto* at = al ? al->result()->type() : as ? as->value()->type() : nullptr;
    auto* base = al ? address_base(al->operand()) : as ? address_base(as->pointer()) : nullptr;
    if (!type->isIntegerTy() || type->getIntegerBitWidth() > 64 || !at ||
        !at->is_integer() || (l && !al) || (s && !as) || !base ||
        !base->has_frontend() || base->frontend< llvm::Value >() != address ||
        (!protected_mode &&
         cast< ar::PointerType >(base->type())->pointee() != at) ||
        cast< ar::IntegerType >(at)->bit_width() !=
            type->getIntegerBitWidth() ||
        ctx.bundle->data_layout().store_size_in_bytes(at) !=
            i.getModule()
                ->getDataLayout()
                .getTypeStoreSize(type)
                .getFixedValue())
      return false;
    bool atomic = l ? l->isAtomic() : s->isAtomic();
    if (atomic != (al ? al->is_atomic() : as->is_atomic())) return false;
    if (protected_mode && atomic)
      return false;
    if (atomic && ((l ? l->getOrdering() : s->getOrdering()) != llvm::AtomicOrdering::SequentiallyConsistent ||
        (l ? l->getSyncScopeID() : s->getSyncScopeID()) != llvm::SyncScope::System ||
        (al ? al->ordering() : as->ordering()) != ar::AtomicOrdering::SeqCst)) return false;
    if (auto* g = llvm::dyn_cast<llvm::GlobalVariable>(address)) {
      if (g->getValueType() != type || g->isConstant() || g->isThreadLocal() ||
          g->getAddressSpace() != 0 || !g->hasInitializer() || g->isExternallyInitialized() ||
          !llvm::isa<llvm::ConstantInt>(g->getInitializer()) || (!root && s)) return false;
      auto kind = atomic_kind.emplace(g, atomic);
      if (!kind.second && kind.first->second != atomic) return false;
      if (atomic && !atomic_slots.count(g)) {
        if (initial.size() == 8) return false;
        auto* ag = dyn_cast<ar::GlobalVariable>(base);
        if (!ag || !ag->is_definition() || ag->is_thread_local()) return false;
        unsigned n = 0;
        for (auto* b : *ag->initializer()) for (auto* st : *b) {
          auto* store = dyn_cast<ar::Store>(st);
          auto* value = store ? dyn_cast<ar::IntegerConstant>(store->value()) : nullptr;
          auto native = llvm::cast<llvm::ConstantInt>(g->getInitializer())->getValue();
          if (!store || store->pointer() != ag || !value || value->value().bit_width() != native.getBitWidth() ||
              value->value().to_z_number() != integer(native, cast<ar::IntegerType>(at)->is_signed())) return false;
          ++n;
        }
        if (n != 1) return false;
        atomic_slots.emplace(g, initial.size());
        initial.push_back(llvm::cast<llvm::ConstantInt>(g->getInitializer())->getZExtValue());
      }
      if (atomic && s) {
        auto* value = llvm::dyn_cast<llvm::ConstantInt>(s->getValueOperand());
        auto* av = dyn_cast<ar::IntegerConstant>(as->value());
        if (!value || !av || av->value().bit_width() != value->getBitWidth()) return false;
        llvm::SmallString<32> text;
        value->getValue().toString(text, 10, cast<ar::IntegerType>(at)->is_signed());
        if (av->value().to_z_number() != core::ZNumber::from_string(text.str().str())) return false;
      }
      return true;
    }
    auto* local = llvm::dyn_cast<llvm::AllocaInst>(address);
    auto* count = local ? llvm::dyn_cast<llvm::ConstantInt>(local->getArraySize()) : nullptr;
    return !atomic && local && count && count->isOne() && local->getAllocatedType() == type &&
           local->getAddressSpace() == 0;
  }
  bool inventory() {
    auto& module = *main->getParent();
    if (!module.getModuleInlineAsm().empty() || !mutex_effect_pointer_representations_closed(module)) return false;
    std::set<llvm::AllocaInst*> outputs;
    for (auto& b : *main) for (auto& i : b) if (auto* c = llvm::dyn_cast<llvm::CallBase>(&i)) {
      auto* f = callee(c);
      if (f && f->getName() == "pthread_create" && !create(c, outputs)) return false;
    }
    if (roots.size() != 2 || !handles(outputs)) return false;
    std::set<llvm::Value*> consumed;
    for (auto& f : module) {
      if (f.isDeclaration()) continue;
      if (!functions.count(&f) || f.isVarArg() || f.getCallingConv() != llvm::CallingConv::C) return false;
      bool root = std::find(roots.begin(), roots.end(), &f) != roots.end();
      for (unsigned k = 0; k < f.arg_size(); ++k)
        for (auto a : {llvm::Attribute::ByVal, llvm::Attribute::StructRet,
                       llvm::Attribute::InAlloca, llvm::Attribute::Preallocated})
          if (f.hasParamAttribute(k, a)) return false;
      std::vector<llvm::Value*> uses{&f}; std::set<llvm::Value*> seen;
      while (!uses.empty()) {
        if (!spend()) return false;
        auto* v = uses.back(); uses.pop_back();
        if (!seen.insert(v).second) continue;
        for (auto* u : v->users()) {
          if (llvm::isa<llvm::BitCastOperator>(u)) { uses.push_back(u); continue; }
          auto* c = llvm::dyn_cast<llvm::CallBase>(u);
          if (!c || !binding(c)) return false;
          if (c->getCalledOperand() == v && !root && &f != main) continue;
          if (!root || c->getFunction() != main || !native_handle_api(c, true) || c->getArgOperand(2) != v) return false;
        }
      }
      for (auto& b : f) for (auto& i : b) {
        if (!spend()) return false;
        if (llvm::isa<llvm::DbgInfoIntrinsic>(i)) continue;
        if (llvm::isa<llvm::LoadInst>(i) || llvm::isa<llvm::StoreInst>(i)) {
          if ((!root && &f != main) || !memory(i, root)) return false;
          consumed.insert(&i);
        } else if (auto* c = llvm::dyn_cast<llvm::CallBase>(&i)) {
          auto* a = binding(c); if (!a) return false;
          consumed.insert(c);
          bool thread = &f == main &&
              ((native_handle_api(c, true) && outputs.count(llvm::dyn_cast<llvm::AllocaInst>(c->getArgOperand(0)->stripPointerCasts()))) ||
               (native_handle_api(c, false) && isa<ar::NullConstant>(a->argument(1))));
          if (!thread && !(protected_mode && mutex_call(c, root)) &&
              !pure_call(c))
              return false;
        } else if (auto* local = llvm::dyn_cast<llvm::AllocaInst>(&i)) {
          auto* count = llvm::dyn_cast<llvm::ConstantInt>(local->getArraySize());
          auto* a = dyn_cast_or_null<ar::Allocate>(original(local));
          auto* n = a ? dyn_cast<ar::IntegerConstant>(a->array_size()) : nullptr;
          if ((!root && &f != main) || !count || !count->isOne() ||
              !local->getAllocatedType()->isIntegerTy() || local->getAllocatedType()->getIntegerBitWidth() > 64 ||
              !a || !n || n->value().to_z_number() != 1 || !a->allocated_type()->is_integer() ||
              cast<ar::IntegerType>(a->allocated_type())->bit_width() != local->getAllocatedType()->getIntegerBitWidth() ||
              !a->result()->has_frontend() || a->result()->frontend<llvm::Value>() != local) return false;
          consumed.insert(local);
        } else if (llvm::isa<llvm::UnreachableInst>(i)) {
          // Helpers are projected as may-return, including their abort paths.
          auto* previous =
              llvm::dyn_cast_or_null< llvm::CallBase >(i.getPrevNode());
          if ((root || &f == main) &&
              !(protected_mode && previous && terminal(previous)))
              return false;
        } else if (!llvm::isa<llvm::BinaryOperator>(i) && !llvm::isa<llvm::CastInst>(i) &&
                   !llvm::isa<llvm::CmpInst>(i) && !llvm::isa<llvm::PHINode>(i) &&
                   !llvm::isa<llvm::SelectInst>(i) && !llvm::isa<llvm::GetElementPtrInst>(i) &&
                   !llvm::isa<llvm::BranchInst>(i) && !llvm::isa<llvm::SwitchInst>(i) &&
                   !llvm::isa<llvm::ReturnInst>(i) && !llvm::isa<llvm::FreezeInst>(i)) return false;
      }
    }
    for (const auto& item : originals) if (!consumed.count(item.first)) return false;
    if (protected_mode ? !protected_frame() : atomic_slots.empty())
      return false;
    for (auto* f : roots) for (auto& b : *f) for (auto& i : b) {
      pcs.emplace(&i, instructions.size()); instructions.push_back(&i);
      if (llvm::isa<llvm::LoadInst>(i) || llvm::isa<llvm::PHINode>(i)) slots.emplace(&i, slots.size());
    }
    return true;
  }
  Value evaluate(llvm::Value* v, const State& s, unsigned depth = 0) const {
    if (depth == 32) return {};
    if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(v))
      return c->getBitWidth() <= 64 ? Value{true, c->getZExtValue()} : Value{};
    auto* i = llvm::dyn_cast<llvm::Instruction>(v);
    if (!i) return {};
    auto saved = slots.find(i);
    if (saved != slots.end()) return s.locals[saved->second];
    auto operand = [&](unsigned n) { return evaluate(i->getOperand(n), s, depth + 1); };
    if (auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(i)) {
      auto a = operand(0), b = operand(1); auto* t = cmp->getOperand(0)->getType();
      if (!a.known || !b.known || !t->isIntegerTy() || t->getIntegerBitWidth() > 64) return {};
      return {true, llvm::ICmpInst::compare(llvm::APInt(t->getIntegerBitWidth(), a.bits),
                                           llvm::APInt(t->getIntegerBitWidth(), b.bits), cmp->getPredicate())};
    }
    if (auto* cast = llvm::dyn_cast<llvm::CastInst>(i)) {
      auto a = operand(0); auto* from = cast->getOperand(0)->getType(); auto* to = cast->getType();
      if (!a.known || !from->isIntegerTy() || !to->isIntegerTy() ||
          from->getIntegerBitWidth() > 64 || to->getIntegerBitWidth() > 64) return {};
      llvm::APInt n(from->getIntegerBitWidth(), a.bits);
      if (cast->getOpcode() == llvm::Instruction::SExt) n = n.sext(to->getIntegerBitWidth());
      else if (cast->getOpcode() == llvm::Instruction::ZExt) n = n.zext(to->getIntegerBitWidth());
      else if (cast->getOpcode() == llvm::Instruction::Trunc) n = n.trunc(to->getIntegerBitWidth());
      else if (cast->getOpcode() != llvm::Instruction::BitCast || from != to) return {};
      return {true, n.getZExtValue()};
    }
    if (llvm::isa<llvm::SelectInst>(i)) {
      auto c = operand(0), a = operand(1), b = operand(2);
      if (c.known) return c.bits ? a : b;
      return a == b ? a : Value{};
    }
    if (auto* op = llvm::dyn_cast<llvm::BinaryOperator>(i)) {
      auto a = operand(0), b = operand(1);
      if (!op->getType()->isIntegerTy(1) || !a.known || !b.known) return {};
      if (op->getOpcode() == llvm::Instruction::And) return {true, a.bits & b.bits};
      if (op->getOpcode() == llvm::Instruction::Or) return {true, a.bits | b.bits};
      if (op->getOpcode() == llvm::Instruction::Xor) return {true, a.bits ^ b.bits};
    }
    return {};
  }
  void enter(State& s, unsigned thread, llvm::BasicBlock* from, llvm::BasicBlock* to) const {
    std::vector<std::pair<unsigned, Value>> updates;
    for (auto& i : *to) {
      auto* phi = llvm::dyn_cast<llvm::PHINode>(&i); if (!phi) break;
      updates.emplace_back(slots.at(phi), evaluate(phi->getIncomingValueForBlock(from), s));
    }
    for (const auto& item : updates) s.locals[item.first] = item.second;
    auto* first = to->getFirstNonPHI(); s.pc[thread] = first ? pcs.at(first) : 0;
  }
  bool explore(llvm::Instruction* a, llvm::Instruction* b) {
    State initial_state;
    initial_state.locals.resize(slots.size());
    initial_state.atomics = initial;
    initial_state.releases.resize(initial.size());
    if (protected_mode) {
      initial_state.mutex_owner.resize(mutexes.size(), -1);
      initial_state.mutex_release.resize(mutexes.size());
    }
    for (unsigned t = 0; t < 2; ++t)
      initial_state.pc[t] = pcs.at(&roots[t]->getEntryBlock().front());
    std::set< State > seen{initial_state};
    std::deque< State > work{initial_state};
    while (!work.empty()) {
      auto before = std::move(work.front());
      work.pop_front();
      for (unsigned t = 0; t < 2; ++t) {
        if (!before.pc[t])
          continue;
        if (!spend() || seen.size() > 65536)
          return false;
        auto s = before;
        auto* i = instructions[s.pc[t]];
        if (protected_mode)
          if (auto* call = llvm::dyn_cast< llvm::CallBase >(i)) {
            auto event = mutex_events.find(call);
            if (event != mutex_events.end()) {
              auto n = event->second.first;
              if (event->second.second) {
                if (s.mutex_owner[n] >= 0)
                  continue;
                s.mutex_owner[n] = t;
                s.causal[t] |= s.mutex_release[n];
              } else {
                if (s.mutex_owner[n] != static_cast< int >(t))
                  return false;
                s.mutex_owner[n] = -1;
                s.mutex_release[n] = s.causal[t];
              }
            }
            if (terminal(call)) {
              s.pc[t] = 0;
              if (seen.insert(s).second)
                work.push_back(std::move(s));
              continue;
            }
          }
        unsigned event = i == a ? 1 : i == b ? 2 : 0;
        if (event) {
          unsigned other = event ^ 3;
          if ((s.executed & other) && !(s.causal[t] & other))
            return false;
          // Endpoint source blocks are acyclic: these bits name unique events.
          if (s.executed & event)
            return false;
          s.executed |= event;
          s.causal[t] |= event;
        }
        std::vector< State > successors;
        if (auto* branch = llvm::dyn_cast< llvm::BranchInst >(i)) {
          auto condition = branch->isConditional()
                               ? evaluate(branch->getCondition(), s)
                               : Value{};
          for (unsigned k = 0; k < branch->getNumSuccessors(); ++k) {
            if (branch->isConditional() && condition.known &&
                k != (condition.bits ? 0u : 1u))
              continue;
            auto next = s;
            enter(next, t, i->getParent(), branch->getSuccessor(k));
            successors.push_back(std::move(next));
          }
        } else if (auto* sw = llvm::dyn_cast< llvm::SwitchInst >(i)) {
          auto v = evaluate(sw->getCondition(), s);
          llvm::BasicBlock* selected = sw->getDefaultDest();
          if (v.known)
            for (auto c : sw->cases())
              if (c.getCaseValue()->getValue().getLimitedValue() == v.bits)
                selected = c.getCaseSuccessor();
          for (unsigned k = 0; k < sw->getNumSuccessors(); ++k) {
            if (v.known && sw->getSuccessor(k) != selected)
              continue;
            auto next = s;
            enter(next, t, i->getParent(), sw->getSuccessor(k));
            successors.push_back(std::move(next));
          }
        } else {
          if (auto* l = llvm::dyn_cast< llvm::LoadInst >(i)) {
            Value value;
            auto* global = llvm::dyn_cast< llvm::GlobalVariable >(
                l->getPointerOperand()->stripPointerCasts());
            if (protected_mode && global && guard_protectors.count(global)) {
              auto m = guard_protectors.at(global);
              if (s.mutex_owner[m] != static_cast< int >(t))
                return false;
              value = {true, s.atomics[atomic_slots.at(global)]};
            } else if (l->isAtomic()) {
              auto n = atomic_slots.at(llvm::cast< llvm::GlobalVariable >(
                  l->getPointerOperand()->stripPointerCasts()));
              value = {true, s.atomics[n]};
              s.causal[t] |= s.releases[n];
            }
            s.locals[slots.at(l)] = value;
          } else if (auto* store = llvm::dyn_cast< llvm::StoreInst >(i)) {
            auto* global = llvm::dyn_cast< llvm::GlobalVariable >(
                store->getPointerOperand()->stripPointerCasts());
            if (protected_mode && global && guard_protectors.count(global)) {
              auto m = guard_protectors.at(global);
              if (s.mutex_owner[m] != static_cast< int >(t))
                return false;
              s.atomics[atomic_slots.at(global)] =
                  llvm::cast< llvm::ConstantInt >(store->getValueOperand())
                      ->getZExtValue();
            } else if (store->isAtomic()) {
              auto n = atomic_slots.at(llvm::cast< llvm::GlobalVariable >(
                  store->getPointerOperand()->stripPointerCasts()));
              s.atomics[n] =
                  llvm::cast< llvm::ConstantInt >(store->getValueOperand())
                      ->getZExtValue();
              s.releases[n] = s.causal[t];
            }
          }
          auto* next = i->getNextNode();
          s.pc[t] = next ? pcs.at(next) : 0;
          successors.push_back(std::move(s));
        }
        for (auto& next : successors)
          if (seen.insert(next).second)
            work.push_back(std::move(next));
      }
    }
    return true;
  }
  llvm::Instruction* endpoint(const AccessRecord& r) {
    if (!r.stmt->has_frontend() || !r.width) return nullptr;
    auto* i = llvm::dyn_cast<llvm::Instruction>(r.stmt->frontend<llvm::Value>());
    if (!i || original(i) != r.stmt || std::find(roots.begin(), roots.end(), i->getFunction()) == roots.end() ||
        cyclic(i->getParent())) return nullptr;
    auto* l = llvm::dyn_cast<llvm::LoadInst>(i); auto* s = llvm::dyn_cast<llvm::StoreInst>(i);
    if (protected_mode &&
        (!r.call_context || !r.call_context->empty() ||
         r.thread_id != i->getFunction()->getName().str() ||
         ((!l && !s) ||
          r.width != i->getModule()
                         ->getDataLayout()
                         .getTypeStoreSize(l ? l->getType()
                                             : s->getValueOperand()->getType())
                         .getFixedValue())))
      return nullptr;
    if ((l && !l->isAtomic() && r.kind == AccessKind::Read) ||
        (s && !s->isAtomic() && r.kind == AccessKind::Write)) return i;
    return nullptr;
  }
public:
  explicit Impl(Context& context, bool mode = false)
      : ctx(context), protected_mode(mode) {
    if (!ctx.opts.enable_thread_modular || ctx.opts.procedural == Procedural::Intraprocedural ||
        ctx.opts.entry_points.size() != 1 || ctx.opts.entry_points.front()->name() != "main" ||
        ctx.opts.entry_points.front()->num_parameters() != 0 || !ctx.opts.entry_points.front()->has_frontend() ||
        ctx.bundle->data_layout().pointers.bit_width != 32 || !ctx.opts.no_init_globals.empty() ||
        ctx.concurrent_env.is_dirty() || !ctx.concurrent_env.unmodeled_effects().empty() ||
        !ctx.concurrent_env.unmodeled_thread_creations().empty() ||
        ctx.bundle->global_or_null("ar.global_ctors") || ctx.bundle->global_or_null("ar.global_dtors")) return;
    main = ctx.opts.entry_points.front()->frontend<llvm::Function>();
    if (!main->use_empty()) return;
    for (auto it = ctx.bundle->function_begin(); it != ctx.bundle->function_end(); ++it) {
      auto* f = *it; if (!f->is_definition() || !f->has_frontend()) continue;
      auto* source = f->frontend<llvm::Function>(); functions.emplace(source, f);
      for (auto* b : *f->body()) for (auto* s : *b) {
        if (isa<ar::CallBase>(s) || isa<ar::Load>(s) || isa<ar::Store>(s) || isa<ar::Allocate>(s)) {
          auto* i = s->has_frontend() ? llvm::dyn_cast<llvm::Instruction>(s->frontend<llvm::Value>()) : nullptr;
          if (!i || i->getFunction() != source || !originals.emplace(i, s).second) return;
        } else if (!isa<ar::Assignment>(s) && !isa<ar::UnaryOperation>(s) &&
                   !isa<ar::BinaryOperation>(s) && !isa<ar::Comparison>(s) &&
                   !isa<ar::PointerShift>(s) && !isa<ar::ReturnValue>(s) &&
                   !isa<ar::Unreachable>(s)) return;
      }
    }
    valid = inventory();
    if (!valid) log::debug("SC protocol inventory refused");
  }
  bool supported() const { return valid; }
  Context& context() const { return ctx; }
  bool ordered(const AccessRecord& a, const AccessRecord& b) {
    if (!valid) return false;
    auto* ia = endpoint(a); auto* ib = endpoint(b);
    if (!ia || !ib) return false;
    auto key = std::make_pair(ia, ib); auto known = proofs.find(key);
    if (known != proofs.end()) return known->second;
    bool ordered = ia->getFunction() == ib->getFunction() || explore(ia, ib);
    proofs.emplace(key, ordered); return ordered;
  }
  JsonDict info(const AccessRecord&, const AccessRecord&) const {
    if (protected_mode) {
      JsonList guards;
      std::vector< std::pair< std::string, std::string > > labels;
      for (const auto& item : guard_protectors)
        labels.emplace_back(item.first->getName().str(),
                            mutexes[item.second]->getName().str());
      std::sort(labels.begin(), labels.end());
      for (const auto& item : labels)
        guards.add(JsonDict{{"cell", item.first}, {"mutex", item.second}});
      return {{"safe_reason",
               "protected scalar protocol fixed point excludes unordered "
               "target events"},
              {"protected_protocol_contract",
               "closed-two-root-protected-scalar-mutex-hb-v1"},
              {"guards", guards},
              {"scope",
               "GNU default mutex and ordinary join; inherited no-child effect "
               "envelope"}};
    }
    return {{"safe_reason",
             "finite SC protocol fixed point excludes unordered target events"},
            {"sc_protocol_contract", "closed-two-root-system-sc-scalar-hb-v1"}};
  }
};
ScProtocol::ScProtocol(Context& ctx) : _impl(std::make_unique<Impl>(ctx)) {}
ScProtocol::~ScProtocol() = default;
bool ScProtocol::ordered(const AccessRecord& a, const AccessRecord& b) const {
  if (_impl->supported())
    return _impl->ordered(a, b);
  if (!_protected)
    _protected = std::make_unique< Impl >(_impl->context(), true);
  return _protected->ordered(a, b);
}
JsonDict ScProtocol::info(const AccessRecord& a, const AccessRecord& b) const {
  return _protected ? _protected->info(a, b) : _impl->info(a, b);
}
} // namespace analyzer
} // namespace ikos
