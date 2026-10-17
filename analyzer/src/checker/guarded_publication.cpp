// Copyright and license: see data_race.hpp.
#include <ikos/analyzer/checker/guarded_publication.hpp>
#include <ikos/analyzer/checker/data_race.hpp>
#include <ikos/analyzer/checker/normal_mutex.hpp>
#include <ikos/analyzer/analysis/source_call_binding.hpp>
#include <deque>
#include <map>
#include <set>

namespace ikos {
namespace analyzer {
namespace {
struct State {
  unsigned held = 1; // Possible U/H states. Joins never invent MUST ownership.
  bool seen = false;
  std::set< llvm::LoadInst* > guarded;
  bool operator==(const State& s) const { return held == s.held && seen == s.seen && guarded == s.guarded; }
  void join(const State& s) {
    held |= s.held; seen &= s.seen;
    for (auto i = guarded.begin(); i != guarded.end();)
      if (!s.guarded.count(*i)) i = guarded.erase(i); else ++i;
  }
};
struct Observation { llvm::LoadInst* load = nullptr; bool positive = true; };
struct Proof {
  llvm::GlobalVariable* flag;
  llvm::GlobalVariable* mutex;
  std::set< ar::Statement* > writes, reads;
};
}
class GuardedPublication::Impl {
  Context& ctx;
  llvm::Function* main = nullptr;
  std::map< llvm::Value*, ar::Statement* > originals;
  std::map< llvm::Function*, ar::Function* > functions;
  std::map< llvm::GlobalVariable*, ar::GlobalVariable* > globals;
  std::set< llvm::Function* > workers;
  std::map< llvm::CallBase*, NormalMutex > locks;
  std::set< llvm::CallBase* > waits;
  llvm::GlobalVariable* mutex = nullptr;
  std::vector< Proof > proofs;
  unsigned budget = 65536;
  bool step() { if (!budget) return false; --budget; return true; }
  llvm::Function* callee(llvm::CallBase* c) const {
    return llvm::dyn_cast< llvm::Function >(c->getCalledOperand()->stripPointerCasts());
  }
  ar::Statement* original(llvm::Value* v) const {
    auto i = originals.find(v); return i == originals.end() ? nullptr : i->second;
  }
  ar::CallBase* binding(llvm::CallBase* c) const { return source_call_binding(c, original(c)); }
  bool terminal(llvm::CallBase* c) const {
    auto* a = binding(c); auto* f = callee(c);
    if (!a || !f || !f->isDeclaration()) return false;
    auto* t = native_handle_callee(a);
    return (f->getName() == "abort" && t->intrinsic_id() == ar::Intrinsic::LibcAbort &&
            c->arg_empty() && c->getType()->isVoidTy()) || constant_assertion_effect(a);
  }
  bool root(llvm::Function* f) const { return f == main || workers.count(f); }
  bool zero(llvm::GlobalVariable* g) const {
    auto i = globals.find(g);
    if (i == globals.end() || !i->second->is_definition() || i->second->is_thread_local() ||
        g->isConstant() || g->isThreadLocal() || g->isExternallyInitialized() ||
        g->getAddressSpace() != 0 || !g->hasInitializer() || !g->getInitializer()->isNullValue()) return false;
    auto* a = i->second; unsigned count = 0;
    if (ctx.bundle->data_layout().alloc_size_in_bytes(a->type()->pointee()) !=
        main->getParent()->getDataLayout().getTypeAllocSize(g->getValueType()).getFixedValue()) return false;
    for (auto* b : *a->initializer()) for (auto* s : *b) {
      auto* st = dyn_cast< ar::Store >(s);
      if (!st || st->pointer() != a || st->value()->type() != a->type()->pointee()) return false;
      auto* n = dyn_cast< ar::IntegerConstant >(st->value());
      if (!(n && n->value().is_zero()) && !isa< ar::AggregateZeroConstant >(st->value())) return false;
      ++count;
    }
    return count == 1;
  }
  bool scalar(llvm::Instruction* i, llvm::GlobalVariable* g) const {
    auto* l = llvm::dyn_cast< llvm::LoadInst >(i); auto* s = llvm::dyn_cast< llvm::StoreInst >(i);
    auto* type = l ? l->getType() : s ? s->getValueOperand()->getType() : nullptr;
    auto* a = original(i); auto* al = dyn_cast_or_null< ar::Load >(a); auto* as = dyn_cast_or_null< ar::Store >(a);
    auto* at = al ? dyn_cast< ar::IntegerType >(al->result()->type()) : as ? dyn_cast< ar::IntegerType >(as->value()->type()) : nullptr;
    if (!type || !type->isIntegerTy() || type != g->getValueType() || !at ||
        at->bit_width() != type->getIntegerBitWidth() || !globals.count(g) ||
        (l && (!al || l->getPointerOperand() != g || l->isAtomic() || l->isVolatile())) ||
        (s && (!as || s->getPointerOperand() != g || s->isAtomic() || s->isVolatile()))) return false;
    return exact_handle_slot(al ? al->operand() : as->pointer(), globals.at(g)) &&
        ctx.bundle->data_layout().store_size_in_bytes(at) ==
            main->getParent()->getDataLayout().getTypeStoreSize(type).getFixedValue();
  }
  bool reaches(llvm::Instruction* from, llvm::Instruction* to) {
    if (from->getParent() == to->getParent()) {
      for (auto* i = from->getNextNode(); i; i = i->getNextNode()) if (i == to) return true;
    }
    std::vector< llvm::BasicBlock* > todo(llvm::succ_begin(from->getParent()), llvm::succ_end(from->getParent()));
    std::set< llvm::BasicBlock* > seen;
    while (!todo.empty()) {
      auto* b = todo.back(); todo.pop_back();
      if (!step()) return true;
      if (b == to->getParent()) return true;
      if (seen.insert(b).second) todo.insert(todo.end(), llvm::succ_begin(b), llvm::succ_end(b));
    }
    return false;
  }
  bool condition(llvm::CallBase* c) {
    auto* f = callee(c); auto* a = binding(c);
    const bool wait = f->getName() == "pthread_cond_wait";
    if (!root(c->getFunction()) || !a || !f->isDeclaration() || !c->use_empty() ||
        !c->getType()->isIntegerTy(32) || c->arg_size() != (wait ? 2u : 1u)) return false;
    auto* g = llvm::dyn_cast< llvm::GlobalVariable >(c->getArgOperand(0)->stripPointerCasts());
    auto* p = llvm::dyn_cast< llvm::PointerType >(c->getArgOperand(0)->getType());
    auto* t = p ? llvm::dyn_cast< llvm::StructType >(p->getPointerElementType()) : nullptr;
    if (!g || !t || t->isOpaque() || !t->hasName() || t->getName() != "union.pthread_cond_t" ||
        !zero(g) || main->getParent()->getDataLayout().getTypeAllocSize(g->getValueType()) != 48 ||
        main->getParent()->getDataLayout().getTypeAllocSize(t) != 48 ||
        !exact_handle_slot(a->argument(0), globals.at(g))) return false;
    auto align = g->getAlign() ? g->getAlign()->value() : main->getParent()->getDataLayout().getABITypeAlign(g->getValueType()).value();
    if (align < 4) return false;
    if (wait) {
      if (c->getArgOperand(1)->stripPointerCasts() != mutex ||
          !exact_handle_slot(a->argument(1), globals.at(mutex))) return false;
      waits.insert(c);
    }
    return closed_control(g, false);
  }
  bool closed_control(llvm::GlobalVariable* g, bool is_mutex) const {
    std::vector< llvm::Value* > todo{g}; std::set< llvm::Value* > seen;
    while (!todo.empty()) {
      auto* v = todo.back(); todo.pop_back();
      if (!seen.insert(v).second) continue;
      if (seen.size() > 2048) return false;
      for (auto* u : v->users()) {
        if (llvm::isa< llvm::BitCastOperator >(u)) { todo.push_back(u); continue; }
        auto* c = llvm::dyn_cast< llvm::CallBase >(u);
        if (!c || !binding(c) || !root(c->getFunction())) return false;
        auto* f = callee(c);
        if (!f || !f->isDeclaration()) return false;
        if (is_mutex && locks.count(c) && c->getArgOperand(0) == v) continue;
        if (f->getName() == "pthread_cond_wait" && c->arg_size() == 2 &&
            c->getArgOperand(is_mutex ? 1 : 0) == v) continue;
        if (!is_mutex && f->getName() == "pthread_cond_broadcast" && c->arg_size() == 1 && c->getArgOperand(0) == v) continue;
        return false;
      }
    }
    return true;
  }
  bool inventory() {
    // Actual source/AR bindings, roots and calls are closed before the CFG proof.
    for (auto it = ctx.bundle->function_begin(); it != ctx.bundle->function_end(); ++it) {
      auto* a = *it;
      if (!a->is_definition()) continue;
      if (!a->has_frontend()) return false;
      functions.emplace(a->frontend< llvm::Function >(), a);
      for (auto* b : *a->body()) for (auto* s : *b)
        if (s->has_frontend() && (isa< ar::CallBase >(s) || isa< ar::Load >(s) || isa< ar::Store >(s)))
          if (!originals.emplace(s->frontend< llvm::Value >(), s).second) return false;
    }
    for (auto it = ctx.bundle->global_begin(); it != ctx.bundle->global_end(); ++it)
      if ((*it)->has_frontend())
        if (auto* g = llvm::dyn_cast< llvm::GlobalVariable >((*it)->frontend< llvm::Value >())) globals.emplace(g, *it);
    auto* module = main->getParent();
    for (auto& f : *module) for (auto& b : f) for (auto& i : b) {
      if (!step()) return false;
      auto* c = llvm::dyn_cast< llvm::CallBase >(&i);
      auto* target = c ? callee(c) : nullptr;
      if (!target) continue;
      if (target->getName() == "pthread_create") {
        auto* a = binding(c);
        if (!a || c->getFunction() != main || !native_handle_api(c, true)) return false;
        auto* worker = llvm::dyn_cast< llvm::Function >(c->getArgOperand(2)->stripPointerCasts());
        auto* aw = dyn_cast< ar::FunctionPointerConstant >(a->argument(2));
        if (!worker || !functions.count(worker) || !aw || !aw->function()->has_frontend() ||
            aw->function()->frontend< llvm::Value >() != worker ||
            !isa< ar::NullConstant >(a->argument(1)) || !isa< ar::NullConstant >(a->argument(3))) return false;
        workers.insert(worker);
      }
      auto m = c ? normal_mutex(ctx, binding(c)) : NormalMutex{};
      if (m.source) {
        if (mutex && mutex != m.source) return false;
        mutex = m.source; locks.emplace(c, m);
      }
    }
    if (workers.empty() || !mutex || !closed_control(mutex, true)) return false;
    for (auto& g : module->globals()) if (g.isDeclaration() && !g.use_empty()) return false;
    for (auto& f : *module) {
      if (!f.isDeclaration()) {
        if (!functions.count(&f)) return false;
        std::vector< llvm::Value* > todo{&f}; std::set< llvm::Value* > seen;
        while (!todo.empty()) {
          auto* v = todo.back(); todo.pop_back();
          if (!step()) return false;
          if (!seen.insert(v).second) continue;
          for (auto* u : v->users()) {
            if (llvm::isa< llvm::BitCastOperator >(u)) { todo.push_back(u); continue; }
            auto* c = llvm::dyn_cast< llvm::CallBase >(u);
            if (!c || !binding(c)) return false;
            if (c->getCalledOperand() == v && !workers.count(&f)) continue;
            if (!native_handle_api(c, true) || c->getArgOperand(2) != v || c->getFunction() != main) return false;
          }
        }
      }
      for (auto& b : f) for (auto& i : b) {
        if (!step() || llvm::isa< llvm::InvokeInst >(i) || llvm::isa< llvm::CallBrInst >(i) ||
            llvm::isa< llvm::IndirectBrInst >(i) || llvm::isa< llvm::ResumeInst >(i)) return false;
        auto* c = llvm::dyn_cast< llvm::CallBase >(&i);
        if (!c || llvm::isa< llvm::DbgInfoIntrinsic >(c)) continue;
        auto* a = binding(c); auto* source = callee(c); auto* target = a ? native_handle_callee(a) : nullptr;
        if (!a || !source || !target) return false;
        if (!source->isDeclaration()) continue;
        auto name = source->getName(); const auto id = target->intrinsic_id();
        if (locks.count(c)) { if (!root(c->getFunction())) return false; continue; }
        if (name == "pthread_cond_wait" || name == "pthread_cond_broadcast") {
          if (!condition(c)) return false;
          continue;
        }
        if (native_handle_api(c, true) || native_handle_api(c, false)) continue;
        if (name == "malloc" && id == ar::Intrinsic::LibcMalloc && c->arg_size() == 1 &&
            c->getArgOperand(0)->getType()->isIntegerTy(32) && c->getType()->isPointerTy()) continue;
        if (name == "free" && id == ar::Intrinsic::LibcFree && c->arg_size() == 1 && c->getType()->isVoidTy()) continue;
        if (terminal(c)) continue;
        if (c->arg_empty() && ((name == "__VERIFIER_nondet_int" && c->getType()->isIntegerTy(32)) ||
            (id == ar::Intrinsic::IkosNonDet && c->getMetadata("ikos.freeze_uninit") &&
             ((name == "__ikos_nondet_int" && c->getType()->isIntegerTy(32)) ||
              (name == "__ikos_nondet_int64" && c->getType()->isIntegerTy(64)))))) continue;
        return false;
      }
    }
    return true;
  }
  Observation observation(llvm::Value* v, llvm::GlobalVariable* flag, unsigned depth = 0) const {
    if (depth == 16) return {};
    if (auto* l = llvm::dyn_cast< llvm::LoadInst >(v))
      return l->getPointerOperand() == flag ? Observation{l, true} : Observation{};
    if (auto* cast = llvm::dyn_cast< llvm::CastInst >(v)) {
      if ((cast->getOpcode() == llvm::Instruction::Trunc && cast->getType()->isIntegerTy(1)) ||
          cast->getOpcode() == llvm::Instruction::ZExt)
        return observation(cast->getOperand(0), flag, depth + 1);
      return {};
    }
    if (auto* x = llvm::dyn_cast< llvm::BinaryOperator >(v)) {
      auto* c = llvm::dyn_cast< llvm::ConstantInt >(x->getOperand(1));
      if (x->getOpcode() != llvm::Instruction::Xor || !x->getType()->isIntegerTy(1) || !c || !c->isOne()) return {};
      auto o = observation(x->getOperand(0), flag, depth + 1); o.positive = !o.positive; return o;
    }
    if (auto* cmp = llvm::dyn_cast< llvm::ICmpInst >(v)) {
      auto* c = llvm::dyn_cast< llvm::ConstantInt >(cmp->getOperand(1));
      if (!c || !(c->isZero() || c->isOne()) ||
          (cmp->getPredicate() != llvm::CmpInst::ICMP_EQ && cmp->getPredicate() != llvm::CmpInst::ICMP_NE)) return {};
      auto o = observation(cmp->getOperand(0), flag, depth + 1);
      if ((cmp->getPredicate() == llvm::CmpInst::ICMP_EQ) == c->isZero()) o.positive = !o.positive;
      return o;
    }
    return {};
  }
  bool flow(llvm::Function* f, llvm::GlobalVariable* flag, llvm::StoreInst* publication,
            std::map< llvm::Instruction*, bool >& observed) {
    std::map< llvm::BasicBlock*, State > inputs;
    std::deque< llvm::BasicBlock* > work{&f->getEntryBlock()}; inputs.emplace(&f->getEntryBlock(), State{});
    while (!work.empty()) {
      auto* b = work.front(); work.pop_front(); auto state = inputs.at(b); bool stopped = false;
      for (auto& i : *b) {
        if (!step() || llvm::isa< llvm::UnreachableInst >(i)) return false;
        if (auto* l = llvm::dyn_cast< llvm::LoadInst >(&i)) {
          if (l->getPointerOperand() == flag) {
            if (state.held != 2) return false;
            state.guarded.insert(l);
          }
          auto r = observed.emplace(l, state.seen); if (!r.second) r.first->second &= state.seen;
        }
        if (&i == publication && state.held != 2) return false;
        if (auto* c = llvm::dyn_cast< llvm::CallBase >(&i)) {
          if (llvm::isa< llvm::DbgInfoIntrinsic >(c)) continue;
          auto m = locks.find(c);
          if (m != locks.end()) {
            if (state.held != (m->second.lock ? 1u : 2u)) return false;
            state.held = m->second.lock ? 2 : 1; state.guarded.clear();
          } else if (waits.count(c)) {
            if (state.held != 2) return false;
            state.guarded.clear();
          } else if (terminal(c)) {
            stopped = true; break;
          }
        }
      }
      if (stopped) continue;
      auto* branch = llvm::dyn_cast< llvm::BranchInst >(b->getTerminator());
      auto o = branch && branch->isConditional() ? observation(branch->getCondition(), flag) : Observation{};
      for (unsigned n = 0; n < b->getTerminator()->getNumSuccessors(); ++n) {
        auto next = state;
        if (o.load && state.guarded.count(o.load) && (n == 0) == o.positive) next.seen = true;
        auto* dest = b->getTerminator()->getSuccessor(n);
        auto result = inputs.emplace(dest, next);
        if (result.second) work.push_back(dest);
        else { auto old = result.first->second; result.first->second.join(next); if (!(old == result.first->second)) work.push_back(dest); }
      }
    }
    return true;
  }
  void prove(llvm::GlobalVariable* flag) {
    if (!zero(flag) || !flag->getValueType()->isIntegerTy()) return;
    llvm::StoreInst* publication = nullptr;
    for (auto* u : flag->users()) {
      auto* i = llvm::dyn_cast< llvm::Instruction >(u);
      if (!i || !scalar(i, flag) || !root(i->getFunction())) return;
      if (auto* s = llvm::dyn_cast< llvm::StoreInst >(i)) {
        auto* c = llvm::dyn_cast< llvm::ConstantInt >(s->getValueOperand());
        auto* ac = dyn_cast< ar::IntegerConstant >(cast< ar::Store >(original(s))->value());
        if (publication || s->getFunction() != main || !c || !c->isOne() || !ac || ac->value().to_z_number() != 1) return;
        publication = s;
      }
    }
    if (!publication || reaches(publication, publication)) return;
    std::map< llvm::Instruction*, bool > observed;
    if (!flow(main, flag, publication, observed)) return;
    for (auto* worker : workers) if (!flow(worker, flag, publication, observed)) return;
    for (const auto& global : globals) {
      auto* g = global.first;
      if (g == flag || !g->getValueType()->isIntegerTy() || !zero(g)) continue;
      Proof proof{flag, mutex, {}, {}}; bool valid = true;
      for (auto* u : g->users()) {
        auto* i = llvm::dyn_cast< llvm::Instruction >(u);
        if (!step() || !i || !scalar(i, g)) { valid = false; break; }
        if (llvm::isa< llvm::StoreInst >(i)) {
          if (i->getFunction() != main || reaches(publication, i)) { valid = false; break; }
          proof.writes.insert(original(i));
        } else {
          if (!workers.count(i->getFunction()) || !observed.count(i) || !observed.at(i)) { valid = false; break; }
          proof.reads.insert(original(i));
        }
      }
      if (valid && !proof.writes.empty() && !proof.reads.empty()) proofs.push_back(std::move(proof));
    }
  }
  const Proof* pair(const AccessRecord& a, const AccessRecord& b) const {
    for (const auto& p : proofs)
      if ((a.kind == AccessKind::Write && b.kind == AccessKind::Read && p.writes.count(a.stmt) && p.reads.count(b.stmt)) ||
          (b.kind == AccessKind::Write && a.kind == AccessKind::Read && p.writes.count(b.stmt) && p.reads.count(a.stmt))) return &p;
    return nullptr;
  }
public:
  explicit Impl(Context& context) : ctx(context) {
    if (!ctx.opts.enable_thread_modular || ctx.opts.procedural == Procedural::Intraprocedural ||
        ctx.opts.entry_points.size() != 1 || ctx.opts.entry_points.front()->name() != "main" ||
        ctx.opts.entry_points.front()->num_parameters() != 0 || !ctx.opts.entry_points.front()->has_frontend() ||
        ctx.bundle->data_layout().pointers.bit_width != 32 || !ctx.opts.no_init_globals.empty() ||
        ctx.concurrent_env.is_dirty() || !ctx.concurrent_env.unmodeled_effects().empty() ||
        !ctx.concurrent_env.unmodeled_thread_creations().empty() ||
        ctx.bundle->global_or_null("ar.global_ctors") || ctx.bundle->global_or_null("ar.global_dtors")) return;
    main = ctx.opts.entry_points.front()->frontend< llvm::Function >();
    auto* module = main->getParent(); llvm::Triple target(module->getTargetTriple());
    if (!main->use_empty() || !module->getModuleInlineAsm().empty() || target.getArch() != llvm::Triple::x86 ||
        !target.isOSLinux() || target.getEnvironment() != llvm::Triple::GNU) return;
    bool potential = false;
    for (auto& g : module->globals()) for (auto* u : g.users())
      if (auto* s = llvm::dyn_cast< llvm::StoreInst >(u))
        if (s->getFunction() == main)
          if (auto* c = llvm::dyn_cast< llvm::ConstantInt >(s->getValueOperand())) potential |= c->isOne();
    if (!potential || !mutex_effect_pointer_representations_closed(*module) || !inventory()) return;
    for (const auto& g : globals) { if (!budget) break; prove(g.first); }
  }
  bool ordered(const AccessRecord& a, const AccessRecord& b) const { return pair(a, b); }
  JsonDict info(const AccessRecord& a, const AccessRecord& b) const {
    auto* p = pair(a, b);
    return p ? JsonDict{{"safe_reason", "main writes precede a monotone flag observed under its actual mutex"},
                         {"guarded_publication_contract", "closed-main-monotone-mutex-flag-v1"},
                         {"publication_flag", p->flag->getName().str()},
                         {"publication_mutex", p->mutex->getName().str()}} : JsonDict{};
  }
};
GuardedPublication::GuardedPublication(Context& ctx) : _impl(std::make_unique< Impl >(ctx)) {}
GuardedPublication::~GuardedPublication() = default;
bool GuardedPublication::ordered(const AccessRecord& a, const AccessRecord& b) const { return _impl->ordered(a, b); }
JsonDict GuardedPublication::info(const AccessRecord& a, const AccessRecord& b) const { return _impl->info(a, b); }
} // namespace analyzer
} // namespace ikos
