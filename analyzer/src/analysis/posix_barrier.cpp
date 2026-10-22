// Copyright and license: see context.hpp.
#include <ikos/analyzer/analysis/posix_barrier.hpp>
#include <ikos/analyzer/analysis/source_call_binding.hpp>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <functional>
#include <set>

namespace ikos {
namespace analyzer {
class PosixBarrier::Impl {
  Context& ctx;
  unsigned budget = 32768, participants = 1, bits = 0;
  llvm::Function* main = nullptr;
  std::map< llvm::Value*, ar::Statement* > originals;
  std::map< llvm::Function*, ar::Function* > functions;
  std::map< llvm::GlobalVariable*, ar::GlobalVariable* > globals;
  std::set< llvm::Function* > roots;
  std::set< llvm::CallBase* > creates;
  struct Barrier {
    llvm::GlobalVariable* source;
    unsigned capacity;
    std::map< llvm::Function*, llvm::CallBase* > waits;
  };
  std::vector< Barrier > barriers;
  struct Phase {
    std::string root;
    std::set< unsigned > before, after;
  };
  std::map< ar::Statement*, Phase > phases;
  bool step() { if (!budget) return false; --budget; return true; }
  llvm::Function* callee(llvm::CallBase* c) const {
    return llvm::dyn_cast< llvm::Function >(c->getCalledOperand()->stripPointerCasts());
  }
  ar::CallBase* binding(llvm::CallBase* c) const {
    auto it = originals.find(c);
    auto* a = it == originals.end() ? nullptr : source_call_binding(c, it->second);
    auto* f = callee(c);
    if (!a || !f) return nullptr;
    auto ordinary = [](llvm::AttributeSet attributes) {
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
    };
    if (!ordinary(c->getAttributes().getRetAttrs()) || !ordinary(f->getAttributes().getRetAttrs())) return nullptr;
    for (unsigned n = 0; n < c->arg_size(); ++n)
      if (!ordinary(c->getAttributes().getParamAttrs(n)) || !ordinary(f->getAttributes().getParamAttrs(n))) return nullptr;
    return a;
  }
  bool create(llvm::CallBase* c) {
    auto* a = binding(c); auto* f = callee(c);
    auto* target = a ? native_handle_callee(a) : nullptr;
    if (!a || !target || !target->has_frontend() || target->frontend< llvm::Value >() != f ||
        target->name() != "pthread_create" || !target->type()->return_type()->is_integer() ||
        cast< ar::IntegerType >(target->type()->return_type())->bit_width() != 32 ||
        !f || !f->isDeclaration() || f->getName() != "pthread_create" ||
        c->arg_size() != 4 || !c->getType()->isIntegerTy(32) || !c->use_empty() ||
        c->getFunction() != main || !isa< ar::NullConstant >(a->argument(1)) ||
        !isa< ar::NullConstant >(a->argument(3)) ||
        !llvm::isa< llvm::ConstantPointerNull >(c->getArgOperand(1)) ||
        !llvm::isa< llvm::ConstantPointerNull >(c->getArgOperand(3))) return false;
    auto* out = llvm::dyn_cast< llvm::PointerType >(c->getArgOperand(0)->getType());
    auto* slot = llvm::dyn_cast< llvm::AllocaInst >(c->getArgOperand(0));
    auto* entry = llvm::dyn_cast< llvm::Function >(c->getArgOperand(2));
    auto* actual = dyn_cast< ar::FunctionPointerConstant >(a->argument(2));
    if (!out || out->getAddressSpace() || !out->getPointerElementType()->isIntegerTy(bits) ||
        !slot || slot->getAllocatedType() != out->getPointerElementType() ||
        !entry || !functions.count(entry) || !functions.at(entry)->is_definition() ||
        !actual || actual->function() != functions.at(entry) || entry == main ||
        entry->isVarArg() || entry->arg_size() != 1 || !entry->getArg(0)->use_empty() ||
        entry->getCallingConv() != llvm::CallingConv::C ||
        !entry->getReturnType()->isPointerTy() ||
        !entry->getArg(0)->getType()->isPointerTy() ||
        llvm::cast< llvm::PointerType >(entry->getReturnType())->getAddressSpace() ||
        llvm::cast< llvm::PointerType >(entry->getArg(0)->getType())->getAddressSpace()) return false;
    auto it = originals.find(slot);
    auto* allocation = it == originals.end() ? nullptr : dyn_cast< ar::Allocate >(it->second);
    auto* count = allocation ? dyn_cast< ar::IntegerConstant >(allocation->array_size()) : nullptr;
    auto* native_count = llvm::dyn_cast< llvm::ConstantInt >(slot->getArraySize());
    auto* integer = allocation ? dyn_cast< ar::IntegerType >(allocation->allocated_type()) : nullptr;
    if (!allocation || a->argument(0) != allocation->result() || !integer || integer->bit_width() != bits ||
        !count || count->value().to_z_number() != 1 || !native_count || !native_count->isOne() ||
        slot->getAlign().value() < bits / 8) return false;
    roots.insert(entry); creates.insert(c); ++participants; return true;
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
  bool object(llvm::GlobalVariable* g) {
    auto* type = llvm::dyn_cast< llvm::StructType >(g->getValueType());
    const unsigned width = bits == 32 ? 20 : 32, align = bits / 8;
    auto& layout = main->getParent()->getDataLayout();
    if (!type || type->isOpaque() || !type->hasName() || type->getName() != "union.pthread_barrier_t" ||
        !globals.count(g) || !globals.at(g)->is_definition() || globals.at(g)->is_thread_local() ||
        g->isConstant() || g->isThreadLocal() || g->isExternallyInitialized() || g->getAddressSpace() ||
        !g->hasInitializer() || !g->getInitializer()->isNullValue() ||
        layout.getTypeAllocSize(type) != width ||
        (g->getAlign() ? g->getAlign()->value() : layout.getABITypeAlign(type).value()) < align ||
        ctx.bundle->data_layout().alloc_size_in_bytes(globals.at(g)->type()->pointee()) != width) return false;
    llvm::CallBase* init = nullptr;
    Barrier barrier{g, 0, {}};
    std::map< ar::CallBase*, ar::GlobalVariable* > pending;
    std::vector< llvm::Value* > todo{g}; std::set< llvm::Value* > seen;
    while (!todo.empty()) {
      auto* v = todo.back(); todo.pop_back();
      if (!step()) return false;
      if (!seen.insert(v).second) continue;
      for (auto* u : v->users()) {
        if (llvm::isa< llvm::BitCastOperator >(u)) { todo.push_back(u); continue; }
        auto* c = llvm::dyn_cast< llvm::CallBase >(u);
        auto* a = c ? binding(c) : nullptr; auto* f = c ? callee(c) : nullptr;
        auto* target = a ? native_handle_callee(a) : nullptr;
        if (!a || !target || !target->has_frontend() || target->frontend< llvm::Value >() != f ||
            target->name() != f->getName().str() || !target->type()->return_type()->is_integer() ||
            cast< ar::IntegerType >(target->type()->return_type())->bit_width() != 32 ||
            !f->isDeclaration() || !roots.count(c->getFunction()) ||
            !c->getType()->isIntegerTy(32) || c->arg_empty() || c->getArgOperand(0) != v ||
            !exact_handle_slot(a->argument(0), globals.at(g)) ||
            f->getFunctionType()->getParamType(0) != g->getType()) return false;
        if (f->getName() == "pthread_barrier_init") {
          if (init || c->arg_size() != 3 || c->getFunction() != main ||
              c->getParent() != &main->getEntryBlock() ||
              !llvm::isa< llvm::ConstantPointerNull >(c->getArgOperand(1)) ||
              !isa< ar::NullConstant >(a->argument(1))) return false;
          auto* n = llvm::dyn_cast< llvm::ConstantInt >(c->getArgOperand(2));
          auto* k = dyn_cast< ar::IntegerConstant >(a->argument(2));
          if (!n || !n->getType()->isIntegerTy(32) || !k || k->value().bit_width() != 32 ||
              n->getZExtValue() == 0 || n->getZExtValue() >= 2147483647 ||
              k->value().to_z_number() != core::ZNumber(n->getZExtValue())) return false;
          init = c; barrier.capacity = n->getZExtValue();
        } else if (f->getName() == "pthread_barrier_wait" && c->arg_size() == 1) {
          if (!barrier.waits.emplace(c->getFunction(), c).second) return false;
        } else return false;
        pending.emplace(a, globals.at(g));
      }
    }
    if (!init || barrier.waits.empty()) return false;
    llvm::DominatorTree dt(*main);
    for (auto* c : creates) if (!dt.dominates(init, c)) return false;
    for (const auto& wait : barrier.waits)
      if (wait.first == main && !dt.dominates(init, wait.second)) return false;
    effects.insert(pending.begin(), pending.end()); barriers.push_back(std::move(barrier)); return true;
  }
  // ponytail: literal capacities and direct DAG roots; add checked snapshots
  // and contextual phases before admitting loops or helpers.
  bool build() {
    if (!ctx.opts.enable_thread_modular || ctx.opts.entry_points.size() != 1 ||
        ctx.opts.entry_points.front()->name() != "main" ||
        !ctx.opts.entry_points.front()->has_frontend() || !ctx.opts.no_init_globals.empty() ||
        ctx.bundle->global_or_null("ar.global_ctors") || ctx.bundle->global_or_null("ar.global_dtors")) return false;
    main = ctx.opts.entry_points.front()->frontend< llvm::Function >();
    auto& module = *main->getParent();
    if (main->isDeclaration() || main->isVarArg() || !main->getReturnType()->isIntegerTy(32) ||
        main->getCallingConv() != llvm::CallingConv::C ||
        (main->arg_size() != 0 && main->arg_size() != 2)) return false;
    if (main->arg_size() == 2) {
      auto* argv = llvm::dyn_cast< llvm::PointerType >(main->getArg(1)->getType());
      auto* element = argv ? llvm::dyn_cast< llvm::PointerType >(argv->getPointerElementType()) : nullptr;
      if (!main->getArg(0)->getType()->isIntegerTy(32) || !argv || argv->getAddressSpace() ||
          !element || element->getAddressSpace() || !element->getPointerElementType()->isIntegerTy(8)) return false;
    }
    bool potential = false;
    for (auto& g : module.globals()) {
      auto* t = llvm::dyn_cast< llvm::StructType >(g.getValueType());
      potential |= t && t->hasName() && t->getName() == "union.pthread_barrier_t";
    }
    if (!potential) return false;
    bits = ctx.bundle->data_layout().pointers.bit_width;
    llvm::Triple triple(module.getTargetTriple());
    if (!main->use_empty() || (bits != 32 && bits != 64) ||
        module.getDataLayout().getPointerSizeInBits() != bits ||
        (bits == 32 ? triple.getArch() != llvm::Triple::x86 : triple.getArch() != llvm::Triple::x86_64) ||
        !triple.isOSLinux() || triple.getEnvironment() != llvm::Triple::GNU ||
        !module.alias_empty() || !module.getModuleInlineAsm().empty() ||
        !mutex_effect_pointer_representations_closed(module)) return false;
    for (auto it = ctx.bundle->global_begin(); it != ctx.bundle->global_end(); ++it)
      if ((*it)->has_frontend()) globals.emplace((*it)->frontend< llvm::GlobalVariable >(), *it);
    for (auto it = ctx.bundle->function_begin(); it != ctx.bundle->function_end(); ++it) {
      auto* f = *it;
      if (f->has_frontend()) functions.emplace(f->frontend< llvm::Function >(), f);
      if (!f->is_definition()) continue;
      for (auto* b : *f->body()) for (auto* s : *b)
        if ((isa< ar::CallBase >(s) || isa< ar::Load >(s) || isa< ar::Store >(s) || isa< ar::Allocate >(s)) &&
            s->has_frontend() && !originals.emplace(s->frontend< llvm::Value >(), s).second) return false;
    }
    roots.insert(main);
    for (auto& b : *main) for (auto& i : b) {
      if (!step()) return false;
      auto* c = llvm::dyn_cast< llvm::CallBase >(&i); auto* f = c ? callee(c) : nullptr;
      if (f && f->getName() == "pthread_create" && !create(c)) return false;
    }
    if (creates.empty()) return false;
    for (auto& g : module.globals()) {
      if (g.isDeclaration() && !g.use_empty()) return false;
      auto* t = llvm::dyn_cast< llvm::StructType >(g.getValueType());
      if (t && t->hasName() && t->getName() == "union.pthread_barrier_t" && !object(&g)) return false;
    }
    if (effects.empty()) return false;
    for (auto& f : module) {
      if (f.isDeclaration() || (&f != main && f.use_empty())) continue;
      if (!roots.count(&f) || !acyclic(&f)) return false;
      for (auto* u : f.users()) {
        auto* c = llvm::dyn_cast< llvm::CallBase >(u);
        if (!c || !creates.count(c) || c->getArgOperand(2) != &f) return false;
      }
      for (auto& b : f) for (auto& i : b) {
        if (!step() || llvm::isa< llvm::InvokeInst >(i) || llvm::isa< llvm::CallBrInst >(i) ||
            llvm::isa< llvm::IndirectBrInst >(i) || llvm::isa< llvm::ResumeInst >(i)) return false;
        auto* c = llvm::dyn_cast< llvm::CallBase >(&i);
        if (!c || llvm::isa< llvm::DbgInfoIntrinsic >(c)) continue;
        auto* a = binding(c); auto* target = callee(c);
        if (!a || !target || !target->isDeclaration()) return false;
        if (effects.count(a) || creates.count(c)) continue;
        // Other effects keep their ordinary consumers. No callbacks, signals,
        // cancellation, nonlocal exits or additional thread creators are closed.
        auto name = target->getName();
        if ((name == "pthread_mutex_lock" || name == "pthread_mutex_unlock") &&
            c->arg_size() == 1 && c->getType()->isIntegerTy(32) &&
            c->getArgOperand(0)->getType()->isPointerTy()) continue;
        if (name == "__VERIFIER_nondet_int" && c->arg_empty() && c->getType()->isIntegerTy(32)) continue;
        if (c->arg_empty() && c->getMetadata("ikos.freeze_uninit") &&
            native_handle_callee(a)->intrinsic_id() == ar::Intrinsic::IkosNonDet &&
            ((name == "__ikos_nondet_int" && c->getType()->isIntegerTy(32)) ||
             (name == "__ikos_nondet_int64" && c->getType()->isIntegerTy(64)))) continue;
        if (name == "abort" && c->arg_empty() && c->getType()->isVoidTy() &&
            native_handle_callee(a)->intrinsic_id() == ar::Intrinsic::LibcAbort) continue;
        return false;
      }
      // No unmatched synthetic AR call may introduce another execution source.
      for (auto* b : *functions.at(&f)->body()) for (auto* s : *b)
        if (auto* c = dyn_cast< ar::CallBase >(s)) {
          if (!c->has_frontend()) return false;
          auto* n = llvm::dyn_cast< llvm::CallBase >(c->frontend< llvm::Value >());
          if (!n || n->getFunction() != &f || binding(n) != c) return false;
        }
    }
    for (auto* root : roots) {
      llvm::DominatorTree dt(*root);
      for (auto& b : *root) for (auto& i : b) {
        if (!llvm::isa< llvm::LoadInst >(i) && !llvm::isa< llvm::StoreInst >(i)) continue;
        auto it = originals.find(&i);
        if (it == originals.end() || (!isa< ar::Load >(it->second) && !isa< ar::Store >(it->second))) continue;
        Phase phase{functions.at(root)->name(), {}, {}};
        for (unsigned n = 0; n < barriers.size(); ++n) {
          const auto& barrier = barriers[n];
          auto wait = barrier.waits.find(root);
          if (barrier.capacity < participants || wait == barrier.waits.end()) continue;
          if (dt.dominates(&i, wait->second)) phase.before.insert(n);
          if (dt.dominates(wait->second, &i)) phase.after.insert(n);
        }
        phases.emplace(it->second, std::move(phase));
      }
    }
    return true;
  }
public:
  std::map< ar::CallBase*, ar::GlobalVariable* > effects;
  explicit Impl(Context& c) : ctx(c) {
    if (!build()) { effects.clear(); phases.clear(); barriers.clear(); }
  }
  bool orders(ar::Statement* a, const std::string& root_a, ar::Statement* b, const std::string& root_b) const {
    auto x = phases.find(a), y = phases.find(b);
    if (x == phases.end() || y == phases.end() || x->second.root != root_a || y->second.root != root_b) return false;
    for (unsigned n : x->second.before) if (y->second.after.count(n)) return true;
    for (unsigned n : y->second.before) if (x->second.after.count(n)) return true;
    return false;
  }
  JsonDict info() const {
    JsonList objects;
    for (const auto& barrier : barriers) {
      JsonList waits;
      for (const auto& wait : barrier.waits) {
        unsigned index = 0;
        for (auto& b : *wait.first) for (auto& i : b) {
          if (&i == wait.second) waits.add(wait.first->getName().str() + "#" + std::to_string(index));
          ++index;
        }
      }
      objects.add(JsonDict{{"object", barrier.source->getName().str()},
                           {"capacity", static_cast< int >(barrier.capacity)}, {"waits", waits}});
    }
    return {{"safe_reason", "all possible participants arrive before any one-shot barrier return"},
            {"barrier_contract", "closed-gnu-one-shot-barrier-v1"},
            {"participant_upper_bound", static_cast< int >(participants)}, {"barriers", objects}};
  }
};
PosixBarrier::PosixBarrier(Context& ctx) : _impl(std::make_unique< Impl >(ctx)) {}
PosixBarrier::~PosixBarrier() = default;
ar::GlobalVariable* PosixBarrier::effect(ar::CallBase* c) const {
  auto it = _impl->effects.find(c); return it == _impl->effects.end() ? nullptr : it->second;
}
bool PosixBarrier::orders(ar::Statement* a, const std::string& root_a, ar::Statement* b, const std::string& root_b) const {
  return _impl->orders(a, root_a, b, root_b);
}
JsonDict PosixBarrier::info() const { return _impl->info(); }
void configure_posix_barrier(Context& ctx) {
  if (!ctx.posix_barrier()) ctx._posix_barrier = std::make_shared< PosixBarrier >(ctx);
}
} // namespace analyzer
} // namespace ikos
