// Copyright and license: see data_race.hpp.
#include <llvm/IR/Dominators.h>
#include <algorithm>
#include <deque>
#include <ikos/analyzer/analysis/execution_engine/pthread_initialization.hpp>
#include <ikos/analyzer/checker/data_race.hpp>
#include <ikos/analyzer/analysis/source_call_binding.hpp>
#include <ikos/analyzer/analysis/source_program_facts.hpp>
#include <ikos/analyzer/checker/normal_mutex.hpp>
#include <ikos/analyzer/checker/thread_lifecycle.hpp>
#include <ikos/analyzer/util/log.hpp>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace ikos {
namespace analyzer {
namespace {
struct Refuse {};
struct Value {
  bool known = false;
  std::uint64_t bits = 0;
  bool operator<(const Value& b) const {
    return std::tie(known, bits) < std::tie(b.known, b.bits);
  }
  bool operator==(const Value& b) const {
    return known == b.known && bits == b.bits;
  }
};
struct State {
  std::map< llvm::Value*, Value > values;
  std::vector< bool > outstanding;
  // Only a runtime-storage lifetime check; never a successful-lock fact.
  std::vector< bool > initialized;
  bool operator<(const State& b) const {
    return std::tie(values, outstanding, initialized) <
           std::tie(b.values, b.outstanding, b.initialized);
  }
};
struct Job {
  llvm::Instruction* pc;
  State state;
  bool operator<(const Job& b) const {
    return std::tie(pc, state) < std::tie(b.pc, b.state);
  }
};
struct Return {
  State state;
  Value value;
};
llvm::Function* callee(llvm::CallBase* c) {
  return llvm::dyn_cast< llvm::Function >(
      c->getCalledOperand()->stripPointerCasts());
}
bool scalar(llvm::Type* t) {
  return t->isIntegerTy() && t->getIntegerBitWidth() <= 64;
}
} // namespace

#include "scalar_thread_completion.inc"

class ThreadLifecycle::Impl {
  ScalarThreadCompletion scalar_completion;
  bool scalar_last = false;
  Context& ctx;
  SourceProgramFacts& source_facts;
  llvm::Function* main = nullptr;
  const std::map< llvm::Value*, ar::Statement* >& originals;
  std::set< llvm::Function* > roots, active;
  std::map< llvm::Function*, std::set< llvm::Function* > > owners;
  struct Child {
    llvm::GlobalVariable* slot;
    llvm::Function* parent;
    llvm::Function* entry;
    llvm::CallBase* create;
  };
  std::vector< Child > children;
  std::vector< llvm::GlobalVariable* > mutexes;
  std::map< llvm::GlobalVariable*, llvm::Function* > private_cells;
  std::map< llvm::CallBase*, unsigned > creates, joins;
  std::map< llvm::CallBase*, std::pair< unsigned, unsigned > > mutex_calls;
  std::map< llvm::Function*, std::set< llvm::Function* > > descendants;
  std::map< std::pair< llvm::Instruction*, llvm::Function* >,
            std::set< llvm::Function* > >
      absent;
  bool attempted = false, valid = false;
  unsigned budget = 131072;
  unsigned word = 0;
  llvm::Instruction* last = nullptr;
  void spend() {
    if (!budget)
      throw Refuse{};
    --budget;
  }
  ar::Statement* original(llvm::Value* v) const {
    auto i = originals.find(v);
    return i == originals.end() ? nullptr : i->second;
  }
  ar::CallBase* binding(llvm::CallBase* c) const {
    return source_call_binding(c, original(c));
  }

