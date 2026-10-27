// Copyright and license: see data_race.hpp.
#include <ikos/ar/semantic/pthread.hpp>
#include <ikos/analyzer/checker/locked_creation.hpp>
#include <ikos/analyzer/analysis/source_call_binding.hpp>
#include <ikos/analyzer/analysis/source_program_facts.hpp>
#include <ikos/analyzer/analysis/pthread_initialization_frame.hpp>
#include <ikos/analyzer/checker/data_race.hpp>
#include <ikos/analyzer/analysis/call_context.hpp>
#include <ikos/analyzer/util/log.hpp>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <set>

namespace ikos {
namespace analyzer {
class LockedCreation::Impl {
  Context& ctx;
  SourceProgramFacts& source_facts;
  unsigned budget = 65536, bits = 0;
  llvm::Function* main = nullptr;
  const std::map< llvm::Value*, ar::Statement* >& originals;
  const std::map< llvm::Function*, ar::Function* >& functions;
  const std::map< llvm::GlobalVariable*, ar::GlobalVariable* >& globals;
  std::set< llvm::Function* > roots;
  std::map< llvm::Function*, std::vector< llvm::CallBase* > > creates;
  std::map< llvm::Function*, llvm::CallBase* > joined_by;
  std::set< llvm::CallBase* > joins;
  struct Interval { llvm::CallBase* lock = nullptr; llvm::CallBase* unlock = nullptr; };
  struct Mutex {
    llvm::GlobalVariable* source;
    std::map< llvm::Function*, Interval > intervals;
  };
  std::vector< Mutex > mutexes;
  struct Phase {
    std::string root;
    std::set< unsigned > held, post_lock, parent_held, parent_forever, after_release, bounded_main;
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
  ar::CallBase* binding(llvm::CallBase* c) const {
    auto it = originals.find(c);
    auto* a = it == originals.end() ? nullptr : source_call_binding(c, it->second);
    auto* f = callee(c);
    if (!a || !f) return nullptr;
    if (!ordinary_source_attributes(c->getAttributes().getRetAttrs()) || !ordinary_source_attributes(f->getAttributes().getRetAttrs())) return nullptr;
    for (unsigned n = 0; n < c->arg_size(); ++n)
      if (!ordinary_source_attributes(c->getAttributes().getParamAttrs(n)) || !ordinary_source_attributes(f->getAttributes().getParamAttrs(n))) return nullptr;
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
        (cast< ar::PointerType >(base->type())->pointee() != at &&
         (!at->is_integer() || !cast< ar::PointerType >(base->type())->pointee()->is_integer() ||
          cast< ar::IntegerType >(cast< ar::PointerType >(base->type())->pointee())->bit_width() !=
              cast< ar::IntegerType >(at)->bit_width())) ||
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
        !ar::is_pthread_create(t) || !c->getType()->isIntegerTy(32) ||
        !t->type()->return_type()->is_integer() ||
        cast< ar::IntegerType >(t->type()->return_type())->bit_width() != 32 ||
        c->arg_size() != 4 || !c->use_empty() ||
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
    if (!ordinary_source_attributes(entry->getAttributes().getRetAttrs()) ||
        !ordinary_source_attributes(entry->getAttributes().getParamAttrs(0))) return refuse(__LINE__);
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
  bool join(llvm::CallBase* c) {
    auto* a = binding(c); auto* f = callee(c);
    auto* target = a ? native_handle_callee(a) : nullptr;
    auto* load = c->arg_size() == 2 ? llvm::dyn_cast< llvm::LoadInst >(c->getArgOperand(0)) : nullptr;
    auto* slot = load ? llvm::dyn_cast< llvm::GlobalVariable >(load->getPointerOperand()) : nullptr;
    auto* al = load ? dyn_cast_or_null< ar::Load >(original(load)) : nullptr;
    auto* output = c->arg_size() == 2 ? llvm::dyn_cast< llvm::PointerType >(c->getArgOperand(1)->getType()) : nullptr;
    auto* pointer = output ? llvm::dyn_cast< llvm::PointerType >(output->getPointerElementType()) : nullptr;
    if (!a || !target || !f->isDeclaration() || f->getName() != "pthread_join" ||
        !ar::is_pthread_join(target) ||
        !c->use_empty() || !c->getType()->isIntegerTy(32) ||
        !target->type()->return_type()->is_integer() ||
        cast< ar::IntegerType >(target->type()->return_type())->bit_width() != 32 ||
        !load || !load->isSimple() || !load->getType()->isIntegerTy(bits) ||
        !output || output->getAddressSpace() || !pointer || pointer->getAddressSpace() ||
        !pointer->getPointerElementType()->isIntegerTy(8) ||
        !llvm::isa< llvm::ConstantPointerNull >(c->getArgOperand(1)) ||
        !isa< ar::NullConstant >(a->argument(1)) || !slot || !globals.count(slot) ||
        !al || al->is_atomic() || al->is_volatile() || unique_def(al->result()) != al ||
        address_base(al->operand()) != globals.at(slot) ||
        a->argument(0) != al->result() || !source_integer_binding(al->result(), load)) return refuse(__LINE__);
    llvm::CallBase* creation = nullptr; llvm::Function* child = nullptr;
    for (auto& item : creates) for (auto* candidate : item.second) {
      if (!step()) return refuse(__LINE__);
      if (candidate->getArgOperand(0) != slot) continue;
      if (creation || item.second.size() != 1) return refuse(__LINE__);
      creation = candidate; child = item.first;
    }
    if (!creation || creation->getFunction() != c->getFunction() ||
        load->getFunction() != c->getFunction() || joined_by.count(child)) return refuse(__LINE__);
    auto& dt = source_facts.dominators(c->getFunction());
    if (!dt.dominates(creation, load) || !dt.dominates(load, c)) return refuse(__LINE__);
    for (auto* u : slot->users())
      if (u != creation && u != load && !llvm::isa< llvm::DbgInfoIntrinsic >(u)) return refuse(__LINE__);
    for (auto* u : load->users()) if (u != c) return refuse(__LINE__);
    // Close imported address uses too; lowering may insert only exact pointer copies.
    auto* storage = globals.at(slot);
    auto* ac = dyn_cast< ar::CallBase >(original(creation));
    std::vector< ar::Value* > pending{storage}; std::set< ar::Value* > seen;
    while (!pending.empty()) {
      auto* value = pending.back(); pending.pop_back();
      if (!seen.insert(value).second) continue;
      for (auto& fn : functions) if (fn.second->is_definition())
        for (auto* b : *fn.second->body()) for (auto* statement : *b) {
          if (!step()) return refuse(__LINE__);
          bool uses = false;
          for (auto it = statement->op_begin(); it != statement->op_end(); ++it) {
            uses |= *it == value;
            if (a->has_result() && *it == a->result()) return refuse(__LINE__);
          }
          if (!uses || (statement == ac && ac->argument(0) == value) ||
              (statement == al && al->operand() == value)) continue;
          auto* copy = dyn_cast< ar::Assignment >(statement);
          auto* cast = dyn_cast< ar::UnaryOperation >(statement);
          auto* shift = dyn_cast< ar::PointerShift >(statement);
          ar::InternalVariable* next = copy ? copy->result() : cast ? cast->result() : shift ? shift->result() : nullptr;
          if (!next || !next->type()->is_pointer() || unique_def(next) != statement ||
              (cast && cast->op() != ar::UnaryOperation::Bitcast) ||
              address_base(next) != storage) return refuse(__LINE__);
          pending.push_back(next);
        }
    }
    joined_by.emplace(child, c); joins.insert(c); return true;
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
    const auto& recursive = ctx.pthread_initialization_frame();
    if (recursive && recursive->recursive_object(globals.at(g))) {
      for (auto* root : roots) {
        auto* region = recursive->recursive_region(globals.at(g), functions.at(root));
        if (!region) continue;
        if (!region->lock || !region->lock->has_frontend() ||
            (region->unlock && !region->unlock->has_frontend())) return refuse(__LINE__);
        mutex.intervals.emplace(root, Interval{
            llvm::dyn_cast< llvm::CallBase >(region->lock->frontend< llvm::Value >()),
            region->unlock ? llvm::dyn_cast< llvm::CallBase >(region->unlock->frontend< llvm::Value >()) : nullptr});
        if (!mutex.intervals.at(root).lock) return refuse(__LINE__);
      }
      // The finite frame closes every native/AR use and identifies the actual
      // first acquisition and final release. Inner relocks/partial unlocks are
      // retained calls, not new outer interval boundaries.
      mutexes.push_back(std::move(mutex)); return true;
    }
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
      auto& dt = source_facts.dominators(item.first);
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
  bool held(Interval interval, llvm::Instruction* p, const llvm::DominatorTree& dt) {
    return interval.lock && dt.dominates(interval.lock, p) &&
           (!interval.unlock || !reaches(interval.unlock, p));
  }
  // ponytail: acyclic spawn/CFG graphs, whole scalars and one normal interval
  // per root/object; exclusive same-creator global-handle joins can bound a
  // closed descendant subtree inside main's retained-lock interval.
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
        main->getCallingConv() != llvm::CallingConv::C || !ordinary_source_attributes(main->getAttributes().getRetAttrs()) || (bits != 32 && bits != 64) ||
        module.getDataLayout().getPointerSizeInBits() != bits ||
        (bits == 32 ? triple.getArch() != llvm::Triple::x86 : triple.getArch() != llvm::Triple::x86_64) ||
        !triple.isOSLinux() || triple.getEnvironment() != llvm::Triple::GNU ||
        !module.alias_empty() || !module.getModuleInlineAsm().empty() ||
        !mutex_effect_pointer_representations_closed(module)) return refuse(__LINE__);
    if (!source_facts.index() || !source_facts.memory_calls_allocations().unique) return refuse(__LINE__);
    for (auto it = ctx.bundle->function_begin(); it != ctx.bundle->function_end(); ++it) {
      auto* f = *it;
      if (!f->is_definition()) continue;
      for (auto* b : *f->body()) for (auto* s : *b) {
        if (isa< ar::CallBase >(s) || isa< ar::Load >(s) || isa< ar::Store >(s) || isa< ar::Allocate >(s)) {
          auto* n = s->has_frontend() ? llvm::dyn_cast< llvm::Instruction >(s->frontend< llvm::Value >()) : nullptr;
          if (!n || !f->has_frontend() || n->getFunction() != f->frontend< llvm::Function >() ||
              original(n) != s) return refuse(__LINE__);
        } else if (!isa< ar::Assignment >(s) && !isa< ar::UnaryOperation >(s) &&
                   !isa< ar::BinaryOperation >(s) && !isa< ar::Comparison >(s) &&
                   !isa< ar::PointerShift >(s) && !isa< ar::ReturnValue >(s) && !isa< ar::Unreachable >(s)) return refuse(__LINE__);
      }
    }
    roots.insert(main);
    for (auto& source : module) for (auto& b : source) for (auto& i : b) {
      auto* c = llvm::dyn_cast< llvm::CallBase >(&i); auto* f = c ? callee(c) : nullptr;
      if (f && f->getName() == "pthread_create" && !create(c)) return refuse(__LINE__);
    }
    if (creates.empty()) return refuse(__LINE__);
    std::vector< llvm::Function* > spawn_order;
    std::set< llvm::Function* > active, done;
    std::function< bool(llvm::Function*) > visit = [&](llvm::Function* parent) {
      if (!step() || active.count(parent)) return false;
      if (done.count(parent)) return true;
      active.insert(parent);
      for (auto& child : creates) for (auto* c : child.second) {
        if (!step()) return false;
        if (c->getFunction() == parent && !visit(child.first)) return false;
      }
      active.erase(parent); done.insert(parent); spawn_order.push_back(parent); return true;
    };
    if (!visit(main) || done.size() != roots.size()) return refuse(__LINE__);
    std::reverse(spawn_order.begin(), spawn_order.end());
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
        if (!c || c->arg_size() != 4 || c->getArgOperand(2) != &f ||
            !creates.count(&f) ||
            std::find(creates.at(&f).begin(), creates.at(&f).end(), c) == creates.at(&f).end()) return refuse(__LINE__);
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
          const auto& recursive = ctx.pthread_initialization_frame();
          bool known = recursive && (recursive->recursive_mutex(a) ||
              (recursive->effect(a) && recursive->effect(a)->result_known));
          for (auto& m : mutexes) {
            auto it = m.intervals.find(&f);
            if (it != m.intervals.end() && (it->second.lock == c || it->second.unlock == c)) known = true;
          }
          for (auto& item : creates) for (auto* creation : item.second) known |= creation == c;
          if (!known && target->getName() == "pthread_join") known = join(c);
          if (!known && !terminal(c) && !pure_external(c)) return refuse(__LINE__);
        } else if (auto* local = llvm::dyn_cast< llvm::AllocaInst >(&i)) {
          auto* count = llvm::dyn_cast< llvm::ConstantInt >(local->getArraySize());
          auto* allocation = dyn_cast_or_null< ar::Allocate >(original(local));
          auto* size = allocation ? dyn_cast< ar::IntegerConstant >(allocation->array_size()) : nullptr;
          if (ctx.pthread_initialization_frame() && allocation &&
              ctx.pthread_initialization_frame()->recursive_attribute(allocation->result())) {
            consumed.insert(local); continue;
          }
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
    if (!joins.empty())
      for (auto& item : creates) if (item.second.size() != 1) return refuse(__LINE__);
    // A static create is one dynamic generation only after closing every
    // root's incoming create, every CFG, and the acyclic spawn tree above.
    // A global handle cell must belong to exactly one of these generations.
    if (!joins.empty()) {
      std::set< llvm::Value* > slots;
      for (auto& item : creates)
        if (!slots.insert(item.second.front()->getArgOperand(0)).second)
          return refuse(__LINE__);
    }
    // A child can outlive its parent unless every returning path drains it.
    // This is a partial-correctness fact: blocking forever is allowed. It
    // neither proves termination nor makes the child hold its parent's lock.
    std::map< llvm::Function*, bool > drains;
    if (!joins.empty()) for (auto pos = spawn_order.rbegin(); pos != spawn_order.rend(); ++pos) {
      auto* root = *pos;
      bool complete = true, returned = false;
      auto& dt = source_facts.dominators(root);
      std::vector< llvm::ReturnInst* > returns;
      for (auto& block : *root)
        for (auto& instruction : block)
          if (auto* ret = llvm::dyn_cast< llvm::ReturnInst >(&instruction)) {
            if (!step()) return refuse(__LINE__);
            // Do not use impossible disconnected returns to weaken a gate.
            if (!dt.isReachableFromEntry(ret->getParent())) continue;
            returned = true;
            if (root != main &&
                !llvm::isa< llvm::ConstantPointerNull >(ret->getReturnValue()))
              complete = false;
            returns.push_back(ret);
          }
      for (auto& item : creates) {
        if (!step()) return refuse(__LINE__);
        if (item.second.size() != 1) continue;
        auto* child = item.first;
        auto* creation = item.second.front();
        if (creation->getFunction() != root) continue;
        auto joined = joined_by.find(child);
        if (joined == joined_by.end() || !drains[child]) {
          complete = false;
          continue;
        }
        for (auto* ret : returns) {
          if (!step()) return refuse(__LINE__);
          if (!dt.dominates(joined->second, ret)) complete = false;
        }
      }
      drains[root] = complete && returned;
    }
    auto& parent_dt = source_facts.dominators(main);
    std::map< llvm::Function*, std::set< unsigned > > birth_held, birth_forever, after_release;
    for (auto* root : roots) {
      if (root == main) continue;
      for (unsigned n = 0; n < mutexes.size(); ++n) {
        auto parent = mutexes[n].intervals.find(main);
        if (parent == mutexes[n].intervals.end()) continue;
        bool all_held = true, forever = true;
        for (auto* c : creates.at(root)) {
          if (!step()) return refuse(__LINE__);
          if (c->getFunction() != main) { all_held = false; break; }
          all_held &= held(parent->second, c, parent_dt);
          if (parent->second.unlock && reaches(c, parent->second.unlock)) forever = false;
        }
        if (all_held) {
          birth_held[root].insert(n);
          if (forever) birth_forever[root].insert(n);
        }
      }
    }
    // A child starts after its actual create; every possible creator must agree.
    for (auto* root : spawn_order) {
      if (root == main) continue;
      for (unsigned n = 0; n < mutexes.size(); ++n) {
        bool all_after = true;
        for (auto* c : creates.at(root)) {
          if (!step()) return refuse(__LINE__);
          auto* parent = c->getFunction();
          bool after = after_release[parent].count(n);
          if (!after && parent != main && birth_held[parent].count(n)) {
            auto own = mutexes[n].intervals.find(parent);
            auto& dt = source_facts.dominators(parent);
            after = own != mutexes[n].intervals.end() && own->second.lock &&
                    dt.dominates(own->second.lock, c);
          }
          all_after &= after;
        }
        if (all_after) after_release[root].insert(n);
      }
    }
    std::map< llvm::Function*, std::set< unsigned > > bounded_main;
    for (auto* root : spawn_order) {
      auto joined = joined_by.find(root);
      if (joined == joined_by.end()) continue;
      auto* c = creates.at(root).front(); auto* j = joined->second;
      auto* parent = c->getFunction();
      auto& dt = source_facts.dominators(parent);
      if (parent != main) continue;
      for (unsigned n : birth_held[root]) {
        auto interval = mutexes[n].intervals.at(main);
        if (held(interval, j, dt) &&
            (!interval.unlock || dt.dominates(j, interval.unlock))) bounded_main[root].insert(n);
      }
    }
    // The selected direct-child subtree, rather than all of main's
    // children, must be drained. Unrelated main children may stay live.
    // Propagate only lifetime containment; held[] remains the actual root's.
    for (auto* parent : spawn_order) {
      if (std::getenv("IKOS_RACE_DISABLE_NESTED_JOIN_SCOPE") ||
          parent == main || bounded_main[parent].empty() || !drains[parent])
        continue;
      for (auto& item : creates) {
        if (!step()) return refuse(__LINE__);
        if (item.second.size() != 1 ||
            item.second.front()->getFunction() != parent) continue;
        bounded_main[item.first].insert(bounded_main[parent].begin(),
                                        bounded_main[parent].end());
      }
    }
    for (auto* root : roots) {
      auto& dt = source_facts.dominators(root);
      for (auto& b : *root) for (auto& i : b) {
        if (!llvm::isa< llvm::LoadInst >(i) && !llvm::isa< llvm::StoreInst >(i)) continue;
        Phase phase{functions.at(root)->name(), {}, {}, birth_held[root],
                    birth_forever[root], after_release[root], bounded_main[root]};
        for (unsigned n = 0; n < mutexes.size(); ++n) {
          auto own = mutexes[n].intervals.find(root);
          if (own != mutexes[n].intervals.end()) {
            if (held(own->second, &i, dt)) phase.held.insert(n);
            if (own->second.lock && dt.dominates(own->second.lock, &i)) phase.post_lock.insert(n);
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
  explicit Impl(Context& c)
      : ctx(c), source_facts(source_program_facts(c)),
        originals(source_facts.memory_calls_allocations().statements),
        functions(source_facts.functions()), globals(source_facts.globals()) {
    if (!build()) phases.clear();
  }
  unsigned direction(const Phase& a, const Phase& b) const {
    // In join mode only a real target access makes its successful birth conditional.
    if (!joins.empty()) {
      if (a.root == "main") return 0;
      for (unsigned n : a.held) if (b.bounded_main.count(n)) return 2 * n + 2;
      return 0;
    }
    // A: the parent's access precedes the child's own acquisition.
    if (a.root == "main")
      for (unsigned n : a.held)
        if ((b.parent_held.count(n) && b.post_lock.count(n)) ||
            b.after_release.count(n)) return 2 * n + 1;
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
    unsigned n = (proof - 1) / 2;
    const auto& frame = ctx.pthread_initialization_frame();
    const bool recursive = frame && frame->recursive_object(globals.at(mutexes[n].source));
    const char* contract = recursive ? "closed-recursive-mutex-creation-v1" : "closed-normal-mutex-creation-v1";
    if (proof % 2 == 0 && !joins.empty() && y->bounded_main.count(n))
      return {{"safe_reason", "a joined child or fully drained descendant stays inside main's mutex interval"},
              {"locked_creation_contract", contract},
              {"mutex", mutexes[n].source->getName().str()},
              {"relation", "other-holder-outside-parent-joined-subtree"},
              {"holder_root", x->root}, {"bounded_root", y->root}};
    return {{"safe_reason", "actual outer mutex intervals and creation chains order accesses"},
            {"locked_creation_contract", contract},
            {"mutex", mutexes[(proof - 1) / 2].source->getName().str()},
            {"relation", proof % 2 ? "parent-held-before-descendant-acquisition-chain" : "other-holder-before-child-created-under-retained-lock"},
            {"before_root", x->root}, {"after_root", y->root}};
  }
};
LockedCreation::LockedCreation(Context& c) : _impl(std::make_unique< Impl >(c)) {}
LockedCreation::~LockedCreation() = default;
bool LockedCreation::ordered(const AccessRecord& a, const AccessRecord& b) const { return _impl->ordered(a, b); }
JsonDict LockedCreation::info(const AccessRecord& a, const AccessRecord& b) const { return _impl->info(a, b); }
} // namespace analyzer
} // namespace ikos
