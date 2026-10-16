// Copyright and license: see data_race.hpp.
#include <ikos/analyzer/checker/once_initialization.hpp>

#include <ikos/analyzer/checker/data_race.hpp>
#include <ikos/analyzer/checker/normal_mutex.hpp>
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
bool plain_call(llvm::CallBase* c, llvm::Function* f) {
  if (!f || !llvm::isa< llvm::CallInst >(c) || c->isInlineAsm() || c->hasOperandBundles() ||
      c->getFunctionType() != f->getFunctionType() || f->isVarArg() ||
      c->getCallingConv() != llvm::CallingConv::C || f->getCallingConv() != llvm::CallingConv::C ||
      c->arg_size() != f->arg_size()) return false;
  for (unsigned i = 0; i < c->arg_size(); ++i)
    for (auto a : {llvm::Attribute::ByVal, llvm::Attribute::StructRet,
                   llvm::Attribute::InAlloca, llvm::Attribute::Preallocated})
      if (c->paramHasAttr(i, a) || f->hasParamAttribute(i, a)) return false;
  return true;
}
struct Gate {
  llvm::GlobalVariable* state = nullptr;
  llvm::GlobalVariable* mutex = nullptr;
  llvm::GlobalVariable* payload = nullptr;
  llvm::CallBase* lock = nullptr;
  llvm::StoreInst* initialization = nullptr;
  llvm::StoreInst* publish = nullptr;
  std::set< llvm::CallBase* > unlocks;
  std::set< llvm::Instruction* > state_effects;
  // Exact critical-prefix edges for the two inductively closed entry states.
  std::map< llvm::Instruction*, std::set< llvm::BasicBlock* > > edges[2];
  std::map< llvm::Instruction*, bool > post;
  bool saw_initialization = false;
};
} // namespace

