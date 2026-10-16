// Copyright and license: see data_race.hpp.
#include <ikos/analyzer/checker/binary_token.hpp>

#include <ikos/analyzer/checker/data_race.hpp>
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
struct Token {
  llvm::GlobalVariable* source = nullptr;
  std::map< llvm::CallBase*, bool > operations;
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
  std::map< llvm::Function*, int > kinds;
  std::map< llvm::CallBase*, NormalMutex > mutex_calls;
  std::vector< Token > tokens;
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
    return true;
  }
  bool token_object(llvm::GlobalVariable* g) const {
    if (!g || !globals.count(g) || g->isConstant() || g->isThreadLocal() || g->isExternallyInitialized() ||
        g->getAddressSpace() != 0 || !g->hasInitializer() || !g->getValueType()->isIntegerTy(32)) return false;
    auto* n = llvm::dyn_cast< llvm::ConstantInt >(g->getInitializer());
    auto* a = globals.at(g); auto* type = dyn_cast< ar::IntegerType >(a->type()->pointee());
    if (!n || !n->isZero() || !a->is_definition() || a->is_thread_local() || !type || type->bit_width() != 32 ||
        ctx.bundle->data_layout().alloc_size_in_bytes(type) != 4) return false;
    unsigned count = 0;
    for (auto* b : *a->initializer()) for (auto* s : *b) {
      auto* store = dyn_cast< ar::Store >(s);
      auto* v = store ? dyn_cast< ar::IntegerConstant >(store->value()) : nullptr;
      if (!store || store->pointer() != a || !v || v->type() != type || v->value().to_z_number() != 0) return false;
      ++count;
    }
    return count == 1;
  }
  bool parameter_effect(llvm::Instruction* i, llvm::Function* f) const {
    auto* pointer = f->getArg(0); auto* ar_pointer = functions.at(f)->param(0);
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
  bool atomic_signature(llvm::Function* f) const {
    auto found = functions.find(f);
    if (found == functions.end() || f->isVarArg() || f->arg_size() != 1 ||
        !f->getReturnType()->isVoidTy() || f->getCallingConv() != llvm::CallingConv::C) return false;
    auto* p = llvm::dyn_cast< llvm::PointerType >(f->getArg(0)->getType());
    auto* a = found->second;
    auto* ar_p = a->num_parameters() == 1 ? dyn_cast< ar::PointerType >(a->param(0)->type()) : nullptr;
    auto* ar_i = ar_p ? dyn_cast< ar::IntegerType >(ar_p->pointee()) : nullptr;
    return p && p->getAddressSpace() == 0 && p->getPointerElementType()->isIntegerTy(32) &&
        ar_i && ar_i->bit_width() == 32 && a->type()->return_type()->is_void() && a->name() == f->getName().str() &&
        classify_atomic_intrinsic(a->name()) == AtomicKind::SingleCall;
  }
  Transition transition(llvm::Function* operation, unsigned entry) {
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
            if (!root || !parameter_effect(&i, f)) return {};
            values[&i] = llvm::APInt(32, state);
          } else if (auto* s = llvm::dyn_cast< llvm::StoreInst >(&i)) {
            auto* n = llvm::dyn_cast< llvm::ConstantInt >(s->getValueOperand());
            auto* a = dyn_cast_or_null< ar::Store >(original(s));
            auto* ar_n = a ? dyn_cast< ar::IntegerConstant >(a->value()) : nullptr;
            if (!root || !parameter_effect(&i, f) || !n || (!n->isZero() && !n->isOne()) ||
                !ar_n || ar_n->value().to_z_number() != core::ZNumber(n->getZExtValue())) return {};
            state = n->getZExtValue(); ++writes;
          } else if (auto* cmp = llvm::dyn_cast< llvm::ICmpInst >(&i)) {
            llvm::APInt x, y;
            if (!value(cmp->getOperand(0), x) || !value(cmp->getOperand(1), y) || x.getBitWidth() != y.getBitWidth()) return {};
            values[&i] = llvm::APInt(1, llvm::ICmpInst::compare(x, y, cmp->getPredicate()));
          } else if (auto* z = llvm::dyn_cast< llvm::ZExtInst >(&i)) {
            llvm::APInt n;
            if (!z->getType()->isIntegerTy(32) || !z->getSrcTy()->isIntegerTy(1) || !value(z->getOperand(0), n)) return {};
            values[&i] = n.zext(32);
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
    return run(operation, nullptr, true);
  }
  int kind(llvm::Function* f) {
    auto known = kinds.find(f); if (known != kinds.end()) return known->second;
    kinds.emplace(f, 0);
    if (!atomic_signature(f)) return 0;
    // Every pointer use is a complete direct access; no hidden alias/callback.
    for (auto* u : f->getArg(0)->users()) {
      if (!spend()) return 0;
      auto* i = llvm::dyn_cast< llvm::Instruction >(u);
      if (!i || (!llvm::isa< llvm::DbgInfoIntrinsic >(i) && !parameter_effect(i, f))) return 0;
    }
    auto zero = transition(f, 0); auto one = transition(f, 1);
    if (!zero.supported || !one.supported) return 0;
    int result = 0;
    if (zero.returned && zero.state == 1 && zero.writes == 1 && !one.returned && one.state == 1 && one.writes == 0) result = 1;
    if (!zero.returned && zero.state == 0 && zero.writes == 0 && one.returned && one.state == 0 && one.writes == 1) result = -1;
    kinds.at(f) = result; return result;
  }
  bool close_token(Token& token) {
    std::vector< llvm::Value* > todo{token.source}; std::set< llvm::Value* > seen;
    bool acquire = false, release = false;
    while (!todo.empty()) {
      auto* v = todo.back(); todo.pop_back();
      if (!spend()) return false;
      if (!seen.insert(v).second) continue;
      for (auto* u : v->users()) {
        if (llvm::isa< llvm::BitCastOperator >(u)) { todo.push_back(u); continue; }
        auto* c = llvm::dyn_cast< llvm::CallBase >(u);
        auto* a = c ? binding(c) : nullptr;
        if (!a || c->arg_size() != 1 || c->getArgOperand(0) != v || !c->use_empty() ||
            !isa< ar::FunctionPointerConstant >(a->called()) || !exact_handle_slot(a->argument(0), globals.at(token.source))) return false;
        int k = kind(callee(c)); if (!k) return false;
        token.operations.emplace(c, k == 1); acquire |= k == 1; release |= k == -1;
      }
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
        (l && (r.kind != AccessKind::Read || l->isVolatile() || l->isAtomic())) ||
        (s && (r.kind != AccessKind::Write || s->isVolatile() || s->isAtomic()))) return nullptr;
    auto* a_l = dyn_cast< ar::Load >(r.stmt); auto* a_s = dyn_cast< ar::Store >(r.stmt);
    if ((l && (!a_l || a_l->operand() != p->global_var() || a_l->result()->type() != p->global_var()->type()->pointee())) ||
        (s && (!a_s || a_s->pointer() != p->global_var() || a_s->value()->type() != p->global_var()->type()->pointee()))) return nullptr;
    return i->getModule()->getDataLayout().getTypeStoreSize(type).getFixedValue() == r.width ? i : nullptr;
  }
  const Token* pair(const AccessRecord& a, const AccessRecord& b) const {
    auto* x = access(a); auto* y = access(b);
    if (!x || !y) return nullptr;
    for (const auto& t : tokens) {
      auto p = t.held.find(x); auto q = t.held.find(y);
      if (p != t.held.end() && q != t.held.end() && p->second && q->second) return &t;
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
  }
  BinaryTokenEvent event(llvm::CallBase* call) const {
    for (const auto& token : tokens) {
      auto found = token.operations.find(call);
      if (found != token.operations.end()) return {token.source, found->second};
    }
    return {};
  }
  bool excludes(const AccessRecord& a, const AccessRecord& b) const { return pair(a, b); }
  JsonDict info(const AccessRecord& a, const AccessRecord& b) const {
    auto* t = pair(a, b);
    return t ? JsonDict{{"safe_reason", "both accesses must own the same closed atomic token"},
                        {"binary_token_contract", "closed-binary-token-ownership-v1"},
                        {"token", t->source->getName().str()}} : JsonDict{};
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
