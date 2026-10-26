// Copyright and license: see data_race.hpp.
#include <ikos/analyzer/checker/self_publication.hpp>

#include <llvm/IR/CFG.h>
#include <z3++.h>
#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <tuple>
#include <vector>
#include <ikos/analyzer/checker/data_race.hpp>
#include <ikos/analyzer/analysis/source_call_binding.hpp>
#include <ikos/analyzer/checker/normal_mutex.hpp>
#include <ikos/analyzer/util/log.hpp>

namespace ikos {
namespace analyzer {
namespace {
llvm::Function* callee(llvm::CallBase* c) {
  return llvm::dyn_cast<llvm::Function>(c->getCalledOperand()->stripPointerCasts());
}
bool plain_call(llvm::CallBase* c, llvm::Function* f) {
  if (!f || !llvm::isa<llvm::CallInst>(c) || c->isInlineAsm() || c->hasOperandBundles() ||
      f->isVarArg() || c->getFunctionType() != f->getFunctionType() ||
      c->getCallingConv() != llvm::CallingConv::C || f->getCallingConv() != llvm::CallingConv::C ||
      c->arg_size() != f->arg_size()) return false;
  for (unsigned i = 0; i < c->arg_size(); ++i)
    for (auto a : {llvm::Attribute::ByVal, llvm::Attribute::StructRet,
                   llvm::Attribute::InAlloca, llvm::Attribute::Preallocated})
      if (c->paramHasAttr(i, a) || f->hasParamAttribute(i, a)) return false;
  return true;
}
// C's unprototyped zero-argument verifier declaration can be an LLVM vararg
// function or a bitcast to i32(). Only this actual declaration/API is normalized.
bool nondet_native(llvm::CallBase* c) {
  auto* f = callee(c);
  return f && f->isDeclaration() && f->getName() == "__VERIFIER_nondet_int" &&
      llvm::isa<llvm::CallInst>(c) && !c->isInlineAsm() && !c->hasOperandBundles() &&
      c->arg_empty() && f->arg_empty() && f->getReturnType()->isIntegerTy(32) &&
      c->getFunctionType()->getReturnType()->isIntegerTy(32) &&
      c->getFunctionType()->getNumParams() == 0 &&
      c->getCallingConv() == llvm::CallingConv::C && f->getCallingConv() == llvm::CallingConv::C;
}
struct Value {
  bool known = false;
  uint64_t bits = 0;
  bool operator<(const Value& o) const { return std::tie(known,bits) < std::tie(o.known,o.bits); }
  bool operator==(const Value& o) const { return known == o.known && bits == o.bits; }
};
struct State {
  unsigned pc = 0;
  std::vector<Value> values;
  bool post = false, held = false, final = false, entry_final = false;
  unsigned stores = 0;
  bool operator<(const State& o) const {
    return std::tie(pc,values,post,held,final,entry_final,stores) <
           std::tie(o.pc,o.values,o.post,o.held,o.final,o.entry_final,o.stores);
  }
};
struct UnsupportedPrefix {};
struct SymbolicFrame {
  llvm::Instruction* pc;
  std::map<llvm::Value*,z3::expr> values;
  std::set<llvm::Instruction*> visited;
};
struct SymbolicState {
  std::vector<SymbolicFrame> stack;
  z3::expr condition;
};
} // namespace

class SelfPublication::Impl {
  Context& ctx;
  std::map<llvm::Value*, ar::Statement*> originals;
  std::map<llvm::Function*, ar::Function*> functions;
  llvm::Function* main = nullptr;
  llvm::Function* body = nullptr;
  llvm::GlobalVariable *payload = nullptr, *array = nullptr, *mutex = nullptr;
  ar::GlobalVariable *ar_payload = nullptr, *ar_array = nullptr;
  llvm::StoreInst* initialization = nullptr;
  std::vector<llvm::Function*> roots;
  std::map<llvm::CallBase*,NormalMutex> mutex_calls;
  std::map<llvm::Instruction*,unsigned> pcs, slots;
  std::vector<llvm::Instruction*> instructions{nullptr};
  std::set<std::vector<uint64_t>> arguments;
  std::map<llvm::Instruction*,bool> post;
  uint64_t initial_bits = 0, elements = 0, element_bytes = 0;
  bool valid = false, saw_write = false;
  unsigned budget = 262144, solver_budget = 256, symbol_id = 0;
  unsigned word = 0;
  std::set< llvm::AllocaInst* > outputs;
  bool spend() { if (!budget) return false; --budget; return true; }
  ar::Statement* original(llvm::Value* v) const {
    auto it = originals.find(v); return it == originals.end() ? nullptr : it->second;
  }
  ar::CallBase* binding(llvm::CallBase* c) const {
    auto* a = dyn_cast_or_null< ar::CallBase >(original(c));
    auto* target = a ? native_handle_callee(a) : nullptr;
    auto* f = callee(c);
    if (!f || !target || !isa< ar::Call >(a) || !(plain_call(c, f) || nondet_native(c)) || !target->has_frontend() ||
        target->frontend< llvm::Value >() != f || a->num_arguments() != c->arg_size() ||
        target->num_parameters() != a->num_arguments() || (target->type()->is_var_arg() && !nondet_native(c)) ||
        !(f->isDeclaration() ? target->is_declaration() : target->is_definition())) return nullptr;
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
  bool terminal(llvm::CallBase* c) const {
    auto* a = binding(c); auto* n = callee(c); auto* f = a ? native_handle_callee(a) : nullptr;
    return a && f && n->isDeclaration() &&
        ((n->getName() == "abort" && f->intrinsic_id() == ar::Intrinsic::LibcAbort &&
          c->arg_empty() && c->getType()->isVoidTy()) || constant_assertion_effect(a));
  }
  bool nondet(llvm::CallBase* c) const {
    auto* a = binding(c); auto* n = callee(c); auto* f = a ? native_handle_callee(a) : nullptr;
    if (!f || !n->isDeclaration() || !c->arg_empty()) return false;
    if (nondet_native(c)) {
      auto* type = dyn_cast<ar::IntegerType>(f->type()->return_type());
      return type && type->bit_width() == 32 && type->is_signed();
    }
    return c->getMetadata("ikos.freeze_uninit") && f->intrinsic_id() == ar::Intrinsic::IkosNonDet &&
        ((n->getName() == "__ikos_nondet_int" && c->getType()->isIntegerTy(32)) ||
         (n->getName() == "__ikos_nondet_int64" && c->getType()->isIntegerTy(64)));
  }
  bool create(llvm::CallBase* c) const {
    auto* a = binding(c);
    if (!a || c->arg_size() != 4) return false;
    auto* slot = llvm::dyn_cast< llvm::AllocaInst >(c->getArgOperand(0)->stripPointerCasts());
    auto* storage = slot ? dyn_cast_or_null< ar::Allocate >(original(slot)) : nullptr;
    return a && c->getFunction() == main && native_handle_api(c, true) && unread_private_thread_output(a) &&
           slot && storage && slot->getAllocatedType()->isIntegerTy(word * 8) &&
           slot->getAlign().value() >= word && address_base(a->argument(0)) == storage->result() &&
           ctx.bundle->data_layout().alloc_size_in_bytes(storage->allocated_type()) == word &&
           c->getModule()->getDataLayout().getTypeAllocSize(slot->getAllocatedType()) == word &&
           isa<ar::NullConstant>(a->argument(1)) && isa<ar::NullConstant>(a->argument(3));
  }
  bool cyclic(llvm::BasicBlock* start) {
    std::vector<llvm::BasicBlock*> work(llvm::succ_begin(start),llvm::succ_end(start));
    std::set<llvm::BasicBlock*> seen;
    while (!work.empty()) {
      if (!spend()) return true;
      auto* b = work.back(); work.pop_back();
      if (b == start) return true;
      if (seen.insert(b).second) work.insert(work.end(),llvm::succ_begin(b),llvm::succ_end(b));
    }
    return false;
  }
  ar::GlobalVariable* global(llvm::GlobalVariable* n) const {
    for (auto it = ctx.bundle->global_begin(); it != ctx.bundle->global_end(); ++it)
      if ((*it)->has_frontend() && (*it)->frontend<llvm::Value>() == n) return *it;
    return nullptr;
  }
  bool choose_objects() {
    for (auto& f : *main->getParent()) for (auto& b : f) for (auto& i : b) {
      auto* s = llvm::dyn_cast<llvm::StoreInst>(&i);
      auto* g = s ? llvm::dyn_cast<llvm::GlobalVariable>(s->getPointerOperand()->stripPointerCasts()) : nullptr;
      if (!g) continue;
      auto* n = g->hasInitializer() ? llvm::dyn_cast<llvm::ConstantInt>(g->getInitializer()) : nullptr;
      if (!n || n->isZero() || (n->getBitWidth() != 32 && n->getBitWidth() != 64) || initialization) return false;
      initialization = s; payload = g; body = &f; initial_bits = n->getZExtValue();
    }
    if (!initialization || body == main || !body->getReturnType()->isVoidTy() || body->arg_size() > 4) return false;
    for (auto& a : body->args()) if (!a.getType()->isIntegerTy() || a.getType()->getIntegerBitWidth() > 64) return false;
    ar_payload = global(payload);
    if (!ar_payload || !ar_payload->is_definition() || ar_payload->is_thread_local() ||
        payload->isConstant() || payload->isThreadLocal() || payload->isExternallyInitialized() || payload->getAddressSpace()) return false;
    unsigned count = 0;
    for (auto* b : *ar_payload->initializer()) for (auto* s : *b) {
      auto* store = dyn_cast<ar::Store>(s); auto* n = store ? dyn_cast<ar::IntegerConstant>(store->value()) : nullptr;
      auto* t = n ? dyn_cast<ar::IntegerType>(n->type()) : nullptr;
      auto bits = n ? n->value().to_z_number() : core::ZNumber(0);
      if (n && bits < 0) bits += core::ZNumber(1) << t->bit_width();
      if (!store || store->pointer() != ar_payload || !t || t->bit_width() != payload->getValueType()->getIntegerBitWidth() ||
          bits != core::ZNumber(initial_bits)) return false;
      ++count;
    }
    if (count != 1) return false;
    for (auto& g : main->getParent()->globals()) {
      auto* t = llvm::dyn_cast<llvm::ArrayType>(g.getValueType());
      if (!t || !t->getElementType()->isIntegerTy() || !g.hasInitializer() ||
          !g.getInitializer()->isNullValue()) continue;
      if (array || g.isThreadLocal() || g.isExternallyInitialized() || g.getAddressSpace() ||
          !t->getNumElements() || t->getNumElements() > 64 ||
          (t->getElementType()->getIntegerBitWidth() != 32 && t->getElementType()->getIntegerBitWidth() != 64)) return false;
      array = &g; elements = t->getNumElements(); element_bytes = t->getElementType()->getIntegerBitWidth()/8;
    }
    ar_array = array ? global(array) : nullptr;
    if (!ar_array || !ar_array->is_definition() || ar_array->is_thread_local() ||
        !ar_array->type()->pointee()->is_array() ||
        ctx.bundle->data_layout().alloc_size_in_bytes(ar_array->type()->pointee()) != elements*element_bytes) return false;
    auto* actual_array = llvm::cast< llvm::ArrayType >(array->getValueType());
    auto* imported_array = cast< ar::ArrayType >(ar_array->type()->pointee());
    const auto& layout = main->getParent()->getDataLayout();
    if (layout.getTypeAllocSize(actual_array->getElementType()) != element_bytes ||
        layout.getTypeStoreSize(actual_array->getElementType()) != element_bytes ||
        layout.getTypeAllocSize(actual_array) != elements * element_bytes ||
        imported_array->num_elements() != core::ZNumber(elements) ||
        !imported_array->element_type()->is_integer() ||
        cast< ar::IntegerType >(imported_array->element_type())->bit_width() != element_bytes * 8 ||
        ctx.bundle->data_layout().store_size_in_bytes(imported_array->element_type()) != element_bytes)
      return false;
    count = 0;
    for (auto* b : *ar_array->initializer()) for (auto* s : *b) {
      auto* store = dyn_cast<ar::Store>(s);
      if (!store || store->pointer() != ar_array || !isa<ar::AggregateZeroConstant>(store->value()) ||
          store->value()->type() != ar_array->type()->pointee()) return false;
      ++count;
    }
    return count == 1;
  }
  bool scalar_memory(llvm::Instruction& i) const {
    auto* l = llvm::dyn_cast<llvm::LoadInst>(&i); auto* s = llvm::dyn_cast<llvm::StoreInst>(&i);
    if ((l && l->isAtomic()) || (s && s->isAtomic())) return false;
    auto* address = (l ? l->getPointerOperand() : s->getPointerOperand())->stripPointerCasts();
    auto* type = l ? l->getType() : s->getValueOperand()->getType();
    auto* a = original(&i); auto* al = dyn_cast_or_null<ar::Load>(a); auto* as = dyn_cast_or_null<ar::Store>(a);
    auto* at = al ? al->result()->type() : as ? as->value()->type() : nullptr;
    if (!type->isIntegerTy() || !at || !at->is_integer() || (l && !al) || (s && !as) ||
        cast<ar::IntegerType>(at)->bit_width() != type->getIntegerBitWidth() ||
        ctx.bundle->data_layout().store_size_in_bytes(at) != i.getModule()->getDataLayout().getTypeStoreSize(type).getFixedValue()) return false;
    auto* base = al ? address_base(al->operand()) : address_base(as->pointer());
    if (address == payload) return i.getFunction() == body && base == ar_payload && type == payload->getValueType();
    if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(address)) {
      if (!l || i.getFunction() != body || gep->getPointerOperand() != array || gep->getSourceElementType() != array->getValueType() ||
          gep->getNumIndices() != 2 || type != llvm::cast<llvm::ArrayType>(array->getValueType())->getElementType()) return false;
      auto* first = llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(1));
      auto* variable = dyn_cast<ar::InternalVariable>(al->operand());
      auto* shift = variable ? dyn_cast_or_null<ar::PointerShift>(unique_def(variable)) : nullptr;
      if (!first || !first->isZero() || !shift || !shift->has_frontend() ||
          shift->frontend<llvm::Value>() != gep || shift->pointer() != ar_array ||
          !gep->getOperand(2)->getType()->isIntegerTy(word * 8)) return false;
      unsigned terms = 0;
      for (auto it = shift->term_begin(); it != shift->term_end(); ++it) {
        auto* zero = dyn_cast< ar::IntegerConstant >((*it).second);
        if (zero && zero->value().is_zero()) continue;
        if ((*it).first.to_z_number() != core::ZNumber(element_bytes) ||
            !source_integer_binding((*it).second, gep->getOperand(2))) return false;
        ++terms;
      }
      auto* zero = llvm::dyn_cast< llvm::ConstantInt >(gep->getOperand(2));
      return terms == 1 || (terms == 0 && zero && zero->isZero());
    }
    auto* local = llvm::dyn_cast<llvm::AllocaInst>(address);
    auto* n = local ? llvm::dyn_cast<llvm::ConstantInt>(local->getArraySize()) : nullptr;
    return i.getFunction() == main && local && n && n->isOne() && !local->getAddressSpace() &&
        (type->isIntegerTy(32) || (outputs.count(local) && type->isIntegerTy(word * 8))) && local->getAllocatedType() == type && base && base->has_frontend() &&
        base->frontend<llvm::Value>() == local;
  }
  bool inventory() {
    auto& module = *main->getParent();
    if (!module.getModuleInlineAsm().empty() || !mutex_effect_pointer_representations_closed(module) || !choose_objects()) return false;
    std::set<llvm::Value*> consumed;
    for (auto& f : module) for (auto& b : f) for (auto& i : b) if (auto* c = llvm::dyn_cast<llvm::CallBase>(&i)) {
      if (llvm::isa<llvm::DbgInfoIntrinsic>(c)) continue;
      auto* a = binding(c); auto* target = callee(c);
      if (!a || !target) return false;
      auto m = normal_mutex(ctx,a);
      if (m.source) {
        if (&f != body || (mutex && mutex != m.source)) return false;
        mutex = m.source; mutex_calls.emplace(c,m);
      } else if (create(c)) {
        outputs.insert(llvm::cast< llvm::AllocaInst >(c->getArgOperand(0)->stripPointerCasts()));
        auto* child = llvm::dyn_cast<llvm::Function>(c->getArgOperand(2)->stripPointerCasts());
        auto* entry = dyn_cast<ar::FunctionPointerConstant>(a->argument(2));
        if (!entry || !entry->function()->has_frontend() || entry->function()->frontend<llvm::Value>() != child ||
            !functions.count(child) || child == body) return false;
        if (std::find(roots.begin(),roots.end(),child) == roots.end()) roots.push_back(child);
      } else if (target->isDeclaration() && !terminal(c) && !nondet(c)) return false;
      consumed.insert(c);
    }
    if (!mutex || roots.empty()) return false;
    for (auto& f : module) {
      if (f.isDeclaration()) continue;
      if (!functions.count(&f) || f.isVarArg() || f.getCallingConv() != llvm::CallingConv::C) return false;
      bool root = std::find(roots.begin(),roots.end(),&f) != roots.end();
      unsigned body_calls = 0;
      for (unsigned k=0;k<f.arg_size();++k)
        for (auto a : {llvm::Attribute::ByVal,llvm::Attribute::StructRet,llvm::Attribute::InAlloca,llvm::Attribute::Preallocated})
          if (f.hasParamAttribute(k,a)) return false;
      std::vector<llvm::Value*> uses{&f}; std::set<llvm::Value*> seen;
      while (!uses.empty()) {
        if (!spend()) return false;
        auto* v=uses.back();uses.pop_back();if (!seen.insert(v).second) continue;
        for (auto* u:v->users()) {
          if (llvm::isa<llvm::BitCastOperator>(u)) {uses.push_back(u);continue;}
          auto* c=llvm::dyn_cast<llvm::CallBase>(u);if (!c || !binding(c)) return false;
          if (c->getCalledOperand()==v && !root && &f!=main) {
            if (&f==body && std::find(roots.begin(),roots.end(),c->getFunction())==roots.end()) return false;
            continue;
          }
          if (!root || !create(c) || c->getArgOperand(2)!=v) return false;
        }
      }
      for (auto& b:f) for (auto& i:b) {
        if (!spend()) return false;
        if (llvm::isa<llvm::DbgInfoIntrinsic>(i)) continue;
        if (llvm::isa<llvm::LoadInst>(i) || llvm::isa<llvm::StoreInst>(i)) {
          if (!scalar_memory(i)) return false;
          consumed.insert(&i);
        } else if (auto* local=llvm::dyn_cast<llvm::AllocaInst>(&i)) {
          auto* n=llvm::dyn_cast<llvm::ConstantInt>(local->getArraySize());
          auto* a=dyn_cast_or_null<ar::Allocate>(original(local));
          auto* count=a?dyn_cast<ar::IntegerConstant>(a->array_size()):nullptr;
          if (&f!=main || !n || !n->isOne() || local->getAddressSpace() || !(local->getAllocatedType()->isIntegerTy(32) ||
                 (outputs.count(local) && local->getAllocatedType()->isIntegerTy(word * 8))) ||
              !a || !count || count->value().to_z_number()!=1 || !a->allocated_type()->is_integer() ||
              cast<ar::IntegerType>(a->allocated_type())->bit_width()!=local->getAllocatedType()->getIntegerBitWidth() || !a->result()->has_frontend() ||
              a->result()->frontend<llvm::Value>()!=local) return false;
          consumed.insert(local);
        } else if (auto* c=llvm::dyn_cast<llvm::CallBase>(&i)) {
          if (callee(c)==body) {
            if (!root || cyclic(c->getParent()) || ++body_calls>1) return false;
          }
          if (&f==main && !create(c) && !nondet(c) && !terminal(c)) return false;
        } else if (!llvm::isa<llvm::BinaryOperator>(i) && !llvm::isa<llvm::CastInst>(i) &&
                   !llvm::isa<llvm::CmpInst>(i) && !llvm::isa<llvm::PHINode>(i) &&
                   !llvm::isa<llvm::SelectInst>(i) && !llvm::isa<llvm::GetElementPtrInst>(i) &&
                   !llvm::isa<llvm::BranchInst>(i) && !llvm::isa<llvm::SwitchInst>(i) &&
                   !llvm::isa<llvm::ReturnInst>(i) && !llvm::isa<llvm::UnreachableInst>(i)) return false;
      }
      if (root && body_calls!=1) return false;
    }
    std::vector<llvm::Value*> uses{mutex};std::set<llvm::Value*> seen;
    while (!uses.empty()) {
      if (!spend()) return false;
      auto* v=uses.back();uses.pop_back();if (!seen.insert(v).second) continue;
      for (auto* u:v->users()) {
        if (llvm::isa<llvm::BitCastOperator>(u)) {uses.push_back(u);continue;}
        auto* c=llvm::dyn_cast<llvm::CallBase>(u);
        if (!c || !mutex_calls.count(c) || c->getArgOperand(0)!=v) return false;
      }
    }
    for (const auto& item:originals) if (!consumed.count(item.first)) return false;
    for (auto& b:*body) for (auto& i:b) {
      pcs.emplace(&i,instructions.size());instructions.push_back(&i);
      if (llvm::isa<llvm::LoadInst>(i)||llvm::isa<llvm::PHINode>(i)) slots.emplace(&i,slots.size());
    }
    return true;
  }
  z3::expr fresh(z3::context& z, llvm::Type* type) {
    return z.bv_const(("self_prefix_"+std::to_string(symbol_id++)).c_str(),type->getIntegerBitWidth());
  }
  void set(SymbolicFrame& f, llvm::Value* v, const z3::expr& value) {
    f.values.erase(v); f.values.emplace(v,value);
  }
  z3::expr symbolic_value(z3::context& z, SymbolicFrame& f, llvm::Value* v) {
    if (llvm::isa<llvm::UndefValue>(v) || llvm::isa<llvm::PoisonValue>(v)) throw UnsupportedPrefix{};
    auto known=f.values.find(v); if (known!=f.values.end()) return known->second;
    if (auto* n=llvm::dyn_cast<llvm::ConstantInt>(v)) return z.bv_val(n->getZExtValue(),n->getBitWidth());
    auto value=fresh(z,v->getType());set(f,v,value);return value;
  }
  z3::expr symbolic_operation(z3::context& z, SymbolicFrame& f, llvm::Instruction* i) {
    auto unknown=[&]() {return fresh(z,i->getType());};
    auto value=[&](unsigned n) {return symbolic_value(z,f,i->getOperand(n));};
    if (auto* cmp=llvm::dyn_cast<llvm::ICmpInst>(i)) {
      if (!cmp->getOperand(0)->getType()->isIntegerTy() || cmp->getOperand(0)->getType()->getIntegerBitWidth()>64) return unknown();
      auto a=value(0),b=value(1);auto result=z.bool_val(true);
      switch (cmp->getPredicate()) {
        case llvm::CmpInst::ICMP_EQ:result=a==b;break;
        case llvm::CmpInst::ICMP_NE:result=a!=b;break;
        case llvm::CmpInst::ICMP_SLT:result=a<b;break;
        case llvm::CmpInst::ICMP_SLE:result=a<=b;break;
        case llvm::CmpInst::ICMP_SGT:result=a>b;break;
        case llvm::CmpInst::ICMP_SGE:result=a>=b;break;
        case llvm::CmpInst::ICMP_ULT:result=z3::ult(a,b);break;
        case llvm::CmpInst::ICMP_ULE:result=z3::ule(a,b);break;
        case llvm::CmpInst::ICMP_UGT:result=z3::ugt(a,b);break;
        case llvm::CmpInst::ICMP_UGE:result=z3::uge(a,b);break;
        default:return unknown();
      }
      return z3::ite(result,z.bv_val(1,1),z.bv_val(0,1));
    }
    if (auto* cast=llvm::dyn_cast<llvm::CastInst>(i)) {
      auto* from=cast->getOperand(0)->getType();
      if (!from->isIntegerTy() || from->getIntegerBitWidth()>64) return unknown();
      auto a=value(0);unsigned before=from->getIntegerBitWidth(),after=cast->getType()->getIntegerBitWidth();
      if (cast->getOpcode()==llvm::Instruction::ZExt) return z3::zext(a,after-before);
      if (cast->getOpcode()==llvm::Instruction::SExt) return z3::sext(a,after-before);
      if (cast->getOpcode()==llvm::Instruction::Trunc) return a.extract(after-1,0);
      if (cast->getOpcode()==llvm::Instruction::BitCast && before==after) return a;
    }
    if (auto* op=llvm::dyn_cast<llvm::BinaryOperator>(i)) {
      auto a=value(0),b=value(1);
      switch (op->getOpcode()) {
        case llvm::Instruction::And:return a&b;
        case llvm::Instruction::Or:return a|b;
        case llvm::Instruction::Xor:return a^b;
        case llvm::Instruction::SRem: {
          auto* n=llvm::dyn_cast<llvm::ConstantInt>(op->getOperand(1));
          if (n && n->getSExtValue()>0) return z3::srem(a,b);
          break;
        }
        // Dropping overflow/definedness constraints enlarges a value to TOP;
        // it must not select the wrapped answer for an nsw/nuw operation.
        case llvm::Instruction::Add:if (!op->hasNoSignedWrap()&&!op->hasNoUnsignedWrap()) return a+b;break;
        case llvm::Instruction::Sub:if (!op->hasNoSignedWrap()&&!op->hasNoUnsignedWrap()) return a-b;break;
        case llvm::Instruction::Mul:if (!op->hasNoSignedWrap()&&!op->hasNoUnsignedWrap()) return a*b;break;
        default:break;
      }
    }
    if (llvm::isa<llvm::SelectInst>(i)) return z3::ite(value(0)!=z.bv_val(0,1),value(1),value(2));
    return unknown();
  }
  void symbolic_enter(z3::context& z, SymbolicFrame& f, llvm::BasicBlock* from, llvm::BasicBlock* to) {
    std::vector<std::pair<llvm::PHINode*,z3::expr>> updates;
    for (auto& i:*to) {
      auto* phi=llvm::dyn_cast<llvm::PHINode>(&i);if (!phi) break;
      if (phi->getType()->isIntegerTy() && phi->getType()->getIntegerBitWidth()<=64)
        updates.emplace_back(phi,symbolic_value(z,f,phi->getIncomingValueForBlock(from)));
    }
    for (const auto& p:updates) set(f,p.first,p.second);
    f.pc=to->getFirstNonPHI();
  }
  bool enumerate(z3::context& z, SymbolicState& state, llvm::CallBase* call) {
    z3::solver solver(z);z3::params params(z);params.set("timeout",100u);solver.set(params);
    solver.add(state.condition);std::vector<z3::expr> actual;
    for (unsigned i=0;i<call->arg_size();++i) actual.push_back(symbolic_value(z,state.stack.back(),call->getArgOperand(i)));
    for (unsigned count=0;;++count) {
      if (!solver_budget) return false;
      --solver_budget;auto result=solver.check();
      if (result==z3::unsat) return true;
      if (result!=z3::sat || count==64) return false;
      auto model=solver.get_model();std::vector<uint64_t> tuple;auto block=z.bool_val(false);
      for (const auto& a:actual) {
        uint64_t n=0;auto v=model.eval(a,true);
        if (!v.is_numeral_u64(n)) return false;
        tuple.push_back(n);block=block||(a!=v);
      }
      arguments.insert(std::move(tuple));if (arguments.size()>64) return false;
      // OR blocks this entire tuple, not a Cartesian slice of the inputs.
      solver.add(block);
    }
  }
  bool prefix(llvm::Function* root) {
    z3::context z;
    SymbolicState start{{SymbolicFrame{&root->getEntryBlock().front(),{}, {}}},z.bool_val(true)};
    std::deque<SymbolicState> work{start};
    while (!work.empty()) {
      auto state=std::move(work.front());work.pop_front();auto& f=state.stack.back();auto* i=f.pc;
      if (!i || !spend() || !f.visited.insert(i).second) return false;
      if (auto* branch=llvm::dyn_cast<llvm::BranchInst>(i)) {
        auto condition=branch->isConditional()?symbolic_value(z,f,branch->getCondition())!=z.bv_val(0,1):z.bool_val(true);
        for (unsigned k=0;k<branch->getNumSuccessors();++k) {
          auto next=state;if (branch->isConditional()) next.condition=next.condition&&(k? !condition:condition);
          symbolic_enter(z,next.stack.back(),i->getParent(),branch->getSuccessor(k));work.push_back(std::move(next));
        }
        continue;
      }
      if (auto* call=llvm::dyn_cast<llvm::CallBase>(i)) {
        auto* target=callee(call);
        if (target==body) {if (!enumerate(z,state,call)) return false;continue;}
        if (terminal(call)) continue;
        if (nondet(call)) {set(f,call,fresh(z,call->getType()));f.pc=i->getNextNode();work.push_back(std::move(state));continue;}
        if (!target || target->isDeclaration() || state.stack.size()==32) return false;
        for (const auto& frame:state.stack) if (frame.pc->getFunction()==target) return false;
        SymbolicFrame child{&target->getEntryBlock().front(),{}, {}};
        for (unsigned k=0;k<call->arg_size();++k) {
          auto* type=call->getArgOperand(k)->getType();
          if (type->isIntegerTy() && type->getIntegerBitWidth()<=64) set(child,target->getArg(k),symbolic_value(z,f,call->getArgOperand(k)));
        }
        state.stack.push_back(std::move(child));work.push_back(std::move(state));continue;
      }
      if (auto* ret=llvm::dyn_cast<llvm::ReturnInst>(i)) {
        if (state.stack.size()==1) continue;
        auto* value=ret->getReturnValue();
        if (value && (!value->getType()->isIntegerTy() || value->getType()->getIntegerBitWidth()>64)) return false;
        auto result=value?symbolic_value(z,f,value):z.bv_val(0,1);
        state.stack.pop_back();auto& caller=state.stack.back();
        if (value) set(caller,caller.pc,result);
        caller.pc=caller.pc->getNextNode();work.push_back(std::move(state));continue;
      }
      if (llvm::isa<llvm::UnreachableInst>(i) || llvm::isa<llvm::SwitchInst>(i)) return false;
      if (i->getType()->isIntegerTy() && i->getType()->getIntegerBitWidth()<=64) set(f,i,symbolic_operation(z,f,i));
      f.pc=i->getNextNode();work.push_back(std::move(state));
    }
    return true;
  }
  Value evaluate(llvm::Value* v, const State& s, const std::vector<uint64_t>& args, unsigned depth = 0) const {
    if (depth == 32) return {};
    if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(v))
      return c->getBitWidth() <= 64 ? Value{true, c->getZExtValue()} : Value{};
    if (auto* argument = llvm::dyn_cast<llvm::Argument>(v))
      return argument->getParent() == body ? Value{true,args.at(argument->getArgNo())} : Value{};
    auto* i = llvm::dyn_cast<llvm::Instruction>(v);
    if (!i) return {};
    auto saved = slots.find(i);
    if (saved != slots.end()) return s.values[saved->second];
    auto operand = [&](unsigned n) { return evaluate(i->getOperand(n), s, args, depth + 1); };
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
      auto a=operand(0),b=operand(1);auto* type=op->getType();
      if (!a.known || !b.known || !type->isIntegerTy() || type->getIntegerBitWidth()>64) return {};
      llvm::APInt x(type->getIntegerBitWidth(),a.bits),y(type->getIntegerBitWidth(),b.bits),n=x;
      bool signed_overflow=false,unsigned_overflow=false;
      switch (op->getOpcode()) {
        case llvm::Instruction::Add:n=x.sadd_ov(y,signed_overflow);n=x.uadd_ov(y,unsigned_overflow);break;
        case llvm::Instruction::Sub:n=x.ssub_ov(y,signed_overflow);n=x.usub_ov(y,unsigned_overflow);break;
        case llvm::Instruction::Mul:n=x.smul_ov(y,signed_overflow);n=x.umul_ov(y,unsigned_overflow);break;
        case llvm::Instruction::And:n=x&y;break;
        case llvm::Instruction::Or:n=x|y;break;
        case llvm::Instruction::Xor:n=x^y;break;
        case llvm::Instruction::SRem:
          if (y.isZero() || (x.isMinSignedValue() && y.isAllOnes())) return {};
          n=x.srem(y);break;
        default:return {};
      }
      if ((op->hasNoSignedWrap()&&signed_overflow)||(op->hasNoUnsignedWrap()&&unsigned_overflow)) return {};
      return {true,n.getZExtValue()};
    }
    return {};
  }
  void enter(State& s, llvm::BasicBlock* from, llvm::BasicBlock* to, const std::vector<uint64_t>& args) const {
    std::vector<std::pair<unsigned,Value>> updates;
    for (auto& i:*to) {
      auto* phi=llvm::dyn_cast<llvm::PHINode>(&i);if (!phi) break;
      updates.emplace_back(slots.at(phi),evaluate(phi->getIncomingValueForBlock(from),s,args));
    }
    for (const auto& item:updates) s.values[item.first]=item.second;
    auto* first=to->getFirstNonPHI();s.pc=first?pcs.at(first):0;
  }
  bool phase(const std::vector<uint64_t>& args) {
    State start;start.pc=pcs.at(&body->getEntryBlock().front());start.values.resize(slots.size());
    std::set<State> seen{start};std::deque<State> work{start};
    while (!work.empty()) {
      auto s=std::move(work.front());work.pop_front();
      if (!spend() || seen.size()>65536 || !s.pc) return false;
      auto* i=instructions[s.pc];std::vector<State> next;
      if (auto* branch=llvm::dyn_cast<llvm::BranchInst>(i)) {
        auto c=branch->isConditional()?evaluate(branch->getCondition(),s,args):Value{};
        for (unsigned k=0;k<branch->getNumSuccessors();++k) {
          if (branch->isConditional()&&c.known&&k!=(c.bits?0u:1u)) continue;
          auto n=s;enter(n,i->getParent(),branch->getSuccessor(k),args);next.push_back(std::move(n));
        }
      } else if (auto* sw=llvm::dyn_cast<llvm::SwitchInst>(i)) {
        auto v=evaluate(sw->getCondition(),s,args);auto* selected=sw->getDefaultDest();
        if (v.known) for (auto c:sw->cases()) if (c.getCaseValue()->getValue().getLimitedValue()==v.bits) selected=c.getCaseSuccessor();
        for (unsigned k=0;k<sw->getNumSuccessors();++k) {
          if (v.known&&sw->getSuccessor(k)!=selected) continue;
          auto n=s;enter(n,i->getParent(),sw->getSuccessor(k),args);next.push_back(std::move(n));
        }
      } else if (llvm::isa<llvm::ReturnInst>(i)) {
        if (s.held) return false;
        continue;
      } else if (llvm::isa<llvm::UnreachableInst>(i)) return false;
      else if (auto* c=llvm::dyn_cast<llvm::CallBase>(i)) {
        if (terminal(c)) continue;
        auto m=mutex_calls.find(c);
        if (m!=mutex_calls.end()) {
          if (m->second.lock) {
            if (s.held) return false;
            for (bool final:{false,true}) {
              if (s.post&&!final) continue;
              auto n=s;n.held=true;n.final=final;n.entry_final=final;n.stores=0;
              n.pc=pcs.at(i->getNextNode());next.push_back(std::move(n));
            }
          } else {
            if (!s.held || !s.final || s.stores!=(s.entry_final?0u:1u)) return false;
            s.held=false;s.post=true;s.stores=0;s.entry_final=false;
            s.pc=pcs.at(i->getNextNode());next.push_back(std::move(s));
          }
        } else {
          s.pc=pcs.at(i->getNextNode());next.push_back(std::move(s));
        }
      } else {
        if (auto* load=llvm::dyn_cast<llvm::LoadInst>(i)) {
          Value value;auto* address=load->getPointerOperand()->stripPointerCasts();
          if (address==payload) {
            auto inserted=post.emplace(i,s.post);if (!inserted.second) inserted.first->second&=s.post;
            if (s.held) value={true,s.final?0:initial_bits};
          } else {
            auto* gep=llvm::dyn_cast<llvm::GetElementPtrInst>(address);
            if (!gep) return false;
            auto index=evaluate(gep->getOperand(2),s,args);auto* type=gep->getOperand(2)->getType();
            if (!index.known || !type->isIntegerTy() || type->getIntegerBitWidth()>64) return false;
            llvm::APInt bits(type->getIntegerBitWidth(),index.bits);
            if (bits.isNegative() || bits.getZExtValue()>=elements || bits.getZExtValue()*element_bytes+element_bytes>elements*element_bytes) return false;
            value={true,0};
          }
          s.values[slots.at(i)]=value;
        } else if (auto* store=llvm::dyn_cast<llvm::StoreInst>(i)) {
          auto value=evaluate(store->getValueOperand(),s,args);
          // A same-value second Store is still a conflicting memory event.
          if (store!=initialization || !s.held || s.final || s.stores || !value.known || value.bits!=0) return false;
          s.final=true;s.stores=1;saw_write=true;
        }
        auto* following=i->getNextNode();if (!following) return false;
        s.pc=pcs.at(following);next.push_back(std::move(s));
      }
      for (auto& n:next) if (seen.insert(n).second) work.push_back(std::move(n));
    }
    return true;
  }
  llvm::Instruction* endpoint(const AccessRecord& r) const {
    if (!r.stmt->has_frontend() || !r.pts.is_set() || r.pts.size()!=1 || !r.width) return nullptr;
    auto* i=llvm::dyn_cast<llvm::Instruction>(r.stmt->frontend<llvm::Value>());
    auto* location=dyn_cast<GlobalMemoryLocation>(*r.pts.begin());
    if (!i || original(i)!=r.stmt || i->getFunction()!=body || !location || location->global_var()!=ar_payload ||
        r.width!=payload->getValueType()->getIntegerBitWidth()/8) return nullptr;
    if ((llvm::isa<llvm::LoadInst>(i)&&r.kind==AccessKind::Read)||(i==initialization&&r.kind==AccessKind::Write)) return i;
    return nullptr;
  }
