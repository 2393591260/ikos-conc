// Copyright and license: see data_race.hpp.
#include <ikos/analyzer/checker/mutex_handover.hpp>

#include <llvm/IR/CFG.h>
#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <vector>
#include <ikos/analyzer/checker/data_race.hpp>
#include <ikos/analyzer/checker/normal_mutex.hpp>
#include <ikos/analyzer/util/log.hpp>

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
struct Proof {
  llvm::GlobalVariable* outer = nullptr;
  llvm::GlobalVariable* inner = nullptr;
  // Role 1: held D after an A acquisition in this D interval.
  // Role 2: held A since its acquisition under D and subsequent D release.
  std::map< llvm::Instruction*, unsigned > roles;
};
} // namespace

class MutexHandover::Impl {
  Context& ctx;
  std::map< llvm::Value*, ar::Statement* > originals;
  std::map< llvm::Function*, ar::Function* > functions;
  std::map< llvm::CallBase*, NormalMutex > mutex_calls;
  std::vector< llvm::Function* > roots;
  std::vector< Proof > proofs;
  unsigned budget = 131072;
  bool spend() { if (!budget) return false; --budget; return true; }
  ar::Statement* original(llvm::Value* v) const {
    auto it = originals.find(v); return it == originals.end() ? nullptr : it->second;
  }
  ar::CallBase* binding(llvm::CallBase* c) const {
    auto* a = dyn_cast_or_null< ar::CallBase >(original(c));
    auto* target = a ? native_handle_callee(a) : nullptr;
    auto* f = callee(c);
    if (!f || !target || !isa< ar::Call >(a) || !plain_call(c, f) || !target->has_frontend() ||
        target->frontend< llvm::Value >() != f || a->num_arguments() != c->arg_size() ||
        target->num_parameters() != a->num_arguments() || target->type()->is_var_arg() ||
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
  bool create(llvm::CallBase* c) const {
    auto* a = binding(c);
    if (!a || !native_handle_api(c, true) ||
        (!ctx.native_handle_effect_certificates().count(a) && !unread_private_thread_output(a)) ||
        !isa< ar::NullConstant >(a->argument(1)) || !isa< ar::NullConstant >(a->argument(3))) return false;
    auto* entry = dyn_cast< ar::FunctionPointerConstant >(a->argument(2));
    auto* slot = address_base(a->argument(0));
    auto* child = entry ? entry->function() : nullptr;
    auto* native_child = llvm::dyn_cast< llvm::Function >(c->getArgOperand(2)->stripPointerCasts());
    auto* native_slot = c->getArgOperand(0)->stripPointerCasts();
    auto* local = llvm::dyn_cast< llvm::AllocaInst >(native_slot);
    auto* count = local ? llvm::dyn_cast< llvm::ConstantInt >(local->getArraySize()) : nullptr;
    return slot && isa< ar::LocalVariable >(slot) && count && count->isOne() &&
        local->getAllocatedType()->isIntegerTy(ctx.bundle->data_layout().pointers.bit_width) &&
        slot->has_frontend() && slot->frontend< llvm::Value >() == native_slot &&
        child && child->is_definition() && child->has_frontend() &&
        child->frontend< llvm::Value >() == native_child;
  }
  bool terminal(llvm::CallBase* c) const {
    auto* a = binding(c); auto* f = a ? native_handle_callee(a) : nullptr;
    auto* source = callee(c);
    return f && source->isDeclaration() &&
        ((f->intrinsic_id() == ar::Intrinsic::LibcAbort && source->getName() == "abort" &&
          c->arg_empty() && c->getType()->isVoidTy()) || constant_assertion_effect(a));
  }
  bool pure_external(llvm::CallBase* c) const {
    auto* a = binding(c); auto* f = a ? native_handle_callee(a) : nullptr;
    auto* source = callee(c);
    if (!f || !source->isDeclaration() || !c->arg_empty()) return false;
    if (source->getName() == "__VERIFIER_nondet_int" && c->getType()->isIntegerTy(32) &&
        f->type()->return_type()->is_integer()) return true;
    return f->intrinsic_id() == ar::Intrinsic::IkosNonDet && c->getMetadata("ikos.freeze_uninit") &&
        ((source->getName() == "__ikos_nondet_int" && c->getType()->isIntegerTy(32)) ||
         (source->getName() == "__ikos_nondet_int64" && c->getType()->isIntegerTy(64)));
  }
  bool scalar_memory(llvm::Instruction& instruction) const {
    auto* l = llvm::dyn_cast< llvm::LoadInst >(&instruction);
    auto* s = llvm::dyn_cast< llvm::StoreInst >(&instruction);
    auto* address = (l ? l->getPointerOperand() : s->getPointerOperand())->stripPointerCasts();
    auto* type = l ? l->getType() : s->getValueOperand()->getType();
    if ((!type->isIntegerTy() && !type->isFloatingPointTy()) ||
        (l && l->isAtomic()) || (s && s->isAtomic())) return false;
    auto* a = original(&instruction);
    auto* al = dyn_cast_or_null< ar::Load >(a); auto* as = dyn_cast_or_null< ar::Store >(a);
    auto* at = al ? al->result()->type() : as ? as->value()->type() : nullptr;
    auto* base = al ? address_base(al->operand()) : as ? address_base(as->pointer()) : nullptr;
    if (!at || (l && !al) || (s && !as) ||
        !base || !base->has_frontend() || base->frontend< llvm::Value >() != address ||
        cast< ar::PointerType >(base->type())->pointee() != at ||
        (type->isIntegerTy() != at->is_integer()) ||
        (type->isFloatingPointTy() && !at->is_float()) ||
        ctx.bundle->data_layout().store_size_in_bytes(at) !=
            instruction.getModule()->getDataLayout().getTypeStoreSize(type).getFixedValue()) return false;
    if (type->isIntegerTy() && cast< ar::IntegerType >(at)->bit_width() != type->getIntegerBitWidth()) return false;
    if (type->isFloatingPointTy() && cast< ar::FloatType >(at)->bit_width() != type->getPrimitiveSizeInBits()) return false;
    // Whole scalar objects only: no out-of-object effect can change a mutex.
    if (auto* global = llvm::dyn_cast< llvm::GlobalVariable >(address))
      return global->getValueType() == type && global->getAddressSpace() == 0 &&
             global->hasInitializer() && !global->isExternallyInitialized();
    auto* local = llvm::dyn_cast< llvm::AllocaInst >(address);
    auto* count = local ? llvm::dyn_cast< llvm::ConstantInt >(local->getArraySize()) : nullptr;
    return local && count && count->isOne() && local->getAllocatedType() == type &&
           local->getAddressSpace() == 0;
  }
  bool close_mutex(llvm::GlobalVariable* global) {
    std::vector< llvm::Value* > todo{global}; std::set< llvm::Value* > seen;
    while (!todo.empty()) {
      auto* v = todo.back(); todo.pop_back();
      if (!spend()) return false;
      if (!seen.insert(v).second) continue;
      for (auto* u : v->users()) {
        if (llvm::isa< llvm::BitCastOperator >(u)) { todo.push_back(u); continue; }
        if (auto* gep = llvm::dyn_cast< llvm::GEPOperator >(u)) {
          if (gep->getPointerOperand() != v || !gep->hasAllZeroIndices()) return false;
          todo.push_back(u); continue;
        }
        auto* call = llvm::dyn_cast< llvm::CallBase >(u);
        auto found = call ? mutex_calls.find(call) : mutex_calls.end();
        if (!call || found == mutex_calls.end() || found->second.source != global ||
            call->getArgOperand(0) != v) return false;
      }
    }
    return true;
  }
  bool inventory(llvm::Module& module, llvm::Function* main) {
    if (!module.getModuleInlineAsm().empty() || !mutex_effect_pointer_representations_closed(module)) return false;
    std::set< llvm::Value* > consumed;
    for (auto& f : module) {
      if (f.isDeclaration()) continue;
      if (!functions.count(&f) || f.isVarArg() || f.getCallingConv() != llvm::CallingConv::C) return false;
      for (unsigned i = 0; i < f.arg_size(); ++i)
        for (auto a : {llvm::Attribute::ByVal, llvm::Attribute::StructRet,
                       llvm::Attribute::InAlloca, llvm::Attribute::Preallocated})
          if (f.hasParamAttribute(i, a)) return false;
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
        if (llvm::isa< llvm::DbgInfoIntrinsic >(i)) continue;
        if (llvm::isa< llvm::LoadInst >(i) || llvm::isa< llvm::StoreInst >(i)) {
          if (!scalar_memory(i)) { log::debug("Mutex handover unsupported scalar memory in " + f.getName().str()); return false; }
          consumed.insert(&i);
        } else if (auto* c = llvm::dyn_cast< llvm::CallBase >(&i)) {
          auto* a = binding(c); auto* target = callee(c);
          if (!a || !target) { log::debug("Mutex handover unsupported binding: " + (target ? target->getName().str() : "indirect")); return false; }
          consumed.insert(c);
          if (!target->isDeclaration()) continue;
          auto m = normal_mutex(ctx, a);
          if (m.source) { mutex_calls.emplace(c, m); continue; }
          if (create(c)) {
            auto* child = llvm::dyn_cast< llvm::Function >(c->getArgOperand(2)->stripPointerCasts());
            if (&f != main || !child || !functions.count(child)) return false;
            if (std::find(roots.begin(), roots.end(), child) == roots.end()) roots.push_back(child);
          } else if (!terminal(c) && !pure_external(c)) { log::debug("Mutex handover unsupported call: " + target->getName().str()); return false; }
        } else if (auto* local = llvm::dyn_cast< llvm::AllocaInst >(&i)) {
          auto* count = llvm::dyn_cast< llvm::ConstantInt >(local->getArraySize());
          auto* allocation = dyn_cast_or_null< ar::Allocate >(original(local));
          auto* size = allocation ? dyn_cast< ar::IntegerConstant >(allocation->array_size()) : nullptr;
          if (!count || !count->isOne() || !local->getAllocatedType()->isIntegerTy() ||
              !allocation || !size || size->value().to_z_number() != 1 ||
              !allocation->result()->has_frontend() || allocation->result()->frontend< llvm::Value >() != local ||
              !allocation->allocated_type()->is_integer() ||
              cast< ar::IntegerType >(allocation->allocated_type())->bit_width() != local->getAllocatedType()->getIntegerBitWidth()) return false;
          consumed.insert(local);
        } else if (!llvm::isa< llvm::BinaryOperator >(i) && !llvm::isa< llvm::CastInst >(i) &&
                   !llvm::isa< llvm::CmpInst >(i) && !llvm::isa< llvm::PHINode >(i) &&
                   !llvm::isa< llvm::SelectInst >(i) && !llvm::isa< llvm::GetElementPtrInst >(i) &&
                   !llvm::isa< llvm::BranchInst >(i) && !llvm::isa< llvm::SwitchInst >(i) &&
                   !llvm::isa< llvm::ReturnInst >(i) && !llvm::isa< llvm::UnreachableInst >(i)) return false;
      }
    }
    for (const auto& item : originals) if (!item.second || !consumed.count(item.first)) return false;
    return true;
  }
  bool phases(Proof& proof) {
    enum : unsigned { D = 1, A = 2, Seen = 4, Crossed = 8 };
    std::map< std::pair< llvm::Function*, unsigned >, unsigned > summaries;
    std::set< llvm::Function* > active;
    bool valid = true;
    std::function< unsigned(llvm::Function*, unsigned) > run = [&](llvm::Function* f, unsigned entry) {
      auto key = std::make_pair(f, entry); auto known = summaries.find(key);
      if (known != summaries.end()) return known->second;
      if (active.size() == 64 || !active.insert(f).second) { valid = false; return 0u; }
      std::map< llvm::BasicBlock*, unsigned > inputs;
      std::deque< llvm::BasicBlock* > work{&f->getEntryBlock()};
      inputs[&f->getEntryBlock()] = 1u << entry; unsigned exits = 0;
      while (valid && !work.empty()) {
        auto* block = work.front(); work.pop_front(); unsigned states = inputs.at(block);
        for (auto& i : *block) {
          if (!spend()) { valid = false; break; }
          if (!states) break;
          if (llvm::isa< llvm::LoadInst >(i) || llvm::isa< llvm::StoreInst >(i)) {
            unsigned roles = 3;
            for (unsigned s = 0; s < 16; ++s) if (states & (1u << s)) {
              if (!(s & D) || !(s & Seen)) roles &= ~1u;
              if (!(s & A) || !(s & Crossed)) roles &= ~2u;
            }
            auto inserted = proof.roles.emplace(&i, roles);
            if (!inserted.second) inserted.first->second &= roles;
          }
          if (auto* c = llvm::dyn_cast< llvm::CallBase >(&i)) {
            if (llvm::isa< llvm::DbgInfoIntrinsic >(c)) continue;
            auto m = mutex_calls.find(c);
            if (m != mutex_calls.end()) {
              const bool outer = m->second.source == proof.outer;
              if (!outer && m->second.source != proof.inner) { valid = false; break; }
              unsigned next = 0;
              for (unsigned before = 0; before < 16; ++before) if (states & (1u << before)) {
                unsigned s = before;
                unsigned held = outer ? D : A;
                if (m->second.lock) {
                  if (s & held) { valid = false; break; }
                  s |= held;
                  if (outer) s &= ~Seen;
                  else { s &= ~Crossed; if (s & D) s |= Seen; }
                } else {
                  if (!(s & held)) { valid = false; break; }
                  if (outer) { if ((s & A) && (s & Seen)) s |= Crossed; s &= ~(D | Seen); }
                  else s &= ~(A | Crossed);
                }
                next |= 1u << s;
              }
              states = next;
            } else if (terminal(c)) states = 0;
            else if (auto* child = callee(c)) {
              if (!child->isDeclaration()) {
                unsigned next = 0;
                for (unsigned s = 0; valid && s < 16; ++s)
                  if (states & (1u << s)) next |= run(child, s);
                states = next;
              }
            }
          }
          if (llvm::isa< llvm::UnreachableInst >(i) && states) { valid = false; break; }
          if (llvm::isa< llvm::ReturnInst >(i)) exits |= states;
        }
        for (auto* next : llvm::successors(block)) {
          auto before = inputs[next]; inputs[next] |= states;
          if (before != inputs[next]) work.push_back(next);
        }
      }
      active.erase(f); summaries.emplace(key, exits); return exits;
    };
    for (auto* root : roots) { run(root, 0); if (!valid) return false; }
    return valid;
  }
  llvm::Instruction* endpoint(const AccessRecord& r) const {
    if (!r.stmt->has_frontend() || !r.width) return nullptr;
    auto* i = llvm::dyn_cast< llvm::Instruction >(r.stmt->frontend< llvm::Value >());
    if (!i || original(i) != r.stmt) return nullptr;
    auto* l = llvm::dyn_cast< llvm::LoadInst >(i); auto* s = llvm::dyn_cast< llvm::StoreInst >(i);
    if ((l && r.kind == AccessKind::Read && isa< ar::Load >(r.stmt)) ||
        (s && r.kind == AccessKind::Write && isa< ar::Store >(r.stmt))) return i;
    return nullptr;
  }
  const Proof* pair(const AccessRecord& a, const AccessRecord& b) const {
    auto* ia = endpoint(a); auto* ib = endpoint(b);
    if (!ia || !ib) return nullptr;
    for (const auto& p : proofs) {
      auto x = p.roles.find(ia), y = p.roles.find(ib);
      if (x != p.roles.end() && y != p.roles.end() &&
          (((x->second & 1) && (y->second & 2)) || ((x->second & 2) && (y->second & 1)))) return &p;
    }
    return nullptr;
  }
public:
  explicit Impl(Context& context) : ctx(context) {
    if (!ctx.opts.enable_thread_modular || ctx.opts.procedural == Procedural::Intraprocedural ||
        ctx.opts.entry_points.size() != 1 || ctx.opts.entry_points.front()->name() != "main" ||
        ctx.opts.entry_points.front()->num_parameters() != 0 || !ctx.opts.entry_points.front()->has_frontend() ||
        !ctx.opts.no_init_globals.empty() ||
        ctx.concurrent_env.is_dirty() || !ctx.concurrent_env.unmodeled_effects().empty() ||
        !ctx.concurrent_env.unmodeled_thread_creations().empty() ||
        ctx.bundle->global_or_null("ar.global_ctors") || ctx.bundle->global_or_null("ar.global_dtors")) return;
    auto* main = ctx.opts.entry_points.front()->frontend< llvm::Function >();
    if (!main->use_empty() || !pthread_object_layout(ctx.bundle, main->getParent())) return;
    for (auto it = ctx.bundle->function_begin(); it != ctx.bundle->function_end(); ++it) {
      auto* f = *it; if (!f->is_definition() || !f->has_frontend()) continue;
      auto* source = f->frontend< llvm::Function >(); functions.emplace(source, f);
      for (auto* b : *f->body()) for (auto* s : *b) {
        if (isa< ar::CallBase >(s) || isa< ar::Load >(s) || isa< ar::Store >(s) || isa< ar::Allocate >(s)) {
          auto* i = s->has_frontend() ? llvm::dyn_cast< llvm::Instruction >(s->frontend< llvm::Value >()) : nullptr;
          if (!i || i->getFunction() != source) return;
          auto inserted = originals.emplace(i, s);
          if (!inserted.second) return;
        } else if (!isa< ar::Assignment >(s) && !isa< ar::UnaryOperation >(s) &&
                   !isa< ar::BinaryOperation >(s) && !isa< ar::Comparison >(s) &&
                   !isa< ar::PointerShift >(s) && !isa< ar::ReturnValue >(s) &&
                   !isa< ar::Unreachable >(s)) return;
      }
    }
    roots.push_back(main);
    if (!inventory(*main->getParent(), main) || roots.size() < 2) { log::debug("Mutex handover inventory refused"); return; }
    std::vector< llvm::GlobalVariable* > mutexes;
    for (auto& g : main->getParent()->globals()) {
      bool found = false;
      for (const auto& c : mutex_calls) found |= c.second.source == &g;
      if (found) { if (!close_mutex(&g)) { log::debug("Mutex handover address uses refused: " + g.getName().str()); return; } mutexes.push_back(&g); }
    }
    // First subset closes exactly two runtime mutexes, not a selected subgraph.
    if (mutexes.size() != 2) return;
    for (unsigned i = 0; i < 2; ++i) {
      Proof p; p.outer = mutexes[i]; p.inner = mutexes[1 - i];
      if (phases(p)) proofs.push_back(std::move(p));
      else log::debug("Mutex handover state closure refused");
    }
  }
  bool ordered(const AccessRecord& a, const AccessRecord& b) const { return pair(a, b); }
  JsonDict info(const AccessRecord& a, const AccessRecord& b) const {
    auto* p = pair(a, b);
    return p ? JsonDict{{"safe_reason", "continuous mutex intervals force access ordering"},
                        {"mutex_handover_contract", "closed-two-mutex-interval-handover-v1"},
                        {"outer_mutex", p->outer->getName().str()},
                        {"inner_mutex", p->inner->getName().str()}} : JsonDict{};
  }
};
MutexHandover::MutexHandover(Context& ctx) : _impl(std::make_unique< Impl >(ctx)) {}
MutexHandover::~MutexHandover() = default;
bool MutexHandover::ordered(const AccessRecord& a, const AccessRecord& b) const { return _impl->ordered(a, b); }
JsonDict MutexHandover::info(const AccessRecord& a, const AccessRecord& b) const { return _impl->info(a, b); }
} // namespace analyzer
} // namespace ikos
