// Copyright and license: see data_race.hpp.
#include <ikos/analyzer/checker/arena_reservation.hpp>

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Dominators.h>
#include <z3++.h>
#include <algorithm>
#include <cstdint>
#include <deque>
#include <ikos/analyzer/checker/data_race.hpp>
#include <ikos/analyzer/checker/normal_mutex.hpp>
#include <ikos/analyzer/util/log.hpp>
#include <map>
#include <set>

namespace ikos {
namespace analyzer {
namespace {
struct Refuse {};
llvm::Function* callee(llvm::CallBase* c) {
  return llvm::dyn_cast< llvm::Function >(
      c->getCalledOperand()->stripPointerCasts());
}
bool scalar(llvm::Type* t) {
  return t->isIntegerTy() && t->getIntegerBitWidth() <= 64;
}
bool plain(llvm::CallBase* c, llvm::Function* f) {
  if (!f || !llvm::isa< llvm::CallInst >(c) || c->isInlineAsm() ||
      c->hasOperandBundles() || c->getCallingConv() != llvm::CallingConv::C ||
      f->getCallingConv() != llvm::CallingConv::C ||
      c->arg_size() != f->arg_size())
    return false;
  for (unsigned n = 0; n < c->arg_size(); ++n)
    for (auto a : {llvm::Attribute::ByVal,
                   llvm::Attribute::StructRet,
                   llvm::Attribute::InAlloca,
                   llvm::Attribute::Preallocated})
      if (c->paramHasAttr(n, a) || f->hasParamAttribute(n, a))
        return false;
  if (f->isDeclaration() && f->getName() == "__VERIFIER_nondet_int" &&
      c->arg_empty() && f->getReturnType()->isIntegerTy(32) &&
      c->getType()->isIntegerTy(32) && !c->getFunctionType()->isVarArg() &&
      !c->getFunctionType()->getNumParams())
    return true;
  return !f->isVarArg() && c->getFunctionType() == f->getFunctionType();
}
struct Frame {
  llvm::Instruction* pc;
  std::map< llvm::Value*, z3::expr > values;
  std::set< llvm::Instruction* > visited;
};
struct State {
  std::vector< Frame > stack;
  std::map< llvm::GlobalVariable*, z3::expr > globals;
  z3::expr path, bound, current, lower, upper, loop_low, loop_end;
  bool held = false, acquired = false, reserved = false, cut = false,
       allocated = false, installed = false;
  unsigned writes = 0;
  explicit State(z3::context& z, llvm::Function* f)
      : stack{{&f->getEntryBlock().front(), {}, {}}},
        path(z.bool_val(true)),
        bound(z.bv_val(0, 32)),
        current(bound),
        lower(bound),
        upper(bound),
        loop_low(bound),
        loop_end(bound) {}
};
struct LengthCase {
  z3::expr path, length, size;
};
} // namespace

class SymbolicArenaReservation::Impl {
  Context& ctx;
  z3::context z;
  llvm::Function *main = nullptr, *root = nullptr;
  llvm::GlobalVariable *data = nullptr, *limit = nullptr, *counter = nullptr,
                       *mutex = nullptr;
  llvm::CallBase* allocation = nullptr;
  llvm::StoreInst* installation = nullptr;
  ar::CallBase* ar_allocation = nullptr;
  std::map< llvm::Value*, ar::Statement* > originals;
  std::map< llvm::GlobalVariable*, ar::GlobalVariable* > globals;
  std::map< llvm::Function*, ar::Function* > functions;
  std::map< llvm::CallBase*, NormalMutex > locks;
  std::set< llvm::CallBase* > creates;
  std::set< llvm::Instruction* > heap_accesses, certified;
  std::vector< LengthCase > lengths;
  std::unique_ptr< llvm::DominatorTree > dom;
  std::unique_ptr< llvm::LoopInfo > loops;
  llvm::Loop* loop = nullptr;
  llvm::PHINode* iterator = nullptr;
  llvm::Value* loop_bound = nullptr;
  uint64_t stride = 0;
  unsigned budget = 32768, queries = 512, symbols = 0;
  bool valid = false;
  void spend() {
    if (!budget--)
      throw Refuse{};
  }
  ar::Statement* original(llvm::Value* v) const {
    auto it = originals.find(v);
    return it == originals.end() ? nullptr : it->second;
  }
  ar::CallBase* binding(llvm::CallBase* c) const {
    auto* a = dyn_cast_or_null< ar::CallBase >(original(c));
    auto* f = callee(c);
    auto* t = a ? native_handle_callee(a) : nullptr;
    if (!a || !isa< ar::Call >(a) || !f || !t || !plain(c, f) ||
        a->num_arguments() != c->arg_size() ||
        t->num_parameters() != a->num_arguments() ||
        (t->has_frontend() && t->frontend< llvm::Value >() != f) ||
        (f->isDeclaration() ? !t->is_declaration()
                            : !t->is_definition() || !t->has_frontend()))
      return nullptr;
    for (unsigned n = 0; n < a->num_arguments(); ++n)
      if (a->argument(n)->type() != t->type()->param_type(n))
        return nullptr;
    return a;
  }
  bool nondet(llvm::CallBase* c) const {
    auto* a = binding(c);
    auto* t = a ? native_handle_callee(a) : nullptr;
    auto* f = callee(c);
    if (!t || !f->isDeclaration() || !c->arg_empty())
      return false;
    if (f->getName() == "__VERIFIER_nondet_int")
      return c->getType()->isIntegerTy(32) && t->has_frontend() &&
             t->frontend< llvm::Value >() == f &&
             t->type()->return_type()->is_integer() &&
             cast< ar::IntegerType >(t->type()->return_type())->bit_width() ==
                 32;
    return c->getMetadata("ikos.freeze_uninit") &&
           t->intrinsic_id() == ar::Intrinsic::IkosNonDet &&
           ((f->getName() == "__ikos_nondet_int" &&
             c->getType()->isIntegerTy(32)) ||
            (f->getName() == "__ikos_nondet_int64" &&
             c->getType()->isIntegerTy(64)));
  }
  bool terminal(llvm::CallBase* c) const {
    auto* a = binding(c);
    auto* t = a ? native_handle_callee(a) : nullptr;
    auto* f = callee(c);
    return t && f->isDeclaration() &&
           ((f->getName() == "abort" &&
             t->intrinsic_id() == ar::Intrinsic::LibcAbort && c->arg_empty() &&
             c->getType()->isVoidTy()) ||
            constant_assertion_effect(a));
  }
  bool cyclic(llvm::Instruction* i) {
    std::vector< llvm::BasicBlock* > work(llvm::succ_begin(i->getParent()),
                                          llvm::succ_end(i->getParent()));
    std::set< llvm::BasicBlock* > seen;
    while (!work.empty()) {
      spend();
      auto* b = work.back();
      work.pop_back();
      if (b == i->getParent())
        return true;
      if (seen.insert(b).second)
        work.insert(work.end(), llvm::succ_begin(b), llvm::succ_end(b));
    }
    return false;
  }
  ar::Variable* base(ar::Value* v) const {
    for (unsigned n = 0; n < 32; ++n) {
      if (isa< ar::GlobalVariable >(v) || isa< ar::LocalVariable >(v))
        return cast< ar::Variable >(v);
      auto* p = dyn_cast< ar::InternalVariable >(v);
      auto* d = p ? unique_def(p) : nullptr;
      if (auto* a = dyn_cast_or_null< ar::Assignment >(d))
        v = a->operand();
      else if (auto* a = dyn_cast_or_null< ar::UnaryOperation >(d)) {
        if (a->op() != ar::UnaryOperation::Bitcast)
          return nullptr;
        v = a->operand();
      } else
        return nullptr;
    }
    return nullptr;
  }
  bool same_index(ar::Value* value, llvm::Value* native) const {
    for (unsigned depth = 0; depth < 32; ++depth) {
      auto* type = dyn_cast< ar::IntegerType >(value->type());
      if (!type || type->bit_width() != 32 ||
          !native->getType()->isIntegerTy(32))
        return false;
      if (auto* n = dyn_cast< ar::IntegerConstant >(value)) {
        auto* c = llvm::dyn_cast< llvm::ConstantInt >(native);
        auto bits = n->value().to_z_number();
        if (bits < 0)
          bits += core::ZNumber(1) << 32;
        return c && bits == core::ZNumber(c->getZExtValue());
      }
      auto* variable = dyn_cast< ar::InternalVariable >(value);
      if (!variable)
        return false;
      if (variable->has_frontend() &&
          variable->frontend< llvm::Value >() == native)
        return true;
      auto* d = unique_def(variable);
      if (auto* copy = dyn_cast_or_null< ar::Assignment >(d))
        value = copy->operand();
      else if (auto* cast = dyn_cast_or_null< ar::UnaryOperation >(d)) {
        if (cast->op() != ar::UnaryOperation::Bitcast)
          return false;
        value = cast->operand();
      } else
        return false;
    }
    return false;
  }
  bool zero_initializer(llvm::GlobalVariable* g, bool pointer = false) const {
    if (!g || !globals.count(g) || g->isConstant() || g->isThreadLocal() ||
        g->isExternallyInitialized() || g->getAddressSpace() ||
        !g->hasInitializer() || !g->getInitializer()->isNullValue())
      return false;
    auto* a = globals.at(g);
    if (!a->is_definition() || a->is_thread_local())
      return false;
    unsigned count = 0;
    for (auto* b : *a->initializer())
      for (auto* s : *b) {
        auto* st = dyn_cast< ar::Store >(s);
        auto* n = st ? dyn_cast< ar::IntegerConstant >(st->value()) : nullptr;
        if (!st || st->pointer() != a)
          return false;
        if (pointer) {
          if (!isa< ar::NullConstant >(st->value()))
            return false;
        } else if (!n || !n->value().is_zero() || !n->type()->is_integer() ||
                   cast< ar::IntegerType >(n->type())->bit_width() != 32)
          return false;
        ++count;
      }
    return count == 1;
  }
  bool choose() {
    for (auto& f : *main->getParent())
      for (auto& b : f)
        for (auto& i : b) {
          auto* c = llvm::dyn_cast< llvm::CallBase >(&i);
          auto* t =
              c && binding(c) ? native_handle_callee(binding(c)) : nullptr;
          if (t && t->is_declaration() &&
              t->intrinsic_id() == ar::Intrinsic::LibcMalloc) {
            auto* native = callee(c);
            if (allocation || &f != main || native->getName() != "malloc" ||
                c->arg_size() != 1 ||
                !c->getArgOperand(0)->getType()->isIntegerTy(32) ||
                !c->getType()->isPointerTy() ||
                c->getType()->getPointerAddressSpace() || cyclic(c))
              return false;
            allocation = c;
            ar_allocation = binding(c);
          }
        }
    if (!allocation)
      return false;
    for (auto& f : *main->getParent())
      for (auto& b : f)
        for (auto& i : b)
          if (auto* c = llvm::dyn_cast< llvm::CallBase >(&i)) {
            if (!native_handle_api(c, true))
              continue;
            auto* a = binding(c);
            auto* child = llvm::dyn_cast< llvm::Function >(
                c->getArgOperand(2)->stripPointerCasts());
            auto* ar_child =
                a ? dyn_cast< ar::FunctionPointerConstant >(a->argument(2))
                  : nullptr;
            if (&f != main || !a || !unread_private_thread_output(a) ||
                !isa< ar::NullConstant >(a->argument(1)) ||
                !isa< ar::NullConstant >(a->argument(3)) || !ar_child ||
                !ar_child->function()->has_frontend() ||
                ar_child->function()->frontend< llvm::Value >() != child ||
                !functions.count(child) || (root && root != child))
              return false;
            root = child;
            creates.insert(c);
          }
    if (!root || creates.empty())
      return false;
    for (auto& b : *main)
      for (auto& i : b)
        if (auto* s = llvm::dyn_cast< llvm::StoreInst >(&i)) {
          if (s->getValueOperand()->stripPointerCasts() != allocation)
            continue;
          auto* g = llvm::dyn_cast< llvm::GlobalVariable >(
              s->getPointerOperand()->stripPointerCasts());
          auto* type =
              g ? llvm::dyn_cast< llvm::PointerType >(g->getValueType())
                : nullptr;
          auto* element = type ? type->getPointerElementType() : nullptr;
          if (!g || data || !element || !element->isIntegerTy() ||
              (element->getIntegerBitWidth() != 32 &&
               element->getIntegerBitWidth() != 64))
            return false;
          data = g;
          installation = s;
          stride = element->getIntegerBitWidth() / 8;
        }
    if (!zero_initializer(data, true))
      return false;
    for (auto& b : *root)
      for (auto& i : b) {
        if (auto* s = llvm::dyn_cast< llvm::StoreInst >(&i))
          if (auto* g = llvm::dyn_cast< llvm::GlobalVariable >(
                  s->getPointerOperand()->stripPointerCasts())) {
            if ((counter && counter != g) ||
                !g->getValueType()->isIntegerTy(32))
              return false;
            counter = g;
          }
      }
    if (!zero_initializer(counter))
      return false;
    for (auto& b : *root)
      for (auto& i : b)
        if (auto* l = llvm::dyn_cast< llvm::LoadInst >(&i)) {
          auto* g = llvm::dyn_cast< llvm::GlobalVariable >(
              l->getPointerOperand()->stripPointerCasts());
          if (!g || g == counter || g == data)
            continue;
          if ((limit && limit != g) || !g->getValueType()->isIntegerTy(32))
            return false;
          limit = g;
        }
    return zero_initializer(limit);
  }
  bool memory(llvm::Instruction* i) {
    auto* l = llvm::dyn_cast< llvm::LoadInst >(i);
    auto* s = llvm::dyn_cast< llvm::StoreInst >(i);
    if ((l && l->isAtomic()) || (s && s->isAtomic()))
      return false;
    auto* p = (l ? l->getPointerOperand() : s->getPointerOperand())
                  ->stripPointerCasts();
    auto* t = l ? l->getType() : s->getValueOperand()->getType();
    auto* al = dyn_cast_or_null< ar::Load >(original(i));
    auto* as = dyn_cast_or_null< ar::Store >(original(i));
    auto* at = al ? al->result()->type() : as ? as->value()->type() : nullptr;
    if ((!al && !as) || (l && !al) || (s && !as) || !at ||
        ctx.bundle->data_layout().store_size_in_bytes(at) !=
            i->getModule()->getDataLayout().getTypeStoreSize(t).getFixedValue())
      return false;
    auto* ar_base = base(al ? al->operand() : as->pointer());
    if (p == counter || p == limit || p == data) {
      auto* g = llvm::cast< llvm::GlobalVariable >(p);
      if ((i->getFunction() != root && i->getFunction() != main) ||
          ar_base != globals.at(g) || t != g->getValueType())
        return false;
      if (g == data)
        return l ? i->getFunction() == root : s == installation;
      if (!at->is_integer() || cast< ar::IntegerType >(at)->bit_width() != 32)
        return false;
      return g != limit || !s || i->getFunction() == main;
    }
    if (auto* local = llvm::dyn_cast< llvm::AllocaInst >(p)) {
      auto* a = dyn_cast_or_null< ar::Allocate >(original(local));
      return i->getFunction() == main && s && t->isIntegerTy(32) && a &&
             ar_base == a->result();
    }
    auto* gep = llvm::dyn_cast< llvm::GetElementPtrInst >(p);
    auto* origin =
        gep ? llvm::dyn_cast< llvm::LoadInst >(gep->getPointerOperand())
            : nullptr;
    auto* ar_origin =
        origin ? dyn_cast_or_null< ar::Load >(original(origin)) : nullptr;
    auto* av =
        dyn_cast< ar::InternalVariable >(al ? al->operand() : as->pointer());
    auto* shift =
        av ? dyn_cast_or_null< ar::PointerShift >(unique_def(av)) : nullptr;
    if (i->getFunction() != root || !gep || gep->getNumIndices() != 1 ||
        !origin || origin->getPointerOperand()->stripPointerCasts() != data ||
        gep->getSourceElementType() != t || !t->isIntegerTy(stride * 8) ||
        !at->is_integer() ||
        cast< ar::IntegerType >(at)->bit_width() != stride * 8 ||
        !gep->getOperand(1)->getType()->isIntegerTy(32) || !ar_origin ||
        !shift || !shift->has_frontend() ||
        shift->frontend< llvm::Value >() != gep ||
        shift->pointer() != ar_origin->result())
      return false;
    auto term = shift->term_begin();
    if (term == shift->term_end() ||
        (*term).first.to_z_number() != core::ZNumber(stride) ||
        !same_index((*term).second, gep->getOperand(1)) ||
        ++term != shift->term_end())
      return false;
    heap_accesses.insert(i);
    return true;
  }
  bool frame() {
    auto& module = *main->getParent();
    if (!module.getModuleInlineAsm().empty() ||
        !mutex_effect_pointer_representations_closed(module) || !choose())
      return false;
    llvm::DominatorTree parent_dom(*main);
    std::set< llvm::Value* > consumed;
    for (auto& f : module) {
      if (f.isDeclaration())
        continue;
      if (!functions.count(&f) || f.isVarArg() ||
          f.getCallingConv() != llvm::CallingConv::C)
        return false;
      for (unsigned k = 0; k < f.arg_size(); ++k)
        for (auto a : {llvm::Attribute::ByVal,
                       llvm::Attribute::StructRet,
                       llvm::Attribute::InAlloca,
                       llvm::Attribute::Preallocated})
          if (f.hasParamAttribute(k, a))
            return false;
      std::vector< llvm::Value* > uses{&f};
      std::set< llvm::Value* > seen;
      while (!uses.empty()) {
        spend();
        auto* v = uses.back();
        uses.pop_back();
        if (!seen.insert(v).second)
          continue;
        for (auto* u : v->users()) {
          if (llvm::isa< llvm::BitCastOperator >(u)) {
            uses.push_back(u);
            continue;
          }
          auto* c = llvm::dyn_cast< llvm::CallBase >(u);
          if (!c || !binding(c))
            return false;
          if (&f == root) {
            if (!creates.count(c) || c->getArgOperand(2) != v)
              return false;
          } else if (&f == main || c->getCalledOperand() != v)
            return false;
        }
      }
      for (auto& b : f)
        for (auto& i : b) {
          spend();
          if (llvm::isa< llvm::DbgInfoIntrinsic >(i))
            continue;
          if (llvm::isa< llvm::LoadInst >(i) ||
              llvm::isa< llvm::StoreInst >(i)) {
            if (!memory(&i))
              return false;
            consumed.insert(&i);
            if (auto* s = llvm::dyn_cast< llvm::StoreInst >(&i))
              if (&f == main &&
                  !llvm::isa< llvm::AllocaInst >(
                      s->getPointerOperand()->stripPointerCasts())) {
                if (cyclic(s))
                  return false;
                for (auto* create : creates)
                  if (!parent_dom.dominates(s, create))
                    return false;
              }
          } else if (auto* a = llvm::dyn_cast< llvm::AllocaInst >(&i)) {
            auto* n = llvm::dyn_cast< llvm::ConstantInt >(a->getArraySize());
            auto* ar = dyn_cast_or_null< ar::Allocate >(original(a));
            auto* count = ar ? dyn_cast< ar::IntegerConstant >(ar->array_size())
                             : nullptr;
            if (&f != main || !n || !n->isOne() || a->getAddressSpace() ||
                !a->getAllocatedType()->isIntegerTy(32) || !ar || !count ||
                count->value().to_z_number() != 1 ||
                !ar->allocated_type()->is_integer() ||
                cast< ar::IntegerType >(ar->allocated_type())->bit_width() !=
                    32 ||
                !ar->result()->has_frontend() ||
                ar->result()->frontend< llvm::Value >() != a)
              return false;
            // Every local use is accounted for by the independent private
            // output preflight.
            bool used = false;
            for (auto* c : creates)
              if (c->getArgOperand(0)->stripPointerCasts() == a)
                used = true;
            if (!used)
              return false;
            consumed.insert(a);
          } else if (auto* c = llvm::dyn_cast< llvm::CallBase >(&i)) {
            auto* a = binding(c);
            auto* f = callee(c);
            if (!a || !f)
              return false;
            auto m = normal_mutex(ctx, a);
            if (m.source) {
              if (c->getFunction() != root || (mutex && mutex != m.source) ||
                  cyclic(c))
                return false;
              mutex = m.source;
              locks.emplace(c, m);
            } else if (creates.count(c) || c == allocation || nondet(c) ||
                       terminal(c)) {
            } else if (f->isDeclaration() || f == main || f == root)
              return false;
            consumed.insert(c);
          } else if (!llvm::isa< llvm::BinaryOperator >(i) &&
                     !llvm::isa< llvm::CastInst >(i) &&
                     !llvm::isa< llvm::CmpInst >(i) &&
                     !llvm::isa< llvm::PHINode >(i) &&
                     !llvm::isa< llvm::SelectInst >(i) &&
                     !llvm::isa< llvm::GetElementPtrInst >(i) &&
                     !llvm::isa< llvm::BranchInst >(i) &&
                     !llvm::isa< llvm::ReturnInst >(i) &&
                     !llvm::isa< llvm::UnreachableInst >(i))
            return false;
        }
    }
    if (!mutex || heap_accesses.empty())
      return false;
    for (auto* g : {data, limit, counter, mutex}) {
      std::vector< llvm::Value* > work{g};
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
          if (auto* l = llvm::dyn_cast< llvm::LoadInst >(u)) {
            if (g == mutex || l->getPointerOperand() != v)
              return false;
          } else if (auto* s = llvm::dyn_cast< llvm::StoreInst >(u)) {
            if (g == mutex || s->getPointerOperand() != v)
              return false;
          } else if (auto* c = llvm::dyn_cast< llvm::CallBase >(u)) {
            if (g != mutex || !locks.count(c) || c->getArgOperand(0) != v)
              return false;
          } else
            return false;
        }
      }
    }
    // The one pointer installation is a noncyclic pre-create snapshot of this
    // allocation.
    if (!parent_dom.dominates(allocation, installation) || cyclic(installation))
      return false;
    for (const auto& e : originals)
      if (!consumed.count(e.first))
        return false;
    dom = std::make_unique< llvm::DominatorTree >(*root);
    loops = std::make_unique< llvm::LoopInfo >(*dom);
    if (std::distance(loops->begin(), loops->end()) != 1)
      return false;
    loop = *loops->begin();
    if (!loop->getSubLoops().empty() || !loop->getLoopPreheader() ||
        !loop->getLoopLatch() || !loop->getExitBlock())
      return false;
    auto* branch =
        llvm::dyn_cast< llvm::BranchInst >(loop->getHeader()->getTerminator());
    auto* cmp = branch && branch->isConditional()
                    ? llvm::dyn_cast< llvm::ICmpInst >(branch->getCondition())
                    : nullptr;
    if (!cmp || cmp->getPredicate() != llvm::CmpInst::ICMP_SLT ||
        !loop->contains(branch->getSuccessor(0)) ||
        loop->contains(branch->getSuccessor(1)))
      return false;
    iterator = llvm::dyn_cast< llvm::PHINode >(cmp->getOperand(0));
    loop_bound = cmp->getOperand(1);
    if (!iterator || iterator->getParent() != loop->getHeader() ||
        !iterator->getType()->isIntegerTy(32) ||
        iterator->getNumIncomingValues() != 2 ||
        (llvm::isa< llvm::Instruction >(loop_bound) &&
         loop->contains(llvm::cast< llvm::Instruction >(loop_bound))))
      return false;
    for (auto& i : *loop->getHeader())
      if (llvm::isa< llvm::PHINode >(i) && &i != iterator)
        return false;
    for (auto* access : heap_accesses)
      if (!loop->contains(access))
        return false;
    return true;
  }
  z3::expr fresh(unsigned bits) {
    return z.bv_const(("slice_" + std::to_string(symbols++)).c_str(), bits);
  }
  static void set(Frame& f, llvm::Value* v, const z3::expr& x) {
    f.values.erase(v);
    f.values.emplace(v, x);
  }
  static void set_global(State& s, llvm::GlobalVariable* g, const z3::expr& x) {
    s.globals.erase(g);
    s.globals.emplace(g, x);
  }
  z3::expr value(Frame& f, llvm::Value* v) {
    if (!scalar(v->getType()) || llvm::isa< llvm::UndefValue >(v) ||
        llvm::isa< llvm::PoisonValue >(v))
      throw Refuse{};
    auto found = f.values.find(v);
    if (found != f.values.end())
      return found->second;
    if (auto* n = llvm::dyn_cast< llvm::ConstantInt >(v))
      return z.bv_val(n->getZExtValue(), n->getBitWidth());
    auto x = fresh(v->getType()->getIntegerBitWidth());
    set(f, v, x);
    return x;
  }
  z3::check_result query(const z3::expr& claim) {
    if (!queries--)
      throw Refuse{};
    z3::solver solver(z);
    z3::params p(z);
    p.set("timeout", 100u);
    solver.set(p);
    solver.add(claim);
    auto result = solver.check();
    if (result == z3::unknown)
      throw Refuse{};
    return result;
  }
  void require(const State& s, const z3::expr& claim) {
    if (query(s.path && !claim) != z3::unsat) {
      throw Refuse{};
    }
  }
  z3::expr operation(State& s, llvm::Instruction* i) {
    auto& f = s.stack.back();
    auto operand = [&](unsigned n) { return value(f, i->getOperand(n)); };
    auto unknown = [&]() { return fresh(i->getType()->getIntegerBitWidth()); };
    if (auto* c = llvm::dyn_cast< llvm::ICmpInst >(i)) {
      if (!scalar(c->getOperand(0)->getType()))
        return unknown();
      auto a = operand(0), b = operand(1), test = z.bool_val(true);
      switch (c->getPredicate()) {
        case llvm::CmpInst::ICMP_EQ:
          test = a == b;
          break;
        case llvm::CmpInst::ICMP_NE:
          test = a != b;
          break;
        case llvm::CmpInst::ICMP_SLT:
          test = a < b;
          break;
        case llvm::CmpInst::ICMP_SLE:
          test = a <= b;
          break;
        case llvm::CmpInst::ICMP_SGT:
          test = a > b;
          break;
        case llvm::CmpInst::ICMP_SGE:
          test = a >= b;
          break;
        case llvm::CmpInst::ICMP_ULT:
          test = z3::ult(a, b);
          break;
        case llvm::CmpInst::ICMP_ULE:
          test = z3::ule(a, b);
          break;
        case llvm::CmpInst::ICMP_UGT:
          test = z3::ugt(a, b);
          break;
        case llvm::CmpInst::ICMP_UGE:
          test = z3::uge(a, b);
          break;
        default:
          return unknown();
      }
      return z3::ite(test, z.bv_val(1, 1), z.bv_val(0, 1));
    }
    if (auto* c = llvm::dyn_cast< llvm::CastInst >(i)) {
      if (!scalar(c->getOperand(0)->getType()))
        return unknown();
      auto a = operand(0);
      unsigned before = c->getOperand(0)->getType()->getIntegerBitWidth(),
               after = c->getType()->getIntegerBitWidth();
      if (c->getOpcode() == llvm::Instruction::SExt)
        return z3::sext(a, after - before);
      if (c->getOpcode() == llvm::Instruction::ZExt)
        return z3::zext(a, after - before);
      if (c->getOpcode() == llvm::Instruction::Trunc)
        return a.extract(after - 1, 0);
      if (c->getOpcode() == llvm::Instruction::BitCast && before == after)
        return a;
    }
    if (auto* b = llvm::dyn_cast< llvm::BinaryOperator >(i)) {
      auto a = operand(0), c = operand(1), result = a;
      switch (b->getOpcode()) {
        case llvm::Instruction::Add:
          result = a + c;
          break;
        case llvm::Instruction::Sub:
          result = a - c;
          break;
        case llvm::Instruction::Mul:
          result = a * c;
          break;
        case llvm::Instruction::And:
          return a & c;
        case llvm::Instruction::Or:
          return a | c;
        case llvm::Instruction::Xor:
          return a ^ c;
        default:
          return unknown();
      }
      auto math = [&](bool sign) {
        const auto width = b->getType()->getIntegerBitWidth();
        auto x = sign ? z3::sext(a, width) : z3::zext(a, width);
        auto y = sign ? z3::sext(c, width) : z3::zext(c, width);
        return b->getOpcode() == llvm::Instruction::Add   ? x + y
               : b->getOpcode() == llvm::Instruction::Sub ? x - y
                                                          : x * y;
      };
      if (b->hasNoSignedWrap())
        require(s,
                z3::sext(result, b->getType()->getIntegerBitWidth()) ==
                    math(true));
      if (b->hasNoUnsignedWrap())
        require(s,
                z3::zext(result, b->getType()->getIntegerBitWidth()) ==
                    math(false));
      return result;
    }
    if (llvm::isa< llvm::SelectInst >(i))
      return z3::ite(operand(0) != z.bv_val(0, 1), operand(1), operand(2));
    return unknown();
  }
  void enter(State& s,
             llvm::BasicBlock* from,
             llvm::BasicBlock* to,
             bool worker) {
    auto& f = s.stack.back();
    std::vector< std::pair< llvm::PHINode*, z3::expr > > updates;
    for (auto& i : *to) {
      auto* phi = llvm::dyn_cast< llvm::PHINode >(&i);
      if (!phi)
        break;
      if (scalar(phi->getType()))
        updates.emplace_back(phi,
                             value(f, phi->getIncomingValueForBlock(from)));
    }
    if (worker && to == loop->getHeader()) {
      auto found =
          std::find_if(updates.begin(), updates.end(), [&](const auto& p) {
            return p.first == iterator;
          });
      if (found == updates.end() || s.held || !s.acquired)
        throw Refuse{};
      auto c = found->second, end = value(f, loop_bound);
      if (loop->contains(from)) {
        if (!s.cut || !s.reserved)
          throw Refuse{};
        require(s, c >= s.loop_low && c <= s.loop_end);
        // Preservation of an arbitrary iteration, not a finite unfolding.
        f.pc = nullptr;
        return;
      }
      if (s.cut)
        throw Refuse{};
      if (!s.reserved)
        require(s, !(c < end));
      else {
        require(s, s.lower <= c && c <= end && end <= s.upper);
        s.loop_low = c;
        s.loop_end = end;
        auto arbitrary = fresh(32);
        // The only loop-carried value is the iterator. All other loop-defined
        // SSA values execute afresh; preheader snapshots remain unchanged.
        s.path = s.path && (arbitrary >= c) && (arbitrary <= end);
        found->second = arbitrary;
        s.cut = true;
      }
    }
    for (const auto& p : updates)
      set(f, p.first, p.second);
    f.pc = to->getFirstNonPHI();
  }
  void execute(State start, bool worker) {
    std::deque< State > work{std::move(start)};
    while (!work.empty()) {
      spend();
      auto s = std::move(work.front());
      work.pop_front();
      auto& f = s.stack.back();
      auto* i = f.pc;
      if (!i)
        continue;
      if (query(s.path) == z3::unsat)
        continue;
      if (!f.visited.insert(i).second)
        throw Refuse{};
      if (llvm::isa< llvm::DbgInfoIntrinsic >(i)) {
        f.pc = i->getNextNode();
        work.push_back(std::move(s));
        continue;
      }
      if (auto* b = llvm::dyn_cast< llvm::BranchInst >(i)) {
        auto c = b->isConditional()
                     ? value(f, b->getCondition()) != z.bv_val(0, 1)
                     : z.bool_val(true);
        for (unsigned k = 0; k < b->getNumSuccessors(); ++k) {
          auto next = s;
          if (b->isConditional())
            next.path = next.path && (k ? !c : c);
          if (query(next.path) == z3::unsat)
            continue;
          enter(next, i->getParent(), b->getSuccessor(k), worker);
          work.push_back(std::move(next));
        }
        continue;
      }
      if (auto* ret = llvm::dyn_cast< llvm::ReturnInst >(i)) {
        if (s.stack.size() == 1) {
          if (s.held)
            throw Refuse{};
          continue;
        }
        auto* returned = ret->getReturnValue();
        auto result = returned ? value(f, returned) : z.bv_val(0, 1);
        s.stack.pop_back();
        auto& caller = s.stack.back();
        if (returned)
          set(caller, caller.pc, result);
        caller.pc = caller.pc->getNextNode();
        work.push_back(std::move(s));
        continue;
      }
      if (llvm::isa< llvm::UnreachableInst >(i))
        throw Refuse{};
      if (auto* c = llvm::dyn_cast< llvm::CallBase >(i)) {
        if (terminal(c))
          continue;
        if (creates.count(c)) {
          if (worker || !s.allocated || !s.installed ||
              !s.globals.count(limit) || !s.globals.count(counter))
            throw Refuse{};
          auto n = s.globals.at(limit);
          auto extent = value(f, allocation->getArgOperand(0));
          require(s, s.globals.at(counter) == z.bv_val(0, 32));
          auto length = z3::sext(n, 32), size = z3::zext(extent, 32);
          require(s,
                  n > z.bv_val(0, 32) &&
                      size == length * z.bv_val(stride, 64) &&
                      z3::ult(size, z.bv_val(UINT64_C(4294967296), 64)));
          lengths.push_back({s.path, n, extent});
          // Noncyclic global writes dominate every create. The suffix cannot
          // change this length/allocation; later workers do not inherit next=0.
          continue;
        }
        auto m = locks.find(c);
        if (m != locks.end()) {
          if (!worker)
            throw Refuse{};
          if (m->second.lock) {
            if (s.held || s.acquired)
              throw Refuse{};
            s.held = true;
            s.acquired = true;
            s.lower = s.current;
          } else {
            if (!s.held)
              throw Refuse{};
            if (s.writes) {
              require(s, s.current > s.lower && s.current <= s.bound);
              s.reserved = true;
              s.upper = s.current;
            }
            s.held = false;
          }
        } else if (c == allocation) {
          if (worker || s.allocated)
            throw Refuse{};
          s.allocated = true;
        } else if (nondet(c)) {
          set(f, c, fresh(c->getType()->getIntegerBitWidth()));
        } else if (worker) {
          // Complete frame proves helper memory/synchronization-free. A return
          // projection adds behaviors and does not assume its assertion/result.
          if (scalar(c->getType()))
            set(f, c, fresh(c->getType()->getIntegerBitWidth()));
        } else {
          auto* child = callee(c);
          if (!child || child->isDeclaration() || s.stack.size() == 32)
            throw Refuse{};
          for (const auto& frame : s.stack)
            if (frame.pc->getFunction() == child)
              throw Refuse{};
          Frame next{&child->getEntryBlock().front(), {}, {}};
          for (unsigned k = 0; k < c->arg_size(); ++k)
            if (scalar(c->getArgOperand(k)->getType()))
              set(next, child->getArg(k), value(f, c->getArgOperand(k)));
          s.stack.push_back(std::move(next));
          work.push_back(std::move(s));
          continue;
        }
      } else if (auto* load = llvm::dyn_cast< llvm::LoadInst >(i)) {
        auto* p = load->getPointerOperand()->stripPointerCasts();
        if (p == counter) {
          if (!worker || !s.held)
            throw Refuse{};
          set(f, i, s.current);
        } else if (p == limit) {
          if (worker)
            set(f, i, s.bound);
          else {
            if (!s.globals.count(limit))
              throw Refuse{};
            set(f, i, s.globals.at(limit));
          }
        } else if (p == data) {
        } else if (!heap_accesses.count(i))
          throw Refuse{};
      } else if (auto* store = llvm::dyn_cast< llvm::StoreInst >(i)) {
        auto* p = store->getPointerOperand()->stripPointerCasts();
        if (p == counter) {
          auto n = value(f, store->getValueOperand());
          if (worker) {
            if (!s.held || s.writes++)
              throw Refuse{};
            s.current = n;
          } else
            set_global(s, counter, n);
        } else if (p == limit) {
          if (worker)
            throw Refuse{};
          set_global(s, limit, value(f, store->getValueOperand()));
        } else if (store == installation) {
          if (!s.allocated || s.installed)
            throw Refuse{};
          s.installed = true;
        }
      } else if (scalar(i->getType()))
        set(f, i, operation(s, i));
      if (heap_accesses.count(i)) {
        if (!worker || s.held || !s.reserved || !s.cut)
          throw Refuse{};
        auto* l = llvm::dyn_cast< llvm::LoadInst >(i);
        auto* st = llvm::dyn_cast< llvm::StoreInst >(i);
        auto* gep = llvm::cast< llvm::GetElementPtrInst >(
            (l ? l->getPointerOperand() : st->getPointerOperand())
                ->stripPointerCasts());
        auto index = value(f, gep->getOperand(1));
        require(s,
                index >= s.lower && index < s.upper &&
                    index >= z.bv_val(0, 32));
        // Whole-element stride and upper<=L combine with size=L*stride<2^32
        // to establish complete nonwrapping byte intervals.
        if (l)
          set(f, i, fresh(l->getType()->getIntegerBitWidth()));
        certified.insert(i);
      }
      f.pc = i->getNextNode();
      work.push_back(std::move(s));
    }
  }
  bool endpoint(const AccessRecord& r) const {
    if (!valid || !r.stmt->has_frontend() || !r.pts.is_set() ||
        r.pts.is_empty() || r.width != stride)
      return false;
    auto* i =
        llvm::dyn_cast< llvm::Instruction >(r.stmt->frontend< llvm::Value >());
    if (!i || !certified.count(i) || original(i) != r.stmt ||
        (llvm::isa< llvm::LoadInst >(i) ? r.kind != AccessKind::Read
                                        : r.kind != AccessKind::Write))
      return false;
    for (auto* object : r.pts) {
      auto* heap = dyn_cast< DynAllocMemoryLocation >(object);
      if (!heap || heap->call() != ar_allocation)
        return false;
    }
    return true;
  }

public:
  explicit Impl(Context& c) : ctx(c) {
    if (!ctx.opts.enable_thread_modular ||
        ctx.opts.procedural == Procedural::Intraprocedural ||
        ctx.opts.entry_points.size() != 1 ||
        ctx.opts.entry_points.front()->name() != "main" ||
        !ctx.opts.entry_points.front()->has_frontend() ||
        ctx.bundle->data_layout().pointers.bit_width != 32 ||
        !ctx.opts.no_init_globals.empty() || ctx.concurrent_env.is_dirty() ||
        !ctx.concurrent_env.unmodeled_effects().empty() ||
        !ctx.concurrent_env.unmodeled_thread_creations().empty() ||
        ctx.bundle->global_or_null("ar.global_ctors") ||
        ctx.bundle->global_or_null("ar.global_dtors"))
      return;
    main = ctx.opts.entry_points.front()->frontend< llvm::Function >();
    if (!main->use_empty())
      return;
    if (main->arg_size() != 0 && main->arg_size() != 2)
      return;
    if (!main->getReturnType()->isIntegerTy(32) ||
        ctx.opts.entry_points.front()->num_parameters() != main->arg_size())
      return;
    if (main->arg_size() == 2) {
      auto* argv =
          llvm::dyn_cast< llvm::PointerType >(main->getArg(1)->getType());
      auto* string = argv ? llvm::dyn_cast< llvm::PointerType >(
                                argv->getPointerElementType())
                          : nullptr;
      if (!main->getArg(0)->getType()->isIntegerTy(32) || !argv ||
          argv->getAddressSpace() || !string || string->getAddressSpace() ||
          !string->getPointerElementType()->isIntegerTy(8))
        return;
    }
    for (auto& a : main->args())
      if (!a.use_empty())
        return;
    // No expensive symbolic preflight on modules without a candidate allocator.
    bool potential = false;
    for (auto& f : *main->getParent())
      if (f.isDeclaration() && f.getName() == "malloc" && !f.use_empty())
        potential = true;
    if (!potential)
      return;
    for (auto it = ctx.bundle->global_begin(); it != ctx.bundle->global_end();
         ++it)
      if ((*it)->has_frontend())
        if (auto* g = llvm::dyn_cast< llvm::GlobalVariable >(
                (*it)->frontend< llvm::Value >()))
          globals.emplace(g, *it);
    for (auto it = ctx.bundle->function_begin();
         it != ctx.bundle->function_end();
         ++it) {
      auto* f = *it;
      if (!f->is_definition() || !f->has_frontend())
        continue;
      auto* native = f->frontend< llvm::Function >();
      functions.emplace(native, f);
      for (auto* b : *f->body())
        for (auto* s : *b) {
          if (isa< ar::CallBase >(s) || isa< ar::Load >(s) ||
              isa< ar::Store >(s) || isa< ar::Allocate >(s)) {
            auto* i = s->has_frontend() ? llvm::dyn_cast< llvm::Instruction >(
                                              s->frontend< llvm::Value >())
                                        : nullptr;
            if (!i || i->getFunction() != native ||
                !originals.emplace(i, s).second)
              return;
          } else if (!isa< ar::Assignment >(s) &&
                     !isa< ar::UnaryOperation >(s) &&
                     !isa< ar::BinaryOperation >(s) &&
                     !isa< ar::Comparison >(s) && !isa< ar::PointerShift >(s) &&
                     !isa< ar::ReturnValue >(s) && !isa< ar::Unreachable >(s))
            return;
        }
    }
    try {
      if (!frame()) {
        log::debug("Symbolic arena frame refused");
        return;
      }
      State parent(z, main);
      execute(std::move(parent), false);
      if (lengths.empty())
        return;
      for (const auto& length : lengths) {
        State worker(z, root);
        worker.bound = length.length;
        worker.current = fresh(32);
        // Shared counter interference hull, not the first-create snapshot.
        worker.path = length.path && (worker.current >= z.bv_val(0, 32)) &&
                      (worker.current <= worker.bound);
        execute(std::move(worker), true);
      }
      valid = !certified.empty();
    } catch (const Refuse&) {
      log::debug("Symbolic arena invariant refused");
    } catch (const z3::exception&) {
      log::debug("Symbolic arena solver refused");
    }
  }
  bool excludes(const AccessRecord& a, const AccessRecord& b) const {
    return endpoint(a) && endpoint(b);
  }
  JsonDict info(const AccessRecord& a) const {
    if (!endpoint(a))
      return {};
    return {{"safe_reason", "disjoint serialized symbolic live-heap slices"},
            {"arena_reservation_contract", "closed-symbolic-bounded-slices-v1"},
            {"counter", counter->getName().str()},
            {"arena_pointer", data->getName().str()},
            {"reservation_mutex", mutex->getName().str()},
            {"element_bytes", stride},
            {"length_cases", static_cast< uint64_t >(lengths.size())}};
  }
};
SymbolicArenaReservation::SymbolicArenaReservation(Context& c)
    : _impl(std::make_unique< Impl >(c)) {}
SymbolicArenaReservation::~SymbolicArenaReservation() = default;
bool SymbolicArenaReservation::excludes(const AccessRecord& a,
                                        const AccessRecord& b) const {
  return _impl->excludes(a, b);
}
JsonDict SymbolicArenaReservation::info(const AccessRecord& a) const {
  return _impl->info(a);
}
} // namespace analyzer
} // namespace ikos