class OnceInitialization::Impl {
  Context& ctx;
  std::map< llvm::Value*, ar::Statement* > originals;
  std::map< llvm::GlobalVariable*, ar::GlobalVariable* > globals;
  std::map< llvm::Function*, ar::Function* > functions;
  std::vector< llvm::Function* > order, roots;
  std::set< llvm::Function* > reachable;
  std::map< llvm::CallBase*, NormalMutex > mutex_calls;
  std::vector< Gate > gates;
  unsigned budget = 262144;
  bool spend() { if (!budget) return false; --budget; return true; }
  ar::Statement* original(llvm::Value* v) const {
    auto it = originals.find(v); return it == originals.end() ? nullptr : it->second;
  }
  ar::CallBase* binding(llvm::CallBase* c) const {
    auto* a = dyn_cast_or_null< ar::CallBase >(original(c));
    auto* target = a ? native_handle_callee(a) : nullptr;
    auto* f = callee(c);
    return f && target && plain_call(c, f) && a->num_arguments() == c->arg_size() &&
        (f->isDeclaration() ? target->is_declaration() : target->is_definition() &&
         target->has_frontend() && target->frontend< llvm::Value >() == f) ? a : nullptr;
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
        (ctx.native_handle_effect_certificates.count(a) || unread_private_thread_output(a));
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
    if (create(c) || ctx.native_handle_joins.count(a) || ctx.mutex_effect_certificates.count(a)) return true;
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
    std::vector< llvm::Function* > todo = roots;
    while (!todo.empty()) {
      auto* f = todo.back(); todo.pop_back();
      if (!spend()) return false;
      if (!reachable.insert(f).second) continue;
      for (auto& b : *f) for (auto& i : b) if (auto* c = llvm::dyn_cast< llvm::CallBase >(&i)) {
        if (llvm::isa< llvm::DbgInfoIntrinsic >(c)) continue;
        auto* child = callee(c); if (child && !child->isDeclaration()) todo.push_back(child);
      }
    }
    return true;
  }
  bool scalar(llvm::GlobalVariable* g, bool zero32) const {
    if (!g || !globals.count(g) || g->isConstant() || g->isThreadLocal() || g->isExternallyInitialized() ||
        g->getAddressSpace() != 0 || !g->hasInitializer()) return false;
    auto* n = llvm::dyn_cast< llvm::ConstantInt >(g->getInitializer());
    auto* a = globals.at(g); auto* type = dyn_cast< ar::IntegerType >(a->type()->pointee());
    if (!n || !a->is_definition() || a->is_thread_local() || !type ||
        (n->getBitWidth() != 32 && n->getBitWidth() != 64) || type->bit_width() != n->getBitWidth() ||
        (zero32 && (n->getBitWidth() != 32 || !n->isZero()))) return false;
    unsigned count = 0;
    for (auto* b : *a->initializer()) for (auto* s : *b) {
      auto* store = dyn_cast< ar::Store >(s);
      auto* v = store ? dyn_cast< ar::IntegerConstant >(store->value()) : nullptr;
      if (!store || store->pointer() != a || !v || v->type() != type ||
          v->value().to_z_number() != core::ZNumber(n->getSExtValue())) return false;
      ++count;
    }
    return count == 1;
  }
  bool effect(llvm::Instruction* i, llvm::GlobalVariable* g) const {
    if (!globals.count(g)) return false;
    auto* a = globals.at(g);
    if (auto* l = llvm::dyn_cast< llvm::LoadInst >(i)) {
      auto* s = dyn_cast_or_null< ar::Load >(original(i));
      return s && s->operand() == a && s->result()->type() == a->type()->pointee() &&
          l->getPointerOperand() == g && l->getType() == g->getValueType() && !l->isVolatile() && !l->isAtomic();
    }
    auto* s = llvm::dyn_cast< llvm::StoreInst >(i);
    auto* a_s = dyn_cast_or_null< ar::Store >(original(i));
    return s && a_s && a_s->pointer() == a && a_s->value()->type() == a->type()->pointee() &&
        s->getPointerOperand() == g && s->getValueOperand()->getType() == g->getValueType() &&
        !s->isVolatile() && !s->isAtomic();
  }
  bool mutex_uses(Gate& g) {
    std::vector< llvm::Value* > todo{g.mutex}; std::set< llvm::Value* > seen;
    while (!todo.empty()) {
      auto* v = todo.back(); todo.pop_back();
      if (!spend()) return false;
      if (!seen.insert(v).second) continue;
      for (auto* u : v->users()) {
        if (llvm::isa< llvm::BitCastOperator >(u)) { todo.push_back(u); continue; }
        auto* c = llvm::dyn_cast< llvm::CallBase >(u);
        if (!c || c->arg_size() != 1 || c->getArgOperand(0) != v || mutex_call(c).source != g.mutex ||
            !reachable.count(c->getFunction())) return false;
        if (mutex_call(c).lock) { if (g.lock && g.lock != c) return false; g.lock = c; }
        else g.unlocks.insert(c);
      }
    }
    return g.lock && !g.unlocks.empty();
  }
  bool prefix(Gate& g, unsigned entry) {
    using core::ZNumber;
    std::map< llvm::Value*, llvm::APInt > values;
    std::set< llvm::Function* > active;
    unsigned state = entry, initializations = 0, writes = 0;
    bool released = false;
    auto value = [&](llvm::Value* v, llvm::APInt& n) {
      if (auto* c = llvm::dyn_cast< llvm::ConstantInt >(v)) { n = c->getValue(); return true; }
      auto it = values.find(v); if (it == values.end()) return false;
      n = it->second; return true;
    };
    std::function< bool(llvm::Function*, llvm::Instruction*) > run = [&](llvm::Function* f, llvm::Instruction* start) {
      if (active.size() == 64 || !active.insert(f).second) return false;
      auto* b = start->getParent(); llvm::BasicBlock* pred = nullptr;
      std::set< llvm::BasicBlock* > seen;
      while (seen.insert(b).second) {
        std::vector< std::pair< llvm::Value*, llvm::APInt > > phis;
        for (auto& i : *b) if (auto* phi = llvm::dyn_cast< llvm::PHINode >(&i)) {
          llvm::APInt n;
          if (!spend() || !pred || !value(phi->getIncomingValueForBlock(pred), n)) return false;
          phis.emplace_back(phi, n);
        }
        for (auto& p : phis) values[p.first] = p.second;
        bool branched = false;
        for (auto it = start->getIterator(); it != b->end(); ++it) {
          auto* i = &*it;
          if (!spend()) return false;
          if (llvm::isa< llvm::DbgInfoIntrinsic >(i) || llvm::isa< llvm::PHINode >(i)) continue;
          if (llvm::isa< llvm::LoadInst >(i)) {
            if (!effect(i, g.state)) return false;
            g.state_effects.insert(i); values[i] = llvm::APInt(32, state);
          } else if (auto* s = llvm::dyn_cast< llvm::StoreInst >(i)) {
            auto* c = llvm::dyn_cast< llvm::ConstantInt >(s->getValueOperand());
            if (!c) return false;
            if (s->getPointerOperand() == g.state) {
              if (entry != 0 || initializations != 1 || writes++ || !c->isOne() || !effect(i, g.state) ||
                  (g.publish && g.publish != s)) return false;
              auto* a = dyn_cast< ar::Store >(original(s));
              auto* n = a ? dyn_cast< ar::IntegerConstant >(a->value()) : nullptr;
              if (!n || n->value().to_z_number() != 1) return false;
              g.publish = s; state = 1; g.state_effects.insert(i);
            } else {
              auto* payload = llvm::dyn_cast< llvm::GlobalVariable >(s->getPointerOperand());
              if (entry != 0 || writes || initializations++ || !scalar(payload, false) || !effect(i, payload) ||
                  (g.initialization && g.initialization != s)) return false;
              auto* ar_s = dyn_cast< ar::Store >(original(s));
              auto* ar_c = ar_s ? dyn_cast< ar::IntegerConstant >(ar_s->value()) : nullptr;
              if (!ar_c || ar_c->value().to_z_number() != ZNumber(c->getSExtValue())) return false;
              g.initialization = s; g.payload = payload;
            }
          } else if (auto* c = llvm::dyn_cast< llvm::CallBase >(i)) {
            if (unused_freeze(c)) continue;
            auto m = mutex_call(c);
            if (m.source) {
              if (m.source != g.mutex || m.lock || !g.unlocks.count(c) || f != g.lock->getFunction() ||
                  state != 1 || initializations != (entry == 0 ? 1u : 0u) || writes != (entry == 0 ? 1u : 0u)) return false;
              released = true; active.erase(f); return true;
            }
            auto* child = callee(c);
            if (!binding(c) || !child || child->isDeclaration() || !c->use_empty() || c->arg_size() != 0 ||
                !child->getReturnType()->isVoidTy() || !run(child, &child->getEntryBlock().front()) || released) return false;
          } else if (auto* cast = llvm::dyn_cast< llvm::BitCastInst >(i)) {
            if (!cast->getType()->isPointerTy() || cast->getOperand(0)->stripPointerCasts() != g.mutex) return false;
          } else if (auto* cmp = llvm::dyn_cast< llvm::ICmpInst >(i)) {
            llvm::APInt a, b;
            if (!value(cmp->getOperand(0), a) || !value(cmp->getOperand(1), b) || a.getBitWidth() != b.getBitWidth()) return false;
            values[i] = llvm::APInt(1, llvm::ICmpInst::compare(a, b, cmp->getPredicate()));
          } else if (auto* br = llvm::dyn_cast< llvm::BranchInst >(i)) {
            unsigned edge = 0; llvm::APInt n;
            if (br->isConditional()) { if (!value(br->getCondition(), n)) return false; edge = n.isZero() ? 1 : 0; }
            auto* next = br->getSuccessor(edge); g.edges[entry][br].insert(next);
            pred = b; b = next; start = &b->front(); branched = true; break;
          } else if (auto* ret = llvm::dyn_cast< llvm::ReturnInst >(i)) {
            if (f == g.lock->getFunction() || ret->getReturnValue()) return false;
            active.erase(f); return true;
          } else return false;
        }
        if (!branched) return false;
      }
      return false;
    };
    return g.lock->getNextNode() && run(g.lock->getFunction(), g.lock->getNextNode()) && released;
  }
  bool phases(Gate& g) {
    // Bits P=1/H=2; a set of phases is a MAY union. P is a MUST fact only if
    // every observed phase has it. No backedge can supply an entry fact.
    std::map< std::pair< llvm::Function*, unsigned >, unsigned > summaries;
    std::set< llvm::Function* > active;
    bool valid = true;
    std::function< unsigned(llvm::Function*, unsigned) > run = [&](llvm::Function* f, unsigned entry) {
      const auto key = std::make_pair(f, entry);
      auto known = summaries.find(key); if (known != summaries.end()) return known->second;
      if (active.size() == 64 || !active.insert(f).second) { valid = false; return 0u; }
      std::map< llvm::BasicBlock*, unsigned > inputs;
      std::deque< llvm::BasicBlock* > work{&f->getEntryBlock()};
      inputs[&f->getEntryBlock()] = 1u << entry; unsigned exits = 0;
      while (valid && !work.empty()) {
        auto* block = work.front(); work.pop_front(); unsigned states = inputs.at(block);
        for (auto& i : *block) {
          if (!spend()) { valid = false; break; }
          if (!states) break;
          if (effect(&i, g.payload)) {
            if (&i == g.initialization) {
              if (states != (1u << 2)) { valid = false; break; }
              g.saw_initialization = true;
            } else {
              bool post = (states & ((1u << 0) | (1u << 2))) == 0;
              auto inserted = g.post.emplace(&i, post);
              if (!inserted.second) inserted.first->second &= post;
            }
          }
          if (effect(&i, g.state) && (!g.state_effects.count(&i) || (states & 3u))) { valid = false; break; }
          if (auto* c = llvm::dyn_cast< llvm::CallBase >(&i)) {
            if (llvm::isa< llvm::DbgInfoIntrinsic >(c)) continue;
            auto m = mutex_call(c);
            if (m.source == g.mutex) {
              unsigned next = 0;
              for (unsigned s = 0; s < 4; ++s) if (states & (1u << s)) {
                if (m.lock) {
                  if (c != g.lock || (s & 2)) { valid = false; break; }
                  next |= 1u << (s | 2);
                } else {
                  if (!g.unlocks.count(c) || !(s & 2)) { valid = false; break; }
                  next |= 1u << 1;
                }
              }
              states = next;
            } else if (terminal(c)) states = 0;
            else if (auto* child = callee(c)) {
              if (!child->isDeclaration()) {
                unsigned next = 0;
                for (unsigned s = 0; s < 4; ++s) if (states & (1u << s)) next |= run(child, s);
                states = next;
              }
            }
          }
          if (llvm::isa< llvm::UnreachableInst >(i) && states) { valid = false; break; }
          if (llvm::isa< llvm::ReturnInst >(i)) {
            // A helper may transfer global phase P, but never leak ownership G.
            if ((states & ((1u << 2) | (1u << 3))) != (entry & 2 ? states : 0u)) {
              valid = false; break;
            }
            exits |= states;
          }
        }
        auto* term = block->getTerminator();
        for (auto* next : llvm::successors(block)) {
          unsigned edge_states = 0;
          for (unsigned s = 0; s < 4; ++s) if (states & (1u << s)) {
            bool allowed = true;
            if (s & 2) {
              auto e1 = g.edges[1].find(term);
              allowed = e1 != g.edges[1].end() && e1->second.count(next);
              if (!(s & 1)) {
                auto e0 = g.edges[0].find(term);
                allowed |= e0 != g.edges[0].end() && e0->second.count(next);
              }
              // Every held-G branch must belong to a complete prefix trace.
              if (!g.edges[0].count(term) && !g.edges[1].count(term)) { valid = false; break; }
            }
            if (allowed) edge_states |= 1u << s;
          }
          auto before = inputs[next]; inputs[next] |= edge_states;
          if (before != inputs[next]) work.push_back(next);
        }
      }
      active.erase(f); summaries.emplace(key, exits); return exits;
    };
    for (auto* root : roots) { run(root, 0); if (!valid) return false; }
    return valid && g.saw_initialization;
  }
  const Gate* pair(const AccessRecord& a, const AccessRecord& b) const {
    auto source = [&](const AccessRecord& r, const Gate& g) -> llvm::Instruction* {
      if (!r.stmt->has_frontend() || !r.pts.is_set() || r.pts.size() != 1 || !r.width) return nullptr;
      auto* i = llvm::dyn_cast< llvm::Instruction >(r.stmt->frontend< llvm::Value >());
      auto* p = dyn_cast< GlobalMemoryLocation >(*r.pts.begin());
      if (!i || original(i) != r.stmt || !p || p->global_var() != globals.at(g.payload) || !effect(i, g.payload)) return nullptr;
      auto* l = llvm::dyn_cast< llvm::LoadInst >(i);
      if ((l && r.kind != AccessKind::Read) || (!l && r.kind != AccessKind::Write)) return nullptr;
      const auto bytes = i->getModule()->getDataLayout().getTypeStoreSize(g.payload->getValueType()).getFixedValue();
      return bytes == r.width ? i : nullptr;
    };
    for (const auto& g : gates) {
      auto* ia = source(a, g); auto* ib = source(b, g);
      auto post = [&](llvm::Instruction* i) { auto it = g.post.find(i); return it != g.post.end() && it->second; };
      if (ia && ib && ((ia == g.initialization && post(ib)) || (ib == g.initialization && post(ia)))) return &g;
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
    bool potential = false;
    for (auto* f : order) for (auto& b : *f) for (auto& i : b)
      if (auto* c = llvm::dyn_cast< llvm::CallBase >(&i)) if (mutex_call(c).lock) potential = true;
    roots.push_back(main);
    if (!potential || !inventory(*main->getParent(), main)) return;
    // Source order, not pointer order, makes the bounded proof work deterministic.
    for (auto it = ctx.bundle->global_begin(); it != ctx.bundle->global_end(); ++it) {
      auto* a = *it;
      auto* state = a->has_frontend() ? llvm::dyn_cast< llvm::GlobalVariable >(a->frontend< llvm::Value >()) : nullptr;
      if (!scalar(state, true)) continue;
      for (auto* f : order) for (auto& b : *f) for (auto& i : b) {
        auto* c = llvm::dyn_cast< llvm::CallBase >(&i); auto m = c ? mutex_call(c) : NormalMutex{};
        if (!m.lock || !reachable.count(f)) continue;
        Gate g; g.state = state; g.mutex = m.source;
        if (!mutex_uses(g) || g.lock != c || !prefix(g, 0) || !prefix(g, 1) || !g.initialization || !g.publish) continue;
        bool closed = true;
        for (auto* u : state->users()) {
          auto* effect_i = llvm::dyn_cast< llvm::Instruction >(u);
          if (!effect_i || !effect(effect_i, state) || !g.state_effects.count(effect_i)) { closed = false; break; }
        }
        for (auto* u : g.payload->users()) {
          auto* effect_i = llvm::dyn_cast< llvm::Instruction >(u);
          if (!effect_i || !effect(effect_i, g.payload)) { closed = false; break; }
        }
        if (closed && phases(g)) gates.push_back(std::move(g));
      }
    }
  }
  bool ordered(const AccessRecord& a, const AccessRecord& b) const { return pair(a, b); }
  JsonDict info(const AccessRecord& a, const AccessRecord& b) const {
    auto* g = pair(a, b);
    return g ? JsonDict{{"safe_reason", "initialization before sealed mutex gate continuation"},
                        {"once_initialization_contract", "closed-state01-must-postgate-v1"},
                        {"state", g->state->getName().str()}, {"gate_mutex", g->mutex->getName().str()},
                        {"payload", g->payload->getName().str()}} : JsonDict{};
  }
};
OnceInitialization::OnceInitialization(Context& ctx, const std::vector< AccessRecord >& records)
    : _impl(std::make_unique< Impl >(ctx, records)) {}
OnceInitialization::~OnceInitialization() = default;
bool OnceInitialization::ordered(const AccessRecord& a, const AccessRecord& b) const { return _impl->ordered(a, b); }
JsonDict OnceInitialization::info(const AccessRecord& a, const AccessRecord& b) const { return _impl->info(a, b); }
} // namespace analyzer
} // namespace ikos