  bool api(llvm::CallBase* c, bool create) const {
    auto* a = binding(c);
    auto* f = callee(c);
    const unsigned count = create ? 4 : 2;
    if (!a || !f->isDeclaration() ||
        f->getName() != (create ? "pthread_create" : "pthread_join") ||
        f->isVarArg() || c->getFunctionType() != f->getFunctionType() ||
        c->arg_size() != count || !c->use_empty() ||
        !c->getType()->isIntegerTy(32))
      return false;
    for (unsigned n = 0; n < count; ++n) {
      auto* t = c->getArgOperand(n)->getType();
      if (n == 0 && !create) {
        if (!t->isIntegerTy(word * 8))
          return false;
      } else {
        auto* p = llvm::dyn_cast< llvm::PointerType >(t);
        if (!p || p->getAddressSpace())
          return false;
      }
    }
    if (create)
      return native_handle_api(c, true) != nullptr;
    return llvm::cast< llvm::PointerType >(c->getArgOperand(1)->getType())
        ->getPointerElementType()
        ->isPointerTy();
  }
  bool terminal(llvm::CallBase* c) const {
    auto* a = binding(c);
    auto* f = callee(c);
    auto* t = a ? native_handle_callee(a) : nullptr;
    return t && f->isDeclaration() &&
           ((f->getName() == "abort" && c->arg_empty() &&
             c->getType()->isVoidTy() &&
             t->intrinsic_id() == ar::Intrinsic::LibcAbort) ||
            constant_assertion_effect(a));
  }
  bool nondet(llvm::CallBase* c) const {
    auto* a = binding(c);
    auto* f = callee(c);
    auto* t = a ? native_handle_callee(a) : nullptr;
    if (!t || !f->isDeclaration() || !c->arg_empty() || !scalar(c->getType()))
      return false;
    if (f->getName() == "__VERIFIER_nondet_int" &&
        c->getType()->isIntegerTy(32) && t->has_frontend() &&
        t->frontend< llvm::Value >() == f && t->type()->return_type()->is_integer() &&
        cast< ar::IntegerType >(t->type()->return_type())->bit_width() == 32)
      return true;
    return c->getMetadata("ikos.freeze_uninit") &&
           t->intrinsic_id() == ar::Intrinsic::IkosNonDet &&
           ((f->getName() == "__ikos_nondet_int" &&
             c->getType()->isIntegerTy(32)) ||
            (f->getName() == "__ikos_nondet_int64" &&
             c->getType()->isIntegerTy(64)));
  }
  ar::GlobalVariable* global(llvm::GlobalVariable* g) const {
    const auto& globals = source_facts.globals();
    auto it = globals.find(g);
    return it == globals.end() ? nullptr : it->second;
  }
  bool whole_global(llvm::GlobalVariable* g) const {
    if (!g || !g->hasInitializer() || g->isConstant() || g->isThreadLocal() ||
        g->isExternallyInitialized() || g->getAddressSpace() ||
        !scalar(g->getValueType()) ||
        !llvm::isa< llvm::ConstantInt >(g->getInitializer()))
      return false;
    auto* ag = global(g);
    if (!ag || !ag->is_definition() || ag->is_thread_local())
      return false;
    auto* t = dyn_cast< ar::IntegerType >(ag->type()->pointee());
    if (!t || t->bit_width() != g->getValueType()->getIntegerBitWidth())
      return false;
    unsigned count = 0;
    for (auto* b : *ag->initializer())
      for (auto* s : *b) {
        auto* store = dyn_cast< ar::Store >(s);
        auto* v =
            store ? dyn_cast< ar::IntegerConstant >(store->value()) : nullptr;
        if (!store || store->pointer() != ag || !v ||
            v->value().bit_width() != t->bit_width())
          return false;
        llvm::SmallString< 32 > text;
        llvm::cast< llvm::ConstantInt >(g->getInitializer())
            ->getValue()
            .toString(text, 10, t->is_signed());
        if (v->value().to_z_number() !=
            core::ZNumber::from_string(text.str().str()))
          return false;
        ++count;
      }
    return count == 1;
  }
  bool exact_pointer(ar::Value* a, llvm::Value* native) const {
    if (auto* g = llvm::dyn_cast< llvm::GlobalVariable >(native)) {
      auto* ag = global(g);
      return ag && exact_handle_slot(a, ag);
    }
    auto* l = llvm::dyn_cast< llvm::AllocaInst >(native);
    auto* alloc = l ? dyn_cast_or_null< ar::Allocate >(original(l)) : nullptr;
    if (!alloc)
      return false;
    for (unsigned depth = 0; depth < 32; ++depth) {
      if (a == alloc->result())
        return true;
      auto* v = dyn_cast< ar::InternalVariable >(a);
      auto* d = v ? unique_def(v) : nullptr;
      if (auto* copy = dyn_cast_or_null< ar::Assignment >(d))
        a = copy->operand();
      else if (auto* cast = dyn_cast_or_null< ar::UnaryOperation >(d)) {
        if (cast->op() != ar::UnaryOperation::Bitcast)
          return false;
        a = cast->operand();
      } else
        return false;
    }
    return false;
  }
  bool private_result(llvm::CallBase* c) {
    auto* out = c->getArgOperand(1);
    if (llvm::isa< llvm::ConstantPointerNull >(out))
      return true;
    auto* local = llvm::dyn_cast< llvm::AllocaInst >(out->stripPointerCasts());
    auto* n = local ? llvm::dyn_cast< llvm::ConstantInt >(local->getArraySize())
                    : nullptr;
    auto* a =
        local ? dyn_cast_or_null< ar::Allocate >(original(local)) : nullptr;
    auto* count =
        a ? dyn_cast< ar::IntegerConstant >(a->array_size()) : nullptr;
    if (!local || !n || !n->isOne() ||
        !local->getAllocatedType()->isPointerTy() || local->getAddressSpace() ||
        llvm::cast< llvm::PointerType >(local->getAllocatedType())->getAddressSpace() ||
        local->getModule()->getDataLayout().getTypeAllocSize(local->getAllocatedType()) != word ||
        local->getAlign().value() < word || !a || !count ||
        count->value().to_z_number() != 1 ||
        !a->allocated_type()->is_pointer() ||
        ctx.bundle->data_layout().alloc_size_in_bytes(a->allocated_type()) !=
            word ||
        !exact_pointer(binding(c)->argument(1), local))
      return false;
    std::vector< llvm::Value* > work{local};
    std::set< llvm::Value* > seen;
    while (!work.empty()) {
      spend();
      auto* v = work.back();
      work.pop_back();
      if (!seen.insert(v).second)
        continue;
      for (auto* u : v->users()) {
        if (llvm::isa< llvm::BitCastOperator >(u)) {
          work.push_back(u);
          continue;
        }
        if (u != c || c->getArgOperand(1) != v)
          return false;
      }
    }
    llvm::DominatorTree dt(*c->getFunction());
    return dt.dominates(local, c);
  }
  Value value(llvm::Value* v, const State& s, unsigned depth = 0) const {
    if (depth == 32 || !scalar(v->getType()))
      return {};
    if (auto* n = llvm::dyn_cast< llvm::ConstantInt >(v))
      return {true, n->getZExtValue()};
    auto it = s.values.find(v);
    if (it != s.values.end())
      return it->second;
    auto* i = llvm::dyn_cast< llvm::Instruction >(v);
    if (!i)
      return {};
    auto operand = [&](unsigned n) {
      return value(i->getOperand(n), s, depth + 1);
    };
    if (auto* cmp = llvm::dyn_cast< llvm::ICmpInst >(i)) {
      auto a = operand(0), b = operand(1);
      auto* t = cmp->getOperand(0)->getType();
      if (!a.known || !b.known || !scalar(t))
        return {};
      return {true,
              llvm::ICmpInst::compare(llvm::APInt(t->getIntegerBitWidth(),
                                                  a.bits),
                                      llvm::APInt(t->getIntegerBitWidth(),
                                                  b.bits),
                                      cmp->getPredicate())};
    }
    if (auto* cast = llvm::dyn_cast< llvm::CastInst >(i)) {
      auto a = operand(0);
      auto* from = cast->getOperand(0)->getType();
      auto* to = cast->getType();
      if (!a.known || !scalar(from) || !scalar(to))
        return {};
      llvm::APInt n(from->getIntegerBitWidth(), a.bits);
      if (cast->getOpcode() == llvm::Instruction::SExt)
        n = n.sext(to->getIntegerBitWidth());
      else if (cast->getOpcode() == llvm::Instruction::ZExt)
        n = n.zext(to->getIntegerBitWidth());
      else if (cast->getOpcode() == llvm::Instruction::Trunc)
        n = n.trunc(to->getIntegerBitWidth());
      else if (cast->getOpcode() != llvm::Instruction::BitCast || from != to)
        return {};
      return {true, n.getZExtValue()};
    }
    if (llvm::isa< llvm::SelectInst >(i)) {
      auto c = operand(0), a = operand(1), b = operand(2);
      return c.known ? (c.bits ? a : b) : (a == b ? a : Value{});
    }
    if (auto* op = llvm::dyn_cast< llvm::BinaryOperator >(i)) {
      auto a = operand(0), b = operand(1);
      if (!a.known || !b.known || !op->getType()->isIntegerTy(1))
        return {};
      if (op->getOpcode() == llvm::Instruction::And)
        return {true, a.bits & b.bits};
      if (op->getOpcode() == llvm::Instruction::Or)
        return {true, a.bits | b.bits};
      if (op->getOpcode() == llvm::Instruction::Xor)
        return {true, a.bits ^ b.bits};
    }
    return {};
  }
  void snapshot(llvm::Instruction* i, llvm::Function* root, const State& s) {
    if (!llvm::isa< llvm::LoadInst >(i) && !llvm::isa< llvm::StoreInst >(i))
      return;
    std::set< llvm::Function* > names;
    for (unsigned n = 0; n < children.size(); ++n)
      if (children[n].parent == root && !s.outstanding[n]) {
        names.insert(children[n].entry);
        const auto& sub = descendants.at(children[n].entry);
        names.insert(sub.begin(), sub.end());
      }
    auto key = std::make_pair(i, root);
    auto old = absent.find(key);
    if (old == absent.end())
      absent.emplace(key, std::move(names));
    else {
      std::set< llvm::Function* > both;
      std::set_intersection(names.begin(),
                            names.end(),
                            old->second.begin(),
                            old->second.end(),
                            std::inserter(both, both.end()));
      old->second = std::move(both);
    }
  }
  bool quiet(llvm::Function* root, const State& s) const {
    for (unsigned n = 0; n < children.size(); ++n)
      if (children[n].parent == root && s.outstanding[n])
        return false;
    return true;
  }
  std::vector< Return > execute(llvm::Function* f,
                                llvm::Function* root,
                                State initial,
                                const std::vector< Value >& args) {
    spend();
    if (!active.insert(f).second || active.size() > 16)
      throw Refuse{};
    for (auto& a : f->args())
      initial.values.erase(&a);
    for (auto& b : *f)
      for (auto& i : b)
        initial.values.erase(&i);
    if (f == root) {
      if (root != main && (!f->getArg(0)->use_empty()))
        throw Refuse{};
    } else {
      if (f->arg_size() != args.size())
        throw Refuse{};
      unsigned n = 0;
      for (auto& a : f->args())
        initial.values[&a] = args[n++];
    }
    std::deque< Job > pending{
        {&f->getEntryBlock().front(), std::move(initial)}};
    std::set< Job > seen;
    std::vector< Return > returns;
    auto enter = [&](State s, llvm::BasicBlock* from, llvm::BasicBlock* to) {
      std::vector< std::pair< llvm::Value*, Value > > updates;
      for (auto& i : *to) {
        auto* phi = llvm::dyn_cast< llvm::PHINode >(&i);
        if (!phi)
          break;
        updates.emplace_back(phi,
                             value(phi->getIncomingValueForBlock(from), s));
      }
      for (const auto& u : updates)
        s.values[u.first] = u.second;
      auto* first = to->getFirstNonPHI();
      if (!first)
        throw Refuse{};
      pending.push_back({first, std::move(s)});
    };
    while (!pending.empty()) {
      spend();
      auto j = std::move(pending.front());
      pending.pop_front();
      if (!seen.insert(j).second)
        continue;
      if (seen.size() > 16384)
        throw Refuse{};
      auto* i = j.pc;
      last = i;
      auto& s = j.state;
      snapshot(i, root, s);
      if (auto* branch = llvm::dyn_cast< llvm::BranchInst >(i)) {
        auto cond = branch->isConditional() ? value(branch->getCondition(), s)
                                            : Value{};
        for (unsigned n = 0; n < branch->getNumSuccessors(); ++n) {
          if (branch->isConditional() && cond.known &&
              n != (cond.bits ? 0u : 1u))
            continue;
          enter(s, i->getParent(), branch->getSuccessor(n));
        }
        continue;
      }
      if (auto* sw = llvm::dyn_cast< llvm::SwitchInst >(i)) {
        auto v = value(sw->getCondition(), s);
        auto* selected = sw->getDefaultDest();
        if (v.known)
          for (auto c : sw->cases())
            if (c.getCaseValue()->getZExtValue() == v.bits)
              selected = c.getCaseSuccessor();
        for (unsigned n = 0; n < sw->getNumSuccessors(); ++n)
          if (!v.known || sw->getSuccessor(n) == selected)
            enter(s, i->getParent(), sw->getSuccessor(n));
        continue;
      }
      if (auto* r = llvm::dyn_cast< llvm::ReturnInst >(i)) {
        if (f == root && root != main &&
            (!quiet(root, s) ||
             !llvm::isa< llvm::ConstantPointerNull >(r->getReturnValue())))
          throw Refuse{};
        auto v = r->getReturnValue() ? value(r->getReturnValue(), s) : Value{};
        returns.push_back({std::move(s), v});
        continue;
      }
      if (llvm::isa< llvm::UnreachableInst >(i))
        throw Refuse{};
      if (auto* load = llvm::dyn_cast< llvm::LoadInst >(i)) {
        auto* g = llvm::cast< llvm::GlobalVariable >(
            load->getPointerOperand()->stripPointerCasts());
        auto owned = private_cells.find(g);
        auto old = s.values.find(g);
        s.values[i] = owned != private_cells.end() && owned->second == root &&
                              old != s.values.end()
                          ? old->second
                          : Value{};
      } else if (auto* store = llvm::dyn_cast< llvm::StoreInst >(i)) {
        auto* g = llvm::cast< llvm::GlobalVariable >(
            store->getPointerOperand()->stripPointerCasts());
        auto owned = private_cells.find(g);
        if (owned != private_cells.end() && owned->second == root)
          s.values[g] = value(store->getValueOperand(), s);
      } else if (auto* call = llvm::dyn_cast< llvm::CallBase >(i)) {
        if (terminal(call))
          continue;
        auto* target = callee(call);
        if (!target->isDeclaration()) {
          std::vector< Value > argv;
          for (auto& arg : call->args())
            argv.push_back(value(arg.get(), s));
          auto outcomes = execute(target, root, s, argv);
          for (auto& out : outcomes) {
            out.state.values[call] = out.value;
            pending.push_back({i->getNextNode(), std::move(out.state)});
          }
          continue;
        }
        auto c = creates.find(call), j = joins.find(call);
        if (c != creates.end()) {
          const unsigned n = c->second;
          if (children[n].parent != root || !quiet(root, s) ||
              std::find(s.initialized.begin(), s.initialized.end(), false) !=
                  s.initialized.end())
            throw Refuse{};
          for (bool live : {false, true}) {
            State next = s;
            next.outstanding[n] = live;
            next.values[i] = {};
            pending.push_back({i->getNextNode(), std::move(next)});
          }
          continue;
        } else if (j != joins.end()) {
          const unsigned n = j->second;
          if (children[n].parent != root)
            throw Refuse{};
          // Current: exclusive ordinary GNU join has no returning error here.
          // Empty: preserve the existing no-HB/unknown-native-handle frame.
          s.outstanding[n] = false;
          s.values[i] = {};
        } else if (mutex_calls.count(call)) {
          auto event = mutex_calls.at(call);
          if (event.second < 2) {
            if (root != main || !quiet(root, s))
              throw Refuse{};
            s.initialized[event.first] = event.second == 0;
          } else if (!s.initialized[event.first])
            throw Refuse{};
          s.values[i] = {};
        } else if (nondet(call))
          s.values[i] = {};
        else
          throw Refuse{};
      }
      auto* next = i->getNextNode();
      if (!next)
        throw Refuse{};
      pending.push_back({next, std::move(s)});
    }
    active.erase(f);
    return returns;
  }
  void mark_owners(llvm::Function* f,
                   llvm::Function* root,
                   std::set< llvm::Function* >& stack) {
    spend();
    if (!stack.insert(f).second || stack.size() > 16)
      throw Refuse{};
    owners[f].insert(root);
    for (auto& b : *f)
      for (auto& i : b)
        if (auto* c = llvm::dyn_cast< llvm::CallBase >(&i)) {
          if (!binding(c))
            throw Refuse{};
          auto* t = callee(c);
          if (!t->isDeclaration()) {
            if (roots.count(t))
              throw Refuse{};
            mark_owners(t, root, stack);
          }
        }
    stack.erase(f);
  }
  std::set< llvm::Function* > tree(llvm::Function* root,
                                   std::set< llvm::Function* >& stack) {
    if (!stack.insert(root).second)
      throw Refuse{};
    std::set< llvm::Function* > result;
    for (const auto& child : children)
      if (child.parent == root) {
        auto sub = tree(child.entry, stack);
        result.insert(child.entry);
        result.insert(sub.begin(), sub.end());
      }
    stack.erase(root);
    descendants[root] = result;
    return result;
  }
  bool same_memory(llvm::Instruction* i) const {
    auto* l = llvm::dyn_cast< llvm::LoadInst >(i);
    auto* s = llvm::dyn_cast< llvm::StoreInst >(i);
    if (!l && !s)
      return false;
    auto* p = (l ? l->getPointerOperand() : s->getPointerOperand())
                  ->stripPointerCasts();
    auto* g = llvm::dyn_cast< llvm::GlobalVariable >(p);
    auto* type = l ? l->getType() : s->getValueOperand()->getType();
    auto* al = dyn_cast_or_null< ar::Load >(original(i));
    auto* as = dyn_cast_or_null< ar::Store >(original(i));
    auto* at = al ? al->result()->type() : as ? as->value()->type() : nullptr;
    return whole_global(g) && type == g->getValueType() &&
           ((l && al && !l->isAtomic() && !l->isVolatile() &&
             !al->is_atomic()) ||
            (s && as && !s->isAtomic() && !s->isVolatile() &&
             !as->is_atomic())) &&
           at && at->is_integer() &&
           cast< ar::IntegerType >(at)->bit_width() ==
               type->getIntegerBitWidth() &&
           exact_pointer(al ? al->operand() : as->pointer(), g) &&
           ctx.bundle->data_layout().store_size_in_bytes(at) ==
               i->getModule()
                   ->getDataLayout()
                   .getTypeStoreSize(type)
                   .getFixedValue();
  }
  void inventory() {
    auto& module = *main->getParent();
    const auto abi = pthread_object_layout(ctx.bundle, &module);
    if (!abi || module.getDataLayout().getIndexSizeInBits(0) != abi.word * 8 ||
        !module.getModuleInlineAsm().empty() ||
        module.getNamedGlobal("llvm.global_ctors") ||
        module.getNamedGlobal("llvm.global_dtors") ||
        !mutex_effect_pointer_representations_closed(module))
      throw Refuse{};
    word = abi.word;
    roots.insert(main);
    // Recover the complete native creation inventory before assigning roles.
    for (auto& f : module)
      if (!f.isDeclaration())
        for (auto& b : f)
          for (auto& i : b) {
            auto* c = llvm::dyn_cast< llvm::CallBase >(&i);
            if (!c)
              continue;
            auto* t = callee(c);
            if (!t)
              throw Refuse{};
            if (t->getName() != "pthread_create")
              continue;
            if (!api(c, true))
              throw Refuse{};
            auto* slot = llvm::dyn_cast< llvm::GlobalVariable >(
                c->getArgOperand(0)->stripPointerCasts());
            auto* child = llvm::dyn_cast< llvm::Function >(
                c->getArgOperand(2)->stripPointerCasts());
            auto* a = binding(c);
            auto* entry =
                dyn_cast< ar::FunctionPointerConstant >(a->argument(2));
            if (!whole_global(slot) || !slot->getValueType()->isIntegerTy(word * 8) ||
                !child || child == main || !entry ||
                !entry->function()->has_frontend() ||
                entry->function()->frontend< llvm::Value >() != child ||
                !exact_pointer(a->argument(0), slot) ||
                !source_facts.definitions().count(child) || !roots.insert(child).second ||
                slot->getAlign().valueOrOne().value() < word)
              throw Refuse{};
            for (const auto& old : children)
              if (old.slot == slot)
                throw Refuse{};
            creates.emplace(c, children.size());
            children.push_back({slot, nullptr, child, c});
          }
    if (children.empty() || children.size() > 8)
      throw Refuse{};
    for (auto* root : roots) {
      std::set< llvm::Function* > stack;
      mark_owners(root, root, stack);
    }
    for (auto& child : children) {
      const auto& role = owners.at(child.create->getFunction());
      if (role.size() != 1)
        throw Refuse{};
      child.parent = *role.begin();
    }
    std::set< llvm::Function* > stack;
    auto all = tree(main, stack);
    if (all.size() + 1 != roots.size())
      throw Refuse{};
    // Exact scalar cells used by one serial parent activation need no
    // interference.
    for (auto& g : module.globals()) {
      if (!whole_global(&g))
        continue;
      std::set< llvm::Function* > uses;
      for (auto* u : g.users()) {
        auto* i = llvm::dyn_cast< llvm::Instruction >(u);
        if (!i || !owners.count(i->getFunction())) {
          uses.insert(nullptr);
          break;
        }
        const auto& roles = owners.at(i->getFunction());
        uses.insert(roles.begin(), roles.end());
      }
      if (uses.size() == 1 && *uses.begin())
        private_cells.emplace(&g, *uses.begin());
    }
    for (auto& f : module) {
      if (f.isDeclaration())
        continue;
      if (!source_facts.definitions().count(&f) || !owners.count(&f) || f.isVarArg() ||
          f.getCallingConv() != llvm::CallingConv::C)
        throw Refuse{};
      for (auto& a : f.args()) {
        if (!scalar(a.getType()) &&
            !(roots.count(&f) && &f != main && a.getType()->isPointerTy() &&
              a.use_empty()))
          throw Refuse{};
        for (auto attr : {llvm::Attribute::ByVal,
                          llvm::Attribute::StructRet,
                          llvm::Attribute::InAlloca,
                          llvm::Attribute::Preallocated})
          if (f.hasParamAttribute(a.getArgNo(), attr))
            throw Refuse{};
      }
      for (auto* u : f.users()) {
        auto* c = llvm::dyn_cast< llvm::CallBase >(u);
        if (!c || !binding(c) ||
            (!roots.count(&f)
                 ? callee(c) != &f
                 : !creates.count(c) || children[creates.at(c)].entry != &f))
          throw Refuse{};
      }
      for (auto& b : f)
        for (auto& i : b) {
          spend();
          last = &i;
          if (llvm::isa< llvm::LoadInst >(i) ||
              llvm::isa< llvm::StoreInst >(i)) {
            if (!same_memory(&i))
              throw Refuse{};
          } else if (auto* c = llvm::dyn_cast< llvm::CallBase >(&i)) {
            auto* a = binding(c);
            auto* t = callee(c);
            if (!a)
              throw Refuse{};
            if (!t->isDeclaration())
              continue;
            if (creates.count(c) || nondet(c) || terminal(c))
              continue;
            if (t->getName() == "pthread_join") {
              auto* load =
                  llvm::dyn_cast< llvm::LoadInst >(c->getArgOperand(0));
              auto* slot =
                  load ? llvm::dyn_cast< llvm::GlobalVariable >(
                             load->getPointerOperand()->stripPointerCasts())
                       : nullptr;
              if (!api(c, false) || !load || !slot ||
                  load->getParent() != c->getParent() || !load->hasOneUse() ||
                  !source_integer_binding(a->argument(0), load) ||
                  !private_result(c))
                throw Refuse{};
              for (auto* next = load->getNextNode(); next != c;
                   next = next ? next->getNextNode() : nullptr)
                if (!next || next->mayReadOrWriteMemory() ||
                    llvm::isa< llvm::CallBase >(next) || next->isTerminator())
                  throw Refuse{};
              unsigned n = 0;
              for (; n < children.size() && children[n].slot != slot; ++n) {
              }
              if (n == children.size() || owners.at(&f).size() != 1 ||
                  *owners.at(&f).begin() != children[n].parent)
                throw Refuse{};
              joins.emplace(c, n);
              continue;
            }
            unsigned op = t->getName() == "pthread_mutex_init"      ? 0
                          : t->getName() == "pthread_mutex_destroy" ? 1
                          : t->getName() == "pthread_mutex_lock"    ? 2
                          : t->getName() == "pthread_mutex_unlock"  ? 3
                                                                    : 4;
            if (op == 4 || !c->use_empty() || !c->getType()->isIntegerTy(32) ||
                c->arg_size() != (op == 0 ? 2u : 1u))
              throw Refuse{};
            auto* g = llvm::dyn_cast< llvm::GlobalVariable >(
                c->getArgOperand(0)->stripPointerCasts());
            auto* ag = g ? global(g) : nullptr;
            auto* object =
                ag ? pthread_initialization_object(a->argument(0), true)
                   : nullptr;
            if (!g || !object || object != ag || !g->hasInitializer() ||
                !g->getInitializer()->isNullValue() ||
                (op == 0 &&
                 !llvm::isa< llvm::ConstantPointerNull >(c->getArgOperand(1))))
              throw Refuse{};
            unsigned n =
                std::find(mutexes.begin(), mutexes.end(), g) - mutexes.begin();
            if (n == mutexes.size())
              mutexes.push_back(g);
            const auto& roles = owners.at(&f);
            for (auto* role : roles) {
              if (op < 2 && role != main)
                throw Refuse{};
              if (op >= 2)
                for (const auto& child : children)
                  if (child.parent == role)
                    throw Refuse{};
            }
            mutex_calls.emplace(c, std::make_pair(n, op));
          } else if (auto* local = llvm::dyn_cast< llvm::AllocaInst >(&i)) {
            auto* a = dyn_cast_or_null< ar::Allocate >(original(local));
            if (!a)
              throw Refuse{};
            for (auto* u : local->users()) {
              auto* c = llvm::dyn_cast< llvm::CallBase >(u);
              if (!c || !api(c, false) || c->getArgOperand(1) != local ||
                  !private_result(c))
                throw Refuse{};
            }
          } else if (auto* p = llvm::dyn_cast< llvm::GetElementPtrInst >(&i)) {
            auto* g = llvm::dyn_cast< llvm::GlobalVariable >(
                p->getPointerOperand()->stripPointerCasts());
            if (!g || !g->isConstant() || !p->hasAllConstantIndices())
              throw Refuse{};
          } else if (!llvm::isa< llvm::BinaryOperator >(i) &&
                     !llvm::isa< llvm::CastInst >(i) &&
                     !llvm::isa< llvm::CmpInst >(i) &&
                     !llvm::isa< llvm::PHINode >(i) &&
                     !llvm::isa< llvm::SelectInst >(i) &&
                     !llvm::isa< llvm::BranchInst >(i) &&
                     !llvm::isa< llvm::SwitchInst >(i) &&
                     !llvm::isa< llvm::ReturnInst >(i) &&
                     !llvm::isa< llvm::UnreachableInst >(i))
            throw Refuse{};
        }
    }
    // Opaque objects may not alias application bytes or escape through
    // pointers.
    for (const auto& child : children)
      for (auto* u : child.slot->users()) {
        if (u == child.create)
          continue;
        auto* load = llvm::dyn_cast< llvm::LoadInst >(u);
        if (!load || !load->hasOneUse())
          throw Refuse{};
        auto* join = llvm::dyn_cast< llvm::CallBase >(*load->user_begin());
        if (!join || !joins.count(join) ||
            joins.at(join) != creates.at(child.create))
          throw Refuse{};
      }
    for (auto* g : mutexes)
      for (auto* u : g->users()) {
        auto* c = llvm::dyn_cast< llvm::CallBase >(u);
        if (!c || !mutex_calls.count(c) || c->getArgOperand(0) != g)
          throw Refuse{};
      }
    for (auto* f : ctx.concurrent_env.get_all_thread_functions()) {
      if (!f || !f->has_frontend() ||
          !roots.count(f->frontend< llvm::Function >()))
        throw Refuse{};
      std::set< std::uint64_t > covered;
      for (const auto& child : children)
        if (child.entry->getName() == f->name())
          covered.insert(ctx.concurrent_env.site_id(
              cast< ar::CallBase >(original(child.create))));
      for (auto sid : ctx.concurrent_env.get_func_instances(f->name()))
        if (!covered.count(sid))
          throw Refuse{};
    }
  }
  void analyze(llvm::Function* root, std::set< llvm::Function* >& done) {
    if (done.count(root))
      return;
    for (const auto& child : children)
      if (child.parent == root)
        analyze(child.entry, done);
    State initial;
    initial.outstanding.resize(children.size());
    initial.initialized.resize(mutexes.size(), root != main);
    for (auto& g : main->getParent()->globals())
      if (whole_global(&g))
        initial.values[&g] =
            root == main
                ? Value{true,
                        llvm::cast< llvm::ConstantInt >(g.getInitializer())
                            ->getZExtValue()}
                : Value{};
    (void)execute(root, root, std::move(initial), {});
    done.insert(root);
  }
  void prove() {
    if (!ctx.opts.enable_thread_modular ||
        ctx.opts.procedural == Procedural::Intraprocedural ||
        ctx.opts.entry_points.size() != 1 ||
        ctx.opts.entry_points.front()->name() != "main" ||
        ctx.opts.entry_points.front()->num_parameters() != 0 ||
        !ctx.opts.entry_points.front()->has_frontend() ||
        !ctx.opts.no_init_globals.empty() || ctx.concurrent_env.is_dirty() ||
        !ctx.concurrent_env.unmodeled_effects().empty() ||
        !ctx.concurrent_env.unmodeled_thread_creations().empty() ||
        ctx.bundle->global_or_null("ar.global_ctors") ||
        ctx.bundle->global_or_null("ar.global_dtors"))
      throw Refuse{};
    main = ctx.opts.entry_points.front()->frontend< llvm::Function >();
    if (!main->use_empty())
      throw Refuse{};
    if (!source_facts.index(static_cast< std::size_t >(budget) + 1) ||
        !source_facts.memory_calls_allocations().unique ||
        !source_facts.memory_calls_allocations().complete)
      throw Refuse{};
    // Cached scans still spend the original logical statement budget.
    budget -= source_facts.statement_count();
    inventory();
    std::set< llvm::Function* > done;
    analyze(main, done);
    valid = true;
  }
  llvm::Function* root(const AccessRecord& r) const {
    for (auto* f : roots)
      if (f->getName() == r.thread_id)
        return f;
    return nullptr;
  }
  llvm::Instruction* endpoint(const AccessRecord& r) const {
    auto* i = r.stmt->has_frontend() ? llvm::dyn_cast< llvm::Instruction >(
                                           r.stmt->frontend< llvm::Value >())
                                     : nullptr;
    auto* role = root(r);
    if (!i || !role || original(i) != r.stmt ||
        !owners.count(i->getFunction()) ||
        !owners.at(i->getFunction()).count(role))
      return nullptr;
    auto* l = llvm::dyn_cast< llvm::LoadInst >(i);
    auto* s = llvm::dyn_cast< llvm::StoreInst >(i);
    auto* t = l ? l->getType() : s ? s->getValueOperand()->getType() : nullptr;
    if (!t ||
        r.width != main->getParent()
                       ->getDataLayout()
                       .getTypeStoreSize(t)
                       .getFixedValue() ||
        (l ? r.kind != AccessKind::Read : r.kind != AccessKind::Write))
      return nullptr;
    return i;
  }

public:
  explicit Impl(Context& c)
      : scalar_completion(c), ctx(c), source_facts(source_program_facts(c)),
        originals(source_facts.memory_calls_allocations().statements) {}
  bool excludes(const AccessRecord& a, const AccessRecord& b) {
    scalar_last = scalar_completion.excludes(a, b);
    if (scalar_last) return true;
    if (!attempted) {
      attempted = true;
      try {
        prove();
      } catch (const Refuse&) {
        valid = false;
        absent.clear();
        if (log::is_enabled_for(LogLevel::Debug)) {
          std::string text;
          llvm::raw_string_ostream out(text);
          if (last)
            last->print(out);
          log::debug("Thread lifecycle: incomplete frame/control at " +
                     out.str());
        }
      } catch (const std::out_of_range&) {
        valid = false;
        absent.clear();
      }
    }
    if (!valid)
      return false;
    auto* x = endpoint(a);
    auto* y = endpoint(b);
    if (!x || !y)
      return false;
    // The closed spawn tree and every parent's current-generation invariant
    // also serialize all activations of a repeated entry, including its
    // helpers.
    if (root(a) == root(b))
      return true;
    auto ia = absent.find({x, root(a)}), ib = absent.find({y, root(b)});
    return (ia != absent.end() && ia->second.count(root(b))) ||
           (ib != absent.end() && ib->second.count(root(a)));
  }
  JsonDict info(const AccessRecord& a, const AccessRecord& b) const {
    if (scalar_last) return scalar_completion.info(a, b);
    auto label = [](llvm::CallBase* call) {
      unsigned position = 0;
      for (auto& block : *call->getFunction())
        for (auto& instruction : block) {
          if (&instruction == call)
            return call->getFunction()->getName().str() + "#" +
                   std::to_string(position) + " (" +
                   callee(call)->getName().str() + ")";
          ++position;
        }
      ikos_unreachable("call missing from its actual function");
    };
    JsonList families;
    for (unsigned n = 0; n < children.size(); ++n) {
      const auto& child = children[n];
      JsonDict family{{"parent", child.parent->getName().str()},
                      {"entry", child.entry->getName().str()},
                      {"create", label(child.create)}};
      std::vector< std::string > sources;
      for (const auto& join : joins)
        if (join.second == n)
          sources.push_back(label(join.first));
      std::sort(sources.begin(), sources.end());
      JsonList consumed;
      for (const auto& text : sources)
        consumed.add(text);
      family.put("joins", consumed);
      families.add(family);
    }
    return {{"thread_lifecycle_contract",
             "serial-structured-thread-lifecycle-v1"},
            {"safe_reason",
             root(a) == root(b) ? "serial activations of one entry"
                                : "all prior descendant generations drained; "
                                  "future generations not yet created"},
            {"thread_a", a.thread_id},
            {"thread_b", b.thread_id},
            {"families", families},
            {"scope",
             "finite relational fixed point; GNU ordinary join and inherited "
             "no-child effect envelope"}};
  }
};
ThreadLifecycle::ThreadLifecycle(Context& ctx) : _impl(new Impl(ctx)) {}
ThreadLifecycle::~ThreadLifecycle() = default;
bool ThreadLifecycle::excludes(const AccessRecord& a, const AccessRecord& b) {
  return _impl->excludes(a, b);
}
JsonDict ThreadLifecycle::info(const AccessRecord& a,
                               const AccessRecord& b) const {
  return _impl->info(a, b);
}

} // namespace analyzer
} // namespace ikos