public:
  explicit Impl(Context& context):ctx(context) {
    if (!ctx.opts.enable_thread_modular || ctx.opts.procedural==Procedural::Intraprocedural ||
        ctx.opts.entry_points.size()!=1 || ctx.opts.entry_points.front()->name()!="main" ||
        ctx.opts.entry_points.front()->num_parameters()!=0 || !ctx.opts.entry_points.front()->has_frontend() ||
        !ctx.opts.no_init_globals.empty() ||
        ctx.concurrent_env.is_dirty() || !ctx.concurrent_env.unmodeled_effects().empty() ||
        !ctx.concurrent_env.unmodeled_thread_creations().empty() ||
        ctx.bundle->global_or_null("ar.global_ctors") || ctx.bundle->global_or_null("ar.global_dtors")) return;
    main=ctx.opts.entry_points.front()->frontend<llvm::Function>();
    const auto abi = pthread_object_layout(ctx.bundle, main->getParent());
    if (!abi || !main->use_empty() ||
        main->getParent()->getDataLayout().getIndexSizeInBits(0) != abi.word * 8) return;
    word = abi.word;
    for (auto it=ctx.bundle->function_begin();it!=ctx.bundle->function_end();++it) {
      auto* f=*it;if (!f->is_definition()||!f->has_frontend()) continue;
      auto* source=f->frontend<llvm::Function>();functions.emplace(source,f);
      for (auto* b:*f->body()) for (auto* s:*b) {
        if (isa<ar::CallBase>(s)||isa<ar::Load>(s)||isa<ar::Store>(s)||isa<ar::Allocate>(s)) {
          auto* i=s->has_frontend()?llvm::dyn_cast<llvm::Instruction>(s->frontend<llvm::Value>()):nullptr;
          if (!i||i->getFunction()!=source||!originals.emplace(i,s).second) return;
        } else if (!isa<ar::Assignment>(s)&&!isa<ar::UnaryOperation>(s)&&!isa<ar::BinaryOperation>(s)&&
                   !isa<ar::Comparison>(s)&&!isa<ar::PointerShift>(s)&&!isa<ar::ReturnValue>(s)&&!isa<ar::Unreachable>(s)) return;
      }
    }
    if (!inventory()) {log::debug("Self publication inventory refused");return;}
    try {
      for (auto* root:roots) if (!prefix(root)) {log::debug("Self publication prefix refused");return;}
      if (arguments.empty()) return;
      for (const auto& args:arguments) if (!phase(args)) {log::debug("Self publication phase refused");return;}
      valid=saw_write;
    } catch (const z3::exception&) {log::debug("Self publication solver refused");}
      catch (const UnsupportedPrefix&) {log::debug("Self publication indeterminate prefix refused");}
  }
  bool ordered(const AccessRecord& a,const AccessRecord& b) const {
    if (!valid) return false;
    auto* ia=endpoint(a);auto* ib=endpoint(b);
    if (!ia||!ib) return false;
    auto published=[&](llvm::Instruction* i){auto p=post.find(i);return p!=post.end()&&p->second;};
    return (ia==initialization&&published(ib))||(ib==initialization&&published(ia));
  }
  JsonDict info(const AccessRecord&,const AccessRecord&) const {
    return {{"safe_reason","unique protected state write happens before every certified post-gate read"},
            {"self_publication_contract","closed-single-write-self-state-postgate-v1"},
            {"payload",payload?payload->getName().str():""},
            {"argument_tuples",static_cast<uint64_t>(arguments.size())}};
  }
};
SelfPublication::SelfPublication(Context& ctx):_impl(std::make_unique<Impl>(ctx)) {}
SelfPublication::~SelfPublication()=default;
bool SelfPublication::ordered(const AccessRecord& a,const AccessRecord& b) const {return _impl->ordered(a,b);}
JsonDict SelfPublication::info(const AccessRecord& a,const AccessRecord& b) const {return _impl->info(a,b);}
} // namespace analyzer
} // namespace ikos
