// Copyright and license: see data_race.hpp.
// Included by thread_lifecycle.cpp after its small source-IR helpers.
namespace {
// Separate from both opaque handle values and the serial control interpreter.
// This certificate proves conditional completion at actual returning joins.
class ScalarThreadCompletion {
  Context& ctx;
  SourceProgramFacts& facts;
  llvm::Function* main = nullptr;
  ar::Function* imported_main = nullptr;
  struct Child {
    llvm::AllocaInst* slot;
    ar::Value* storage;
    llvm::Function* entry;
    ar::Function* imported_entry;
    llvm::CallBase* create;
    ar::CallBase* imported_create;
    llvm::LoadInst* load = nullptr;
    ar::Load* imported_load = nullptr;
    llvm::CallBase* join = nullptr;
    ar::CallBase* imported_join = nullptr;
  };
  std::vector< Child > children;
  std::map< llvm::Function*, std::set< llvm::Function* > > owners;
  std::set< llvm::Function* > roots;
  std::set< llvm::CallBase* > creates, joins;
  std::map< std::pair< ar::Statement*, ar::Statement* >, bool > dominance;
  bool attempted = false, valid = false;
  unsigned budget = 131072;
  void need(bool yes) const { if (!yes) throw Refuse{}; }
  void spend() { need(budget != 0); --budget; }
  ar::Statement* original(llvm::Value* v) const {
    const auto& all = facts.memory_calls_allocations().statements;
    auto i = all.find(v); return i == all.end() ? nullptr : i->second;
  }
  ar::CallBase* binding(llvm::CallBase* c) const {
    return source_call_binding(c, original(c));
  }
  // Paths include every successor, with no numerical/guard feasibility premise.
  bool reachable(ar::Code* code, ar::BasicBlock* target,
                 ar::BasicBlock* blocked = nullptr) {
    std::vector< ar::BasicBlock* > todo{code->entry_block()};
    std::set< ar::BasicBlock* > seen;
    while (!todo.empty()) {
      spend(); auto* block = todo.back(); todo.pop_back();
      if (block == blocked || !seen.insert(block).second) continue;
      if (block == target) return true;
      for (auto i = block->successor_begin(); i != block->successor_end(); ++i)
        todo.push_back(*i);
    }
    return false;
  }
  bool imported_dominates(ar::Statement* before, ar::Statement* after) {
    if (!before || !after || before == after || before->code() != after->code())
      return false;
    auto key = std::make_pair(before, after);
    auto cached = dominance.find(key);
    if (cached != dominance.end()) return cached->second;
    bool result = false;
    if (reachable(after->code(), after->parent())) {
      if (before->parent() == after->parent()) {
        bool passed = false;
        for (auto* s : *after->parent()) {
          spend();
          if (s == after) { result = passed; break; }
          if (s == before) passed = true;
        }
      } else {
        result = !reachable(after->code(), after->parent(), before->parent());
      }
    }
    dominance.emplace(key, result); return result;
  }
  bool dominates(llvm::Instruction* before, llvm::Instruction* after) {
    if (!before || !after || before == after || before->getFunction() != main ||
        after->getFunction() != main) return false;
    return facts.dominators(main).dominates(before, after) &&
           imported_dominates(original(before), original(after));
  }
  bool representation(llvm::Type* n, ar::Type* a) const {
    if (n->isVoidTy()) return a->is_void();
    if (n->isIntegerTy()) {
      auto* t = dyn_cast< ar::IntegerType >(a);
      return t && t->bit_width() == n->getIntegerBitWidth();
    }
    auto* p = llvm::dyn_cast< llvm::PointerType >(n);
    return p && !p->getAddressSpace() && a->is_pointer();
  }
  bool closed_leaf(llvm::CallBase* c, ar::CallBase* a,
                   llvm::Function* role) const {
    auto* f = callee(c);
    auto* t = a ? native_handle_callee(a) : nullptr;
    if (!f || !f->isDeclaration() || !a || !t || !t->is_declaration() ||
        (!((f->getName() == "__VERIFIER_nondet_int" ||
            f->getName() == "__VERIFIER_nondet_uint") && c->arg_empty() &&
           f->getReturnType()->isIntegerTy(32)) &&
          !(c->getType()->isVoidTy() && native_handle_range_call(a)) && f->isVarArg()) ||
        !representation(c->getType(), t->type()->return_type()))
      return false;
    for (unsigned n = 0; n < c->arg_size(); ++n)
      if (!representation(c->getArgOperand(n)->getType(), a->argument(n)->type()))
        return false;
    const auto name = f->getName();
    const auto id = t->intrinsic_id();
    // These exact library effects cannot create/join/cancel a thread, invoke an
    // application callback, change stack lifetime, or acquire a parent lock.
    struct Leaf { const char* name; ar::Intrinsic::ID id; };
    static const Leaf leaves[] = {
      {"malloc", ar::Intrinsic::LibcMalloc},
      {"calloc", ar::Intrinsic::LibcCalloc},
      {"realloc", ar::Intrinsic::LibcRealloc},
      {"free", ar::Intrinsic::LibcFree},
      {"abs", ar::Intrinsic::LibcAbs},
      {"abort", ar::Intrinsic::LibcAbort},
      {"exit", ar::Intrinsic::LibcExit},
      {"strlen", ar::Intrinsic::LibcStrlen},
      {"strnlen", ar::Intrinsic::LibcStrnlen},
      {"strcpy", ar::Intrinsic::LibcStrcpy},
      {"strncpy", ar::Intrinsic::LibcStrncpy},
      {"strcat", ar::Intrinsic::LibcStrcat},
      {"strncat", ar::Intrinsic::LibcStrncat},
      {"strcmp", ar::Intrinsic::LibcStrcmp},
      {"strncmp", ar::Intrinsic::LibcStrncmp},
      {"memcpy", ar::Intrinsic::MemoryCopy},
      {"memmove", ar::Intrinsic::MemoryMove},
      {"memset", ar::Intrinsic::MemorySet}
    };
    for (const auto& leaf : leaves)
      if (name == leaf.name && id == leaf.id) return true;
    if (name.startswith("llvm.memcpy.") && id == ar::Intrinsic::MemoryCopy)
      return true;
    if (name.startswith("llvm.memmove.") && id == ar::Intrinsic::MemoryMove)
      return true;
    if (name.startswith("llvm.memset.") && id == ar::Intrinsic::MemorySet)
      return true;
    if (constant_assertion_effect(a)) return true;
    // Legacy verifier nondet declarations retain their source function identity.
    if ((name == "__VERIFIER_nondet_int" || name == "__VERIFIER_nondet_uint") &&
        c->arg_empty() && c->getType()->isIntegerTy(32) && t->has_frontend() &&
        t->frontend< llvm::Value >() == f)
      return true;
    if (name == "__VERIFIER_nondet_bool" && native_handle_range_call(a)) return true;
    if (id == ar::Intrinsic::IkosNonDet && c->arg_empty() &&
        (name.startswith("__VERIFIER_nondet_") ||
         (c->getMetadata("ikos.freeze_uninit") &&
          (name == "__ikos_nondet_int" || name == "__ikos_nondet_int64"))) &&
        c->getType()->isIntegerTy() && c->getType()->getIntegerBitWidth() <= 64)
      return true;
    if ((name == "__VERIFIER_assume" || name == "__ikos_assume") &&
        id == ar::Intrinsic::IkosAssume && c->arg_size() == 1 &&
        c->getArgOperand(0)->getType()->isIntegerTy()) return true;
    if (name == "__VERIFIER_atomic_begin" || name == "__VERIFIER_atomic_end")
      return role != main && c->arg_empty() && c->getType()->isVoidTy() &&
             t->has_frontend() && t->frontend< llvm::Value >() == f;
    // No parent lock/wait exists anywhere in the main call graph. Worker locks
    // may block forever; completion is conditional on ordinary join returning.
    const bool init = name == "pthread_mutex_init";
    const bool destroy = name == "pthread_mutex_destroy";
    const bool lock = name == "pthread_mutex_lock";
    const bool unlock = name == "pthread_mutex_unlock";
    if (!init && !destroy && !lock && !unlock) return false;
    if (role == main && (lock || unlock)) return false;
    if (!c->getType()->isIntegerTy(32) || c->arg_size() != (init ? 2u : 1u) ||
        !c->getArgOperand(0)->getType()->isPointerTy() ||
        (init && !llvm::isa< llvm::ConstantPointerNull >(c->getArgOperand(1))))
      return false;
    if (t->has_frontend()) return t->frontend< llvm::Value >() == f;
    return (lock && id == ar::Intrinsic::PthreadMutexLock) ||
           (unlock && id == ar::Intrinsic::PthreadMutexUnlock);
  }
  void mark(llvm::Function* f, llvm::Function* role,
            std::set< llvm::Function* >& active) {
    spend();
    need(f && facts.definitions().count(f) && !f->isVarArg() &&
         f->getCallingConv() == llvm::CallingConv::C &&
         ordinary_source_function_abi_attributes(f) && active.size() < 16 &&
         active.insert(f).second);
    owners[f].insert(role);
    for (auto& b : *f) for (auto& i : b) {
      spend();
      if (auto* c = llvm::dyn_cast< llvm::CallBase >(&i)) {
        auto* a = binding(c); auto* t = callee(c);
        need(a && t);
        need(!c->hasFnAttr(llvm::Attribute::ReturnsTwice) &&
             !t->hasFnAttribute(llvm::Attribute::ReturnsTwice));
        // Name-based synchronization models may also apply to definitions.
        if (role == main)
          need(t->getName() != "__VERIFIER_atomic_begin" &&
               t->getName() != "__VERIFIER_atomic_end" &&
               t->getName() != "pthread_mutex_lock" &&
               t->getName() != "pthread_mutex_unlock");
        if (creates.count(c) || joins.count(c)) { need(f == main); continue; }
        if (t->isDeclaration()) need(closed_leaf(c, a, role));
        else { need(!roots.count(t)); mark(t, role, active); }
      } else {
        need(!llvm::isa< llvm::IndirectBrInst >(i) &&
             !llvm::isa< llvm::CallBrInst >(i) &&
             !llvm::isa< llvm::LandingPadInst >(i) &&
             !llvm::isa< llvm::ResumeInst >(i));
        if (llvm::isa< llvm::LoadInst >(i) || llvm::isa< llvm::StoreInst >(i) ||
            llvm::isa< llvm::AllocaInst >(i)) need(original(&i) != nullptr);
      }
    }
    active.erase(f);
  }
  void prove() {
    need(ctx.opts.enable_thread_modular &&
         ctx.opts.procedural != Procedural::Intraprocedural &&
         ctx.opts.entry_points.size() == 1 &&
         ctx.opts.entry_points.front()->name() == "main" &&
         ctx.opts.entry_points.front()->num_parameters() == 0 &&
         ctx.opts.entry_points.front()->has_frontend() &&
         ctx.opts.no_init_globals.empty() && !ctx.concurrent_env.is_dirty() &&
         ctx.concurrent_env.unmodeled_effects().empty() &&
         ctx.concurrent_env.unmodeled_thread_creations().empty() &&
         !ctx.bundle->global_or_null("ar.global_ctors") &&
         !ctx.bundle->global_or_null("ar.global_dtors"));
    configure_native_thread_handles(ctx);
    need(ctx.native_handles_only && !ctx.native_handle_accesses.empty());
    imported_main = ctx.opts.entry_points.front();
    main = imported_main->frontend< llvm::Function >();
    auto& module = *main->getParent();
    const auto abi = pthread_object_layout(ctx.bundle, &module);
    need(abi && main->use_empty() && module.getModuleInlineAsm().empty() &&
         !module.getNamedGlobal("llvm.global_ctors") &&
         !module.getNamedGlobal("llvm.global_dtors") &&
         module.getDataLayout().getIndexSizeInBits(0) == abi.word * 8 &&
         mutex_effect_pointer_representations_closed(module));
    need(facts.index(budget + 1) &&
         facts.memory_calls_allocations().unique &&
         facts.memory_calls_allocations().complete);
    need(facts.statement_count() < budget); budget -= facts.statement_count();
    roots.insert(main);
    std::set< ar::Value* > cells;
    // Inventory provides closed storage, never successful-create/handle bits.
    for (const auto& item : ctx.native_handle_accesses) {
      need(item.second.shape == NativeHandleShape::Scalar);
      auto* a = dyn_cast< ar::CallBase >(item.first);
      if (!a) continue;
      auto* c = a->has_frontend()
          ? llvm::dyn_cast< llvm::CallBase >(a->frontend< llvm::Value >()) : nullptr;
      auto* f = c ? native_handle_api(c, true) : nullptr;
      auto* entry = c && c->arg_size() == 4 ? llvm::dyn_cast< llvm::Function >(
          c->getArgOperand(2)->stripPointerCasts()) : nullptr;
      auto* target = a->num_arguments() == 4
          ? dyn_cast< ar::FunctionPointerConstant >(a->argument(2)) : nullptr;
      auto* slot = c && c->arg_size() == 4 ? llvm::dyn_cast< llvm::AllocaInst >(
          c->getArgOperand(0)->stripPointerCasts()) : nullptr;
      auto* allocation = slot ? dyn_cast_or_null< ar::Allocate >(original(slot)) : nullptr;
      need(c && f && binding(c) == a && c->getFunction() == main &&
           a->code() == imported_main->body() && slot && allocation &&
           unique_allocation_definition(allocation) &&
           allocation->result() == item.second.storage &&
           native_handle_scalar_acyclic(c, a) && entry && entry != main &&
           target && target->function()->has_frontend() &&
           target->function()->frontend< llvm::Value >() == entry &&
           cells.insert(item.second.storage).second &&
           dominates(slot, c));
      roots.insert(entry);
      creates.insert(c);
      children.push_back({slot, item.second.storage, entry, target->function(), c, a});
    }
    need(!children.empty() && children.size() <= 32);
    for (auto& child : children) {
      for (auto* a : ctx.native_handle_joins) {
        auto* c = a->has_frontend()
            ? llvm::dyn_cast< llvm::CallBase >(a->frontend< llvm::Value >()) : nullptr;
        auto* load = c && c->arg_size() == 2
            ? llvm::dyn_cast< llvm::LoadInst >(c->getArgOperand(0)) : nullptr;
        if (!load || load->getPointerOperand() != child.slot) continue;
        auto* imported_load = dyn_cast_or_null< ar::Load >(original(load));
        need(!child.join && native_handle_api(c, false) && binding(c) == a &&
             c->getFunction() == main && load->getFunction() == main &&
             a->code() == imported_main->body() && imported_load &&
             imported_load->operand() == child.storage && load->hasOneUse() &&
             a->argument(0) == imported_load->result() &&
             isa< ar::NullConstant >(a->argument(1)) &&
             native_handle_scalar_acyclic(c, a) &&
             dominates(child.create, load) && dominates(load, c));
        child.load = load; child.imported_load = imported_load;
        child.join = c; child.imported_join = a; joins.insert(c);
      }
      need(child.join != nullptr);
      for (const auto& item : ctx.native_handle_accesses)
        if (item.second.storage == child.storage)
          if (auto* l = dyn_cast< ar::Load >(item.first)) need(l == child.imported_load);
      // Imported frontend seeds must precede creation on every path as well.
      for (auto* b : *imported_main->body()) for (auto* s : *b)
        if (auto* store = dyn_cast< ar::Store >(s))
          if (store->pointer() == child.storage)
            need(store->has_frontend() &&
                 dominates(llvm::dyn_cast< llvm::Instruction >(
                     store->frontend< llvm::Value >()), child.create));
    }
    need(joins.size() == ctx.native_handle_joins.size());
    // Close every source thread API independently of imported storage metadata.
    for (auto& f : module) for (auto& b : f) for (auto& i : b)
      if (auto* c = llvm::dyn_cast< llvm::CallBase >(&i)) {
        spend(); auto* t = callee(c); need(t != nullptr);
        if (t->getName() == "pthread_create") need(creates.count(c));
        if (t->getName() == "pthread_join") need(joins.count(c));
        if (t->getName().startswith("pthread_") &&
            !t->getName().startswith("pthread_mutex_") &&
            t->getName() != "pthread_create" && t->getName() != "pthread_join")
          throw Refuse{};
      }
    for (auto* role : roots) { std::set< llvm::Function* > active; mark(role, role, active); }
    // No callback escape or synchronous extra activation may share an entry.
    for (const auto& item : owners)
      for (auto* u : item.first->users()) {
        auto* c = llvm::dyn_cast< llvm::CallBase >(u);
        need(c && binding(c));
        if (roots.count(item.first)) {
          need(creates.count(c));
          auto* entry = llvm::dyn_cast< llvm::Function >(
              c->getArgOperand(2)->stripPointerCasts());
          need(entry == item.first);
        } else need(callee(c) == item.first && owners.count(c->getFunction()));
      }
    for (auto* f : ctx.concurrent_env.get_all_thread_functions()) {
      need(f && f->has_frontend());
      auto* n = f->frontend< llvm::Function >(); need(roots.count(n));
      std::set< std::uint64_t > covered;
      for (const auto& child : children) if (child.imported_entry == f)
        covered.insert(ctx.concurrent_env.site_id(child.imported_create));
      for (auto sid : ctx.concurrent_env.get_func_instances(f->name())) need(covered.count(sid));
    }
    valid = true;
  }
  llvm::Function* root(const AccessRecord& r) const {
    for (auto* f : roots) if (f->getName() == r.thread_id) return f;
    return nullptr;
  }
  llvm::Instruction* endpoint(const AccessRecord& r) const {
    auto* i = r.stmt->has_frontend() ? llvm::dyn_cast< llvm::Instruction >(
        r.stmt->frontend< llvm::Value >()) : nullptr;
    auto* role = root(r);
    if (!i || !role || original(i) != r.stmt || !owners.count(i->getFunction()) ||
        !owners.at(i->getFunction()).count(role)) return nullptr;
    if (auto* call = llvm::dyn_cast< llvm::CallBase >(i))
      return binding(call) ? i : nullptr; // Includes actual implicit libc effects.
    auto* l = llvm::dyn_cast< llvm::LoadInst >(i);
    auto* s = llvm::dyn_cast< llvm::StoreInst >(i);
    auto* t = l ? l->getType() : s ? s->getValueOperand()->getType() : nullptr;
    if (!t || (l ? r.kind != AccessKind::Read : r.kind != AccessKind::Write) ||
        r.width != main->getParent()->getDataLayout().getTypeStoreSize(t).getFixedValue())
      return nullptr;
    return i;
  }
public:
  explicit ScalarThreadCompletion(Context& c) : ctx(c), facts(source_program_facts(c)) {}
  bool excludes(const AccessRecord& a, const AccessRecord& b) {
    if (std::getenv("IKOS_RACE_DISABLE_SCALAR_COMPLETION")) return false;
    if (!attempted) {
      attempted = true;
      try { prove(); } catch (const Refuse&) { valid = false; }
      catch (const std::out_of_range&) { valid = false; }
    }
    if (!valid) return false;
    try {
      auto* x = endpoint(a); auto* y = endpoint(b);
      auto* ra = root(a); auto* rb = root(b);
      if (!x || !y || !ra || !rb || (ra == main && rb == main)) return false;
      // Access records may merge an entry's instances. Certify every actual
      // family, never extrapolate a first family's join to the whole entry.
      if (ra == main || rb == main) {
        auto* access = ra == main ? x : y;
        auto* entry = ra == main ? rb : ra;
        if (access->getFunction() != main) return false;
        unsigned covered = 0;
        for (const auto& c : children) if (c.entry == entry) {
          ++covered;
          if (!dominates(c.join, access) && !dominates(access, c.create)) return false;
        }
        return covered != 0;
      }
      unsigned pairs = 0;
      for (const auto& ca : children) if (ca.entry == ra)
        for (const auto& cb : children) if (cb.entry == rb) {
          if (&ca == &cb) continue; // Both accesses belong to one actual thread.
          ++pairs;
          if (!dominates(ca.join, cb.create) && !dominates(cb.join, ca.create))
            return false;
        }
      return pairs != 0;
    } catch (const Refuse&) { valid = false; return false; }
  }
  JsonDict info(const AccessRecord& a, const AccessRecord& b) const {
    auto label = [](llvm::Instruction* at) {
      unsigned position = 0;
      for (auto& block : *at->getFunction()) for (auto& instruction : block) {
        if (&instruction == at)
          return at->getFunction()->getName().str() + "#" + std::to_string(position);
        ++position;
      }
      ikos_unreachable("scalar completion occurrence missing from function");
    };
    JsonList families;
    for (const auto& c : children)
      families.add(JsonDict{{"parent", "main"}, {"entry", c.entry->getName().str()},
                            {"full_width_slot", c.slot->getName().str()},
                            {"create", label(c.create)}, {"load", label(c.load)},
                            {"join", label(c.join)},
                            {"generation", "one acyclic actual Create and one consuming Join"}});
    return {{"thread_lifecycle_contract", "independent-scalar-thread-completion-v1"},
            {"families", families},
            {"implicit_call_endpoint",
             bool(llvm::dyn_cast_or_null< llvm::CallBase >(endpoint(a))) ||
             bool(llvm::dyn_cast_or_null< llvm::CallBase >(endpoint(b)))},
            {"safe_reason", "source-exact returning current-child join precedes access or next creation"},
            {"thread_a", a.thread_id}, {"thread_b", b.thread_id},
            {"scope", "native and imported MUST control; GNU ordinary join; no-child/output-only failure envelope; opaque handle values unchanged"}};
  }
};
} // namespace
