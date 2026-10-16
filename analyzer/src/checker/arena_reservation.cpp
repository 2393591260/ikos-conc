// Copyright and license: see data_race.hpp.
#include <ikos/analyzer/checker/arena_reservation.hpp>

#include <llvm/IR/Dominators.h>
#include <llvm/IR/IntrinsicInst.h>
#include <ikos/analyzer/analysis/execution_engine/native_thread_handles.hpp>
#include <ikos/analyzer/analysis/execution_engine/concurrent_semantics.hpp>
#include <ikos/analyzer/checker/data_race.hpp>
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
  // Complete reachable counter states; each success reserves [start, end).
  std::vector< std::pair< ZNumber, ZNumber > > slices;
};
struct Proof {
  llvm::GlobalVariable* arena = nullptr;
  llvm::GlobalVariable* counter = nullptr;
};
} // namespace

class ArenaReservation::Impl {
  Context& ctx;
  std::map< llvm::Value*, ar::Statement* > originals;
  std::map< llvm::GlobalVariable*, ar::GlobalVariable* > globals;
  std::map< llvm::Function*, ar::Function* > functions;
  std::vector< llvm::Function* > function_order;
  std::vector< Counter > counters;
  std::map< llvm::GlobalVariable*, bool > closed_arenas;
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
          if (!ar_call || (!ctx.native_handle_effect_certificates.count(ar_call) && !unread_private_thread_output(ar_call)) ||
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
        if (ctx.native_handle_effect_certificates.count(ar_call) ||
            ctx.native_handle_joins.count(ar_call) ||
            ctx.mutex_effect_certificates.count(ar_call)) continue;
        if (native_handle_api(c, true) && unread_private_thread_output(ar_call)) continue;
        const auto id = target->intrinsic_id();
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
    std::map< llvm::Value*, ZNumber > values;
    auto value = [&](llvm::Value* v, ZNumber& n) {
      if (constant(v, n)) return true;
      auto it = values.find(v); if (it == values.end()) return false;
      n = it->second; return true;
    };
    auto* block = &f->getEntryBlock();
    std::set< llvm::BasicBlock* > visited;
    while (visited.insert(block).second) {
      bool branched = false;
      for (auto& i : *block) {
        if (!spend()) return false;
        if (llvm::isa< llvm::DbgInfoIntrinsic >(i)) continue;
        if (auto* l = llvm::dyn_cast< llvm::LoadInst >(&i)) {
          if (l->getPointerOperand() == proof.global) values[&i] = next;
          else return false;
        } else if (auto* s = llvm::dyn_cast< llvm::StoreInst >(&i)) {
          ZNumber n(0); if (!value(s->getValueOperand(), n)) return false;
          if (s->getPointerOperand() == proof.global) { next = n; ++writes; }
          else if (s->getPointerOperand() == f->getArg(0)) { output = n; ++outputs; }
          else return false;
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
          block = br->getSuccessor(edge); branched = true; break;
        } else if (auto* ret = llvm::dyn_cast< llvm::ReturnInst >(&i)) {
          return !ret->getReturnValue() && outputs == 1 &&
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
    if (f->getName() != ar_f->name() || f->isVarArg() || f->arg_size() != 1 || !f->getReturnType()->isVoidTy() ||
        !f->getFunctionType()->getParamType(0)->isPointerTy() ||
        f->getFunctionType()->getParamType(0)->getPointerAddressSpace() != 0 ||
        !f->getFunctionType()->getParamType(0)->getPointerElementType()->isIntegerTy(32) ||
        classify_atomic_intrinsic(ar_f->name()) != AtomicKind::SingleCall) return {};
    // Inspect the complete body, not just the paths encountered during closure.
    for (auto& b : *f) for (auto& i : b) {
      if (!spend()) return {};
      if (llvm::isa< llvm::DbgInfoIntrinsic >(i)) continue;
      llvm::Value* pointer = nullptr;
      if (auto* l = llvm::dyn_cast< llvm::LoadInst >(&i)) pointer = l->getPointerOperand();
      if (auto* s = llvm::dyn_cast< llvm::StoreInst >(&i)) pointer = s->getPointerOperand();
      if (pointer) {
        if (pointer == f->getArg(0)) {
          if (!llvm::isa< llvm::StoreInst >(i) || !integer_effect(&i, pointer, ar_f->param(0))) return {};
        } else {
          auto* g = llvm::dyn_cast< llvm::GlobalVariable >(pointer);
          if (!g || !globals.count(g) || (p.global && p.global != g) ||
              !integer_effect(&i, g, globals.at(g))) return {};
          p.global = g;
        }
      } else if (!llvm::isa< llvm::BinaryOperator >(i) && !llvm::isa< llvm::ICmpInst >(i) &&
                 !llvm::isa< llvm::BranchInst >(i) && !llvm::isa< llvm::ReturnInst >(i)) return {};
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
  bool nonzero(llvm::AllocaInst* slot, llvm::Instruction* access, llvm::DominatorTree& dom) {
    for (auto& b : *slot->getFunction()) {
      auto* br = llvm::dyn_cast< llvm::BranchInst >(b.getTerminator());
      auto* cmp = br && br->isConditional() ? llvm::dyn_cast< llvm::ICmpInst >(br->getCondition()) : nullptr;
      if (!cmp || !cmp->isEquality()) continue;
      auto* load = llvm::dyn_cast< llvm::LoadInst >(cmp->getOperand(0));
      auto* zero = llvm::dyn_cast< llvm::ConstantInt >(cmp->getOperand(1));
      if (!load || load->getPointerOperand() != slot || !zero || !zero->isZero()) continue;
      unsigned edge = cmp->getPredicate() == llvm::CmpInst::ICMP_NE ? 0 : 1;
      if (dom.dominates(llvm::BasicBlockEdge(br->getParent(), br->getSuccessor(edge)), access->getParent())) return true;
    }
    return false;
  }
  bool index(llvm::Value* v, llvm::AllocaInst*& slot, ZNumber& delta,
             const Counter& counter, unsigned depth = 0) {
    if (depth == 16 || !spend()) return false;
    if (auto* l = llvm::dyn_cast< llvm::LoadInst >(v)) {
      slot = llvm::dyn_cast< llvm::AllocaInst >(l->getPointerOperand()); delta = ZNumber(0);
      return slot && private_output(slot, counter.allocator);
    }
    auto* op = llvm::dyn_cast< llvm::BinaryOperator >(v);
    ZNumber c(0);
    if (!op || !op->getType()->isIntegerTy(32) ||
        (op->getOpcode() != llvm::Instruction::Add && op->getOpcode() != llvm::Instruction::Sub) ||
        !constant(op->getOperand(1), c) || !index(op->getOperand(0), slot, delta, counter, depth + 1)) return false;
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
      llvm::AllocaInst* slot = nullptr; ZNumber delta(0);
      if (!index(gep->getOperand(2), slot, delta, counter)) continue;
      llvm::DominatorTree dom(*access->getFunction());
      if (slot->getFunction() != access->getFunction() || !nonzero(slot, access, dom)) continue;
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
  Impl(Context& context, const std::vector< AccessRecord >& records) : ctx(context) {
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
    for (auto* f : function_order) if (classify_atomic_intrinsic(functions.at(f)->name()) == AtomicKind::SingleCall &&
                                      f->arg_size() == 1 && f->getReturnType()->isVoidTy()) potential = true;
    if (!potential || !module_closed(*main->getParent())) return;
    for (auto* f : function_order) {
      if (classify_atomic_intrinsic(functions.at(f)->name()) != AtomicKind::SingleCall) continue;
      auto proof = certify_counter(f);
      if (!proof.global) continue;
      // Every actual allocator use has an activation-private, closed output.
      bool closed = !f->use_empty();
      for (auto* u : f->users()) {
        auto* c = llvm::dyn_cast< llvm::CallBase >(u);
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
    auto* pa = access(a); auto* pb = access(b);
    return pa && pb && pa->arena == pb->arena && pa->counter == pb->counter;
  }
  JsonDict info(const AccessRecord& r) const {
    auto* p = access(r);
    for (const auto& counter : counters) if (p && counter.global == p->counter)
      return {{"safe_reason", "disjoint serialized arena reservations"},
              {"arena_reservation_contract", "closed-finite-counter-slices-v1"},
              {"arena", p->arena->getName().str()},
              {"counter", p->counter->getName().str()},
              {"counter_initial", counter.slices.front().first.str()},
              {"counter_exhausted", counter.slices.back().second.str()},
              {"successful_reservations", std::to_string(counter.slices.size())},
              {"serialization", "complete defined SV-COMP named-atomic body"}};
    return {};
  }
};
ArenaReservation::ArenaReservation(Context& ctx, const std::vector< AccessRecord >& records)
    : _impl(std::make_unique< Impl >(ctx, records)) {}
ArenaReservation::~ArenaReservation() = default;
bool ArenaReservation::excludes(const AccessRecord& a, const AccessRecord& b) const { return _impl->excludes(a, b); }
JsonDict ArenaReservation::info(const AccessRecord& r) const { return _impl->info(r); }
} // namespace analyzer
} // namespace ikos
