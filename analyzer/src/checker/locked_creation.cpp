// Copyright and license: see data_race.hpp.
#include <ikos/analyzer/checker/locked_creation.hpp>
#include <ikos/analyzer/analysis/source_call_binding.hpp>
#include <ikos/analyzer/checker/data_race.hpp>
#include <ikos/analyzer/analysis/call_context.hpp>
#include <ikos/analyzer/util/log.hpp>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <functional>
#include <set>

namespace ikos {
namespace analyzer {
class LockedCreation::Impl {
  Context& ctx;
  unsigned budget = 65536, bits = 0;
  llvm::Function* main = nullptr;
  std::map< llvm::Value*, ar::Statement* > originals;
  std::map< llvm::Function*, ar::Function* > functions;
  std::map< llvm::GlobalVariable*, ar::GlobalVariable* > globals;
  std::set< llvm::Function* > roots;
  std::map< llvm::Function*, std::vector< llvm::CallBase* > > creates;
  struct Interval { llvm::CallBase* lock = nullptr; llvm::CallBase* unlock = nullptr; };
  struct Mutex {
    llvm::GlobalVariable* source;
    std::map< llvm::Function*, Interval > intervals;
  };
  std::vector< Mutex > mutexes;
  struct Phase {
    std::string root;
    std::set< unsigned > held, post_lock, parent_held, parent_forever;
  };
  std::map< ar::Statement*, Phase > phases;
  bool refuse(unsigned line) const {
    log::debug("Locked creation refused at " + std::to_string(line)); return false;
  }
  bool step() { if (!budget) return false; --budget; return true; }
  ar::Statement* original(llvm::Value* v) const {
    auto it = originals.find(v); return it == originals.end() ? nullptr : it->second;
  }
  llvm::Function* callee(llvm::CallBase* c) const {
    return llvm::dyn_cast< llvm::Function >(c->getCalledOperand()->stripPointerCasts());
  }
  bool ordinary(llvm::AttributeSet attributes) const {
      for (auto attribute : attributes) {
        if (attribute.isStringAttribute()) return false;
        switch (attribute.getKindAsEnum()) {
          case llvm::Attribute::NoUndef: case llvm::Attribute::NonNull:
          case llvm::Attribute::NoCapture: case llvm::Attribute::ReadOnly:
          case llvm::Attribute::WriteOnly: case llvm::Attribute::ReadNone:
          case llvm::Attribute::Dereferenceable: case llvm::Attribute::DereferenceableOrNull:
          case llvm::Attribute::Alignment: case llvm::Attribute::NoAlias: break;
          default: return false;
        }
      }
      return true;
  }
  ar::CallBase* binding(llvm::CallBase* c) const {
    auto it = originals.find(c);
    auto* a = it == originals.end() ? nullptr : source_call_binding(c, it->second);
    auto* f = callee(c);
    if (!a || !f) return nullptr;
    if (!ordinary(c->getAttributes().getRetAttrs()) || !ordinary(f->getAttributes().getRetAttrs())) return nullptr;
    for (unsigned n = 0; n < c->arg_size(); ++n)
      if (!ordinary(c->getAttributes().getParamAttrs(n)) || !ordinary(f->getAttributes().getParamAttrs(n))) return nullptr;
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
  bool acyclic(llvm::Function* f) {
    std::set< llvm::BasicBlock* > active, done;
    std::function< bool(llvm::BasicBlock*) > visit = [&](llvm::BasicBlock* b) {
      if (!step()) return false;
      if (done.count(b)) return true;
      if (!active.insert(b).second) return false;
      for (auto* next : llvm::successors(b)) if (!visit(next)) return false;
      active.erase(b); done.insert(b); return true;
    };
    for (auto& b : *f) if (!visit(&b)) return false;
    return true;
  }
  bool create(llvm::CallBase* c) {
    auto* a = binding(c); auto* f = callee(c);
    auto* t = a ? native_handle_callee(a) : nullptr;
    if (!a || !t || !t->has_frontend() || t->frontend< llvm::Value >() != f ||
        !f || !f->isDeclaration() || f->getName() != "pthread_create" ||
        t->name() != "pthread_create" || !c->getType()->isIntegerTy(32) ||
        !t->type()->return_type()->is_integer() ||
        cast< ar::IntegerType >(t->type()->return_type())->bit_width() != 32 ||
        c->arg_size() != 4 || !c->use_empty() || c->getFunction() != main ||
        !isa< ar::NullConstant >(a->argument(1)) || !isa< ar::NullConstant >(a->argument(3)) ||
        !llvm::isa< llvm::ConstantPointerNull >(c->getArgOperand(1)) ||
        !llvm::isa< llvm::ConstantPointerNull >(c->getArgOperand(3))) return refuse(__LINE__);
    auto* entry = llvm::dyn_cast< llvm::Function >(c->getArgOperand(2));
    auto* actual = dyn_cast< ar::FunctionPointerConstant >(a->argument(2));
    auto* out = llvm::dyn_cast< llvm::PointerType >(c->getArgOperand(0)->getType());
    if (!entry || entry == main || !functions.count(entry) || !functions.at(entry)->is_definition() ||
        !actual || actual->function() != functions.at(entry) || entry->isVarArg() ||
        entry->arg_size() != 1 || !entry->getArg(0)->use_empty() ||
        entry->getCallingConv() != llvm::CallingConv::C || !entry->getReturnType()->isPointerTy() ||
        !entry->getArg(0)->getType()->isPointerTy() ||
        llvm::cast< llvm::PointerType >(entry->getReturnType())->getAddressSpace() ||
        llvm::cast< llvm::PointerType >(entry->getArg(0)->getType())->getAddressSpace() ||
        !out || out->getAddressSpace() || !out->getPointerElementType()->isIntegerTy(bits)) return refuse(__LINE__);
    if (!ordinary(entry->getAttributes().getRetAttrs()) ||
        !ordinary(entry->getAttributes().getParamAttrs(0))) return refuse(__LINE__);
    auto* slot = llvm::dyn_cast< llvm::GlobalVariable >(c->getArgOperand(0));
    auto* local = llvm::dyn_cast< llvm::AllocaInst >(c->getArgOperand(0));
    if (slot) {
      if (!globals.count(slot) || !globals.at(slot)->is_definition() || globals.at(slot)->is_thread_local() ||
          slot->getValueType() != out->getPointerElementType() || slot->isConstant() || slot->isThreadLocal() ||
          slot->isExternallyInitialized() || !slot->hasInitializer() ||
          !exact_handle_slot(a->argument(0), globals.at(slot)) ||
          !globals.at(slot)->type()->pointee()->is_integer() ||
          cast< ar::IntegerType >(globals.at(slot)->type()->pointee())->bit_width() != bits ||
          ctx.bundle->data_layout().alloc_size_in_bytes(globals.at(slot)->type()->pointee()) != bits / 8 ||
          (slot->getAlign() ? slot->getAlign()->value() : slot->getParent()->getDataLayout().getABITypeAlign(slot->getValueType()).value()) < bits / 8) return refuse(__LINE__);
    } else {
      auto* allocation = local ? dyn_cast_or_null< ar::Allocate >(original(local)) : nullptr;
      auto* count = allocation ? dyn_cast< ar::IntegerConstant >(allocation->array_size()) : nullptr;
      auto* native_count = local ? llvm::dyn_cast< llvm::ConstantInt >(local->getArraySize()) : nullptr;
      auto* type = allocation ? dyn_cast< ar::IntegerType >(allocation->allocated_type()) : nullptr;
      if (!allocation || a->argument(0) != allocation->result() || !type || type->bit_width() != bits ||
          !count || count->value().to_z_number() != 1 || !native_count || !native_count->isOne() ||
          local->getAllocatedType() != out->getPointerElementType() || local->getAlign().value() < bits / 8) return refuse(__LINE__);
    }
    roots.insert(entry); creates[entry].push_back(c); return true;
  }
  bool object(llvm::GlobalVariable* g) {
    auto& layout = main->getParent()->getDataLayout();
    unsigned width = bits == 32 ? 24 : 40, align = bits / 8;
    if (!globals.count(g) || !globals.at(g)->is_definition() || globals.at(g)->is_thread_local() ||
        g->isConstant() || g->isThreadLocal() || g->isExternallyInitialized() || g->getAddressSpace() ||
        !g->hasInitializer() || !g->getInitializer()->isNullValue() ||
        layout.getTypeAllocSize(g->getValueType()) != width ||
        (g->getAlign() ? g->getAlign()->value() : layout.getABITypeAlign(g->getValueType()).value()) < align ||
        ctx.bundle->data_layout().alloc_size_in_bytes(globals.at(g)->type()->pointee()) != width) return refuse(__LINE__);
    unsigned initialized = 0;
    for (auto* b : *globals.at(g)->initializer()) for (auto* s : *b) {
      auto* store = dyn_cast< ar::Store >(s);
      if (!store || store->pointer() != globals.at(g) || !isa< ar::AggregateZeroConstant >(store->value()) ||
          store->value()->type() != globals.at(g)->type()->pointee()) return refuse(__LINE__);
      ++initialized;
    }
    if (initialized != 1) return refuse(__LINE__);
    Mutex mutex{g, {}};
    std::vector< llvm::Value* > todo{g}; std::set< llvm::Value* > seen;
    while (!todo.empty()) {
      auto* v = todo.back(); todo.pop_back();
      if (!step()) return refuse(__LINE__);
      if (!seen.insert(v).second) continue;
      for (auto* u : v->users()) {
        if (llvm::isa< llvm::BitCastOperator >(u)) { todo.push_back(u); continue; }
        if (auto* gep = llvm::dyn_cast< llvm::GEPOperator >(u)) {
          if (gep->getPointerOperand() != v || !gep->hasAllZeroIndices()) return refuse(__LINE__);
          todo.push_back(u); continue;
        }
        auto* c = llvm::dyn_cast< llvm::CallBase >(u);
        auto* a = c ? binding(c) : nullptr; auto* f = c ? callee(c) : nullptr;
        auto* t = a ? native_handle_callee(a) : nullptr;
        auto* pointer = c && !c->arg_empty() ? llvm::dyn_cast< llvm::PointerType >(c->getArgOperand(0)->getType()) : nullptr;
        auto* type = pointer ? llvm::dyn_cast< llvm::StructType >(pointer->getPointerElementType()) : nullptr;
        if (!a || !f || !t || (t->has_frontend() && t->frontend< llvm::Value >() != f) ||
            !f->isDeclaration() || !roots.count(c->getFunction()) || c->arg_size() != 1 ||
            !c->use_empty() || !c->getType()->isIntegerTy(32) ||
            c->getArgOperand(0) != v || !pointer || pointer->getAddressSpace() || !type ||
            type->isOpaque() || !type->hasName() || type->getName() != "union.pthread_mutex_t" ||
            layout.getTypeAllocSize(type) != width || f->getFunctionType()->getParamType(0) != pointer ||
            !exact_handle_slot(a->argument(0), globals.at(g))) return refuse(__LINE__);
        bool lock = f->getName() == "pthread_mutex_lock" && t->intrinsic_id() == ar::Intrinsic::PthreadMutexLock;
        bool unlock = f->getName() == "pthread_mutex_unlock" && t->intrinsic_id() == ar::Intrinsic::PthreadMutexUnlock;
        auto& interval = mutex.intervals[c->getFunction()];
        if (lock) { if (interval.lock) return refuse(__LINE__); interval.lock = c; }
        else if (unlock) { if (interval.unlock) return refuse(__LINE__); interval.unlock = c; }
        else return refuse(__LINE__);
      }
    }
    for (auto& item : mutex.intervals) {
      auto& interval = item.second;
      if (!interval.lock) return refuse(__LINE__);
      llvm::DominatorTree dt(*item.first);
      if (interval.unlock && !dt.dominates(interval.lock, interval.unlock)) return refuse(__LINE__);
    }
    mutexes.push_back(std::move(mutex)); return true;
  }
  bool reaches(llvm::Instruction* from, llvm::Instruction* to) {
    if (from->getFunction() != to->getFunction()) return false;
    if (from->getParent() == to->getParent()) return from == to || from->comesBefore(to);
    std::vector< llvm::BasicBlock* > todo; std::set< llvm::BasicBlock* > seen;
    for (auto* next : llvm::successors(from->getParent())) todo.push_back(next);
    while (!todo.empty()) {
      auto* block = todo.back(); todo.pop_back();
      if (!step()) return true; // Uncertain reachability cannot grant a MUST fact.
      if (block == to->getParent()) return true;
      if (!seen.insert(block).second) continue;
      for (auto* next : llvm::successors(block)) todo.push_back(next);
    }
    return false;
  }
  bool held(Interval interval, llvm::Instruction* p, llvm::DominatorTree& dt) {
    return interval.lock && dt.dominates(interval.lock, p) &&
           (!interval.unlock || !reaches(interval.unlock, p));
  }
  // ponytail: direct DAG roots, whole scalars and one normal-lock interval per
  // root/object; helpers, loops and joins need contextual lifecycle certificates.
  bool build() {
    if (!ctx.opts.enable_thread_modular || ctx.opts.entry_points.size() != 1 ||
        ctx.opts.entry_points.front()->name() != "main" || !ctx.opts.entry_points.front()->has_frontend() ||
        !ctx.opts.no_init_globals.empty() || ctx.concurrent_env.is_dirty() ||
        !ctx.concurrent_env.unmodeled_effects().empty() || !ctx.concurrent_env.unmodeled_thread_creations().empty() ||
        ctx.bundle->global_or_null("ar.global_ctors") || ctx.bundle->global_or_null("ar.global_dtors")) return refuse(__LINE__);
    main = ctx.opts.entry_points.front()->frontend< llvm::Function >();
    auto& module = *main->getParent();
    if (!module.getFunction("pthread_mutex_lock")) return refuse(__LINE__);
    bits = ctx.bundle->data_layout().pointers.bit_width;
    llvm::Triple triple(module.getTargetTriple());
    if (!main->use_empty() || main->isDeclaration() || main->isVarArg() ||
        main->arg_size() != 0 || !main->getReturnType()->isIntegerTy(32) ||
        main->getCallingConv() != llvm::CallingConv::C || !ordinary(main->getAttributes().getRetAttrs()) || (bits != 32 && bits != 64) ||
        module.getDataLayout().getPointerSizeInBits() != bits ||
        (bits == 32 ? triple.getArch() != llvm::Triple::x86 : triple.getArch() != llvm::Triple::x86_64) ||
        !triple.isOSLinux() || triple.getEnvironment() != llvm::Triple::GNU ||
        !module.alias_empty() || !module.getModuleInlineAsm().empty() ||
        !mutex_effect_pointer_representations_closed(module)) return refuse(__LINE__);
    for (auto it = ctx.bundle->global_begin(); it != ctx.bundle->global_end(); ++it)
      if ((*it)->has_frontend()) globals.emplace((*it)->frontend< llvm::GlobalVariable >(), *it);
    for (auto it = ctx.bundle->function_begin(); it != ctx.bundle->function_end(); ++it) {
      auto* f = *it; if (f->has_frontend()) functions.emplace(f->frontend< llvm::Function >(), f);
      if (!f->is_definition()) continue;
      for (auto* b : *f->body()) for (auto* s : *b) {
        if (isa< ar::CallBase >(s) || isa< ar::Load >(s) || isa< ar::Store >(s) || isa< ar::Allocate >(s)) {
          auto* n = s->has_frontend() ? llvm::dyn_cast< llvm::Instruction >(s->frontend< llvm::Value >()) : nullptr;
          if (!n || !f->has_frontend() || n->getFunction() != f->frontend< llvm::Function >() ||
              !originals.emplace(n, s).second) return refuse(__LINE__);
        } else if (!isa< ar::Assignment >(s) && !isa< ar::UnaryOperation >(s) &&
                   !isa< ar::BinaryOperation >(s) && !isa< ar::Comparison >(s) &&
                   !isa< ar::PointerShift >(s) && !isa< ar::ReturnValue >(s) && !isa< ar::Unreachable >(s)) return refuse(__LINE__);
      }
    }
    roots.insert(main);
    for (auto& b : *main) for (auto& i : b) {
      auto* c = llvm::dyn_cast< llvm::CallBase >(&i); auto* f = c ? callee(c) : nullptr;
      if (f && f->getName() == "pthread_create" && !create(c)) return refuse(__LINE__);
    }
    if (creates.empty()) return refuse(__LINE__);
    std::set< llvm::GlobalVariable* > mutex_objects;
    for (auto& f : module) for (auto& b : f) for (auto& i : b) {
      auto* c = llvm::dyn_cast< llvm::CallBase >(&i); auto* t = c ? callee(c) : nullptr;
      if (!t || (t->getName() != "pthread_mutex_lock" && t->getName() != "pthread_mutex_unlock")) continue;
      if (c->arg_size() != 1) return refuse(__LINE__);
      auto* g = llvm::dyn_cast< llvm::GlobalVariable >(c->getArgOperand(0)->stripPointerCasts());
      if (!g) return refuse(__LINE__);
      mutex_objects.insert(g);
    }
    for (auto& g : module.globals())
      if (g.isDeclaration() && !g.use_empty()) return refuse(__LINE__);
    for (auto* g : mutex_objects) if (!object(g)) return refuse(__LINE__);
    std::set< llvm::Value* > consumed;
    for (auto& f : module) {
      if (f.isDeclaration() || (&f != main && f.use_empty())) continue;
      if (!roots.count(&f) || !functions.count(&f) || !acyclic(&f)) return refuse(__LINE__);
      for (auto* u : f.users()) {
        auto* c = llvm::dyn_cast< llvm::CallBase >(u);
        if (!c || c->getFunction() != main || c->arg_size() != 4 ||
            c->getArgOperand(2) != &f || !creates.count(&f)) return refuse(__LINE__);
      }
      for (auto& b : f) for (auto& i : b) {
        if (!step()) return refuse(__LINE__);
        if (llvm::isa< llvm::DbgInfoIntrinsic >(i)) continue;
        if (llvm::isa< llvm::LoadInst >(i) || llvm::isa< llvm::StoreInst >(i)) {
          if (!scalar_memory(i)) return refuse(__LINE__);
          consumed.insert(&i);
        } else if (auto* c = llvm::dyn_cast< llvm::CallBase >(&i)) {
          auto* a = binding(c); auto* target = callee(c);
          if (!a || !target || !target->isDeclaration()) return refuse(__LINE__);
          consumed.insert(c);
          bool known = false;
          for (auto& m : mutexes) {
            auto it = m.intervals.find(&f);
            if (it != m.intervals.end() && (it->second.lock == c || it->second.unlock == c)) known = true;
          }
          for (auto& item : creates) for (auto* creation : item.second) known |= creation == c;
          if (!known && !terminal(c) && !pure_external(c)) return refuse(__LINE__);
        } else if (auto* local = llvm::dyn_cast< llvm::AllocaInst >(&i)) {
          auto* count = llvm::dyn_cast< llvm::ConstantInt >(local->getArraySize());
          auto* allocation = dyn_cast_or_null< ar::Allocate >(original(local));
          auto* size = allocation ? dyn_cast< ar::IntegerConstant >(allocation->array_size()) : nullptr;
          if (!count || !count->isOne() || !local->getAllocatedType()->isIntegerTy() ||
              !allocation || !size || size->value().to_z_number() != 1 ||
              !allocation->result()->has_frontend() || allocation->result()->frontend< llvm::Value >() != local ||
              !allocation->allocated_type()->is_integer() ||
              cast< ar::IntegerType >(allocation->allocated_type())->bit_width() != local->getAllocatedType()->getIntegerBitWidth()) return refuse(__LINE__);
          consumed.insert(local);
        } else if (!llvm::isa< llvm::BinaryOperator >(i) && !llvm::isa< llvm::CastInst >(i) &&
                   !llvm::isa< llvm::CmpInst >(i) && !llvm::isa< llvm::PHINode >(i) &&
                   !llvm::isa< llvm::SelectInst >(i) && !llvm::isa< llvm::GetElementPtrInst >(i) &&
                   !llvm::isa< llvm::BranchInst >(i) && !llvm::isa< llvm::SwitchInst >(i) &&
                   !llvm::isa< llvm::ReturnInst >(i) && !llvm::isa< llvm::UnreachableInst >(i)) return refuse(__LINE__);
      }
    }
    for (auto& item : originals) if (!consumed.count(item.first)) return refuse(__LINE__);
    llvm::DominatorTree parent_dt(*main);
    for (auto* root : roots) {
      llvm::DominatorTree dt(*root);
      for (auto& b : *root) for (auto& i : b) {
        if (!llvm::isa< llvm::LoadInst >(i) && !llvm::isa< llvm::StoreInst >(i)) continue;
        Phase phase{functions.at(root)->name(), {}, {}, {}, {}};
        for (unsigned n = 0; n < mutexes.size(); ++n) {
          auto& m = mutexes[n];
          auto own = m.intervals.find(root), parent = m.intervals.find(main);
          if (own != m.intervals.end()) {
            if (held(own->second, &i, dt)) phase.held.insert(n);
            if (own->second.lock && dt.dominates(own->second.lock, &i)) phase.post_lock.insert(n);
          }
          if (root == main || parent == m.intervals.end()) continue;
          bool all_held = true, forever = true;
          for (auto* c : creates.at(root)) {
            all_held &= held(parent->second, c, parent_dt);
            if (parent->second.unlock && reaches(c, parent->second.unlock)) forever = false;
          }
          if (all_held) {
            phase.parent_held.insert(n);
            if (forever) phase.parent_forever.insert(n);
          }
        }
        phases.emplace(original(&i), std::move(phase));
      }
    }
    return true;
  }
  const Phase* endpoint(const AccessRecord& r) const {
    if (!r.stmt || !r.width || !r.call_context || !r.call_context->empty()) return nullptr;
    auto it = phases.find(r.stmt);
    if (it == phases.end() || it->second.root != r.thread_id) return nullptr;
    if ((r.kind == AccessKind::Read && !isa< ar::Load >(r.stmt)) ||
        (r.kind == AccessKind::Write && !isa< ar::Store >(r.stmt))) return nullptr;
    return &it->second;
  }
public:
  explicit Impl(Context& c) : ctx(c) { if (!build()) phases.clear(); }
  unsigned direction(const Phase& a, const Phase& b) const {
    // A: the parent's access precedes the child's own acquisition.
    if (a.root == "main")
      for (unsigned n : a.held)
        if (b.parent_held.count(n) && b.post_lock.count(n)) return 2 * n + 1;
    // B: main never releases after create; another actual holder precedes child.
    if (a.root != "main")
      for (unsigned n : a.held)
        if (b.parent_forever.count(n)) return 2 * n + 2;
    return 0;
  }
  bool ordered(const AccessRecord& a, const AccessRecord& b) const {
    auto* x = endpoint(a); auto* y = endpoint(b);
    return x && y && (direction(*x, *y) || direction(*y, *x));
  }
  JsonDict info(const AccessRecord& a, const AccessRecord& b) const {
    auto* x = endpoint(a); auto* y = endpoint(b);
    if (!x || !y) return {};
    unsigned proof = direction(*x, *y);
    if (!proof) { std::swap(x, y); proof = direction(*x, *y); }
    if (!proof) return {};
    return {{"safe_reason", "actual normal-lock intervals order direct creation accesses"},
            {"locked_creation_contract", "closed-normal-mutex-creation-v1"},
            {"mutex", mutexes[(proof - 1) / 2].source->getName().str()},
            {"relation", proof % 2 ? "parent-held-before-child-acquisition" : "other-holder-before-child-created-under-retained-lock"},
            {"before_root", x->root}, {"after_root", y->root}};
  }
};
LockedCreation::LockedCreation(Context& c) : _impl(std::make_unique< Impl >(c)) {}
LockedCreation::~LockedCreation() = default;
bool LockedCreation::ordered(const AccessRecord& a, const AccessRecord& b) const { return _impl->ordered(a, b); }
JsonDict LockedCreation::info(const AccessRecord& a, const AccessRecord& b) const { return _impl->info(a, b); }
} // namespace analyzer
} // namespace ikos
