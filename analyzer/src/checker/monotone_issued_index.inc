// Copyright and license: see data_race.hpp.
// Included inside BinaryTokenOwnership::Impl. A consumer of final indexed-family
// facts; never a producer of AI state, handle identities, or successful-create HB.
  struct IssuedIndexCertificate {
    llvm::GlobalVariable *counter = nullptr, *data = nullptr, *mutex = nullptr;
    llvm::Function* worker = nullptr;
    llvm::CallBase* source_allocation = nullptr;
    ar::CallBase *allocation = nullptr, *creation = nullptr;
    IndexedThreadFamily family;
    core::ZNumber maximum{0};
    std::uint64_t stride = 0;
    std::set< llvm::Instruction* > endpoints;
  };
  std::vector< IssuedIndexCertificate > issued_indices;

  // Strip only representation-preserving, uniquely defined adapters.
  ar::Value* issued_uncast(ar::Value* v) const {
    for (unsigned n = 0; n < 32; ++n) {
      auto* iv = dyn_cast_or_null< ar::InternalVariable >(v);
      auto* d = iv ? unique_def(iv) : nullptr;
      ar::Value* next = nullptr;
      if (auto* a = dyn_cast_or_null< ar::Assignment >(d)) next = a->operand();
      else if (auto* u = dyn_cast_or_null< ar::UnaryOperation >(d)) {
        if (u->op() == ar::UnaryOperation::Bitcast) next = u->operand();
      }
      if (!next) return v;
      if (v->type()->is_pointer() != next->type()->is_pointer() ||
          v->type()->is_integer() != next->type()->is_integer() ||
          (v->type()->is_integer() && cast< ar::IntegerType >(v->type())->bit_width() !=
                                        cast< ar::IntegerType >(next->type())->bit_width()) ||
          ctx.bundle->data_layout().store_size_in_bytes(v->type()) !=
              ctx.bundle->data_layout().store_size_in_bytes(next->type())) return nullptr;
      v = next;
    }
    return nullptr;
  }
  ar::Statement* issued_definition(ar::Value* v) const {
    auto* iv = dyn_cast_or_null< ar::InternalVariable >(issued_uncast(v));
    return iv ? unique_def(iv) : nullptr;
  }
  bool issued_reaches(ar::BasicBlock* from, ar::BasicBlock* to,
                      ar::BasicBlock* avoid = nullptr) {
    std::vector< ar::BasicBlock* > todo{from};
    std::set< ar::BasicBlock* > seen;
    while (!todo.empty()) {
      auto* b = todo.back(); todo.pop_back();
      if (b == avoid || !seen.insert(b).second) continue;
      if (!spend() || b == to) return true;
      todo.insert(todo.end(), b->successor_begin(), b->successor_end());
    }
    return false;
  }
  bool issued_before(ar::Statement* a, ar::Statement* b) {
    if (!a || !b || a == b || a->code() != b->code() ||
        !issued_reaches(b->code()->entry_block(), b->parent())) return false;
    if (a->parent() == b->parent()) {
      for (auto* s : *a->parent()) { if (s == a) return true; if (s == b) return false; }
      return false;
    }
    return !issued_reaches(b->code()->entry_block(), b->parent(), a->parent());
  }
  bool issued_once(ar::Statement* s) {
    for (auto i = s->parent()->successor_begin(); i != s->parent()->successor_end(); ++i)
      if (issued_reaches(*i, s->parent())) return false;
    return true;
  }
  bool issued_acyclic(llvm::Function* f) {
    std::set< llvm::BasicBlock* > active, done;
    std::function< bool(llvm::BasicBlock*) > visit = [&](llvm::BasicBlock* b) {
      if (!spend()) return false;
      if (done.count(b)) return true;
      if (!active.insert(b).second) return false;
      for (auto* n : llvm::successors(b)) if (!visit(n)) return false;
      active.erase(b); done.insert(b); return true;
    };
    if (!visit(&f->getEntryBlock())) return false;
    std::set< ar::BasicBlock* > aa, dd;
    std::function< bool(ar::BasicBlock*) > av = [&](ar::BasicBlock* b) {
      if (!spend()) return false;
      if (dd.count(b)) return true;
      if (!aa.insert(b).second) return false;
      for (auto i = b->successor_begin(); i != b->successor_end(); ++i)
        if (!av(*i)) return false;
      aa.erase(b); dd.insert(b); return true;
    };
    return av(functions.at(f)->body()->entry_block());
  }
  // Check actual definitions even for leaves; frontend identity alone is not
  // enough to identify a captured load, malloc result or nondeterministic call.
  bool issued_pointer(ar::Value* a, llvm::Value* n) const {
    if (!a || !n || !a->type()->is_pointer() || !n->getType()->isPointerTy() ||
        n->getType()->getPointerAddressSpace()) return false;
    a = issued_uncast(a); n = n->stripPointerCasts();
    if (!a) return false;
    if (llvm::isa< llvm::ConstantPointerNull >(n)) return isa< ar::NullConstant >(a);
    if (auto* g = llvm::dyn_cast< llvm::GlobalVariable >(n))
      return globals.count(g) && a == globals.at(g);
    if (auto* l = llvm::dyn_cast< llvm::LoadInst >(n)) {
      auto* al = dyn_cast_or_null< ar::Load >(original(l));
      return al && a == al->result() && unique_def(al->result()) == al;
    }
    if (auto* c = llvm::dyn_cast< llvm::CallBase >(n)) {
      auto* ac = binding(c);
      return ac && ac->has_result() && a == ac->result() && unique_def(ac->result()) == ac;
    }
    // GEP semantics are checked separately, at every certified endpoint.
    auto* g = llvm::dyn_cast< llvm::GetElementPtrInst >(n);
    auto* d = issued_definition(a);
    return g && dyn_cast_or_null< ar::PointerShift >(d) && d->has_frontend() &&
           d->frontend< llvm::Value >() == n && d->result_or_null() == a;
  }
  bool issued_expression(ar::Value* a, llvm::Value* n, unsigned depth = 0) const {
    if (depth > 32 || !a || !n || !a->type()->is_integer() || !n->getType()->isIntegerTy() ||
        cast< ar::IntegerType >(a->type())->bit_width() != n->getType()->getIntegerBitWidth()) return false;
    a = issued_uncast(a); if (!a) return false;
    if (auto* c = llvm::dyn_cast< llvm::ConstantInt >(n)) {
      auto* ac = dyn_cast< ar::IntegerConstant >(a);
      return ac && ac->value().to_z_number() == (ac->type()->is_signed()
          ? core::ZNumber(c->getSExtValue()) : core::ZNumber(c->getZExtValue()));
    }
    if (auto* l = llvm::dyn_cast< llvm::LoadInst >(n)) {
      auto* al = dyn_cast_or_null< ar::Load >(original(l));
      return al && a == al->result() && unique_def(al->result()) == al;
    }
    if (auto* c = llvm::dyn_cast< llvm::CallBase >(n)) {
      auto* ac = binding(c);
      return ac && ac->has_result() && a == ac->result() && unique_def(ac->result()) == ac;
    }
    auto* d = issued_definition(a);
    if (!d || !d->has_frontend() || d->frontend< llvm::Value >() != n) return false;
    if (auto* op = llvm::dyn_cast< llvm::BinaryOperator >(n)) {
      auto* b = dyn_cast< ar::BinaryOperation >(d); if (!b) return false;
      const auto code = b->op();
      bool match = (op->getOpcode() == llvm::Instruction::Add &&
                    (code == ar::BinaryOperation::SAdd || code == ar::BinaryOperation::UAdd)) ||
                   (op->getOpcode() == llvm::Instruction::Mul &&
                    (code == ar::BinaryOperation::SMul || code == ar::BinaryOperation::UMul)) ||
                   (op->getOpcode() == llvm::Instruction::Shl &&
                    (code == ar::BinaryOperation::SShl || code == ar::BinaryOperation::UShl));
      return match && issued_expression(b->left(), op->getOperand(0), depth + 1) &&
                      issued_expression(b->right(), op->getOperand(1), depth + 1);
    }
    auto* c = llvm::dyn_cast< llvm::CastInst >(n);
    auto* u = dyn_cast< ar::UnaryOperation >(d);
    return c && u &&
        ((c->getOpcode() == llvm::Instruction::SExt && u->op() == ar::UnaryOperation::SExt) ||
         (c->getOpcode() == llvm::Instruction::ZExt && u->op() == ar::UnaryOperation::ZExt)) &&
        issued_expression(u->operand(), c->getOperand(0), depth + 1);
  }
  bool issued_memory(llvm::Instruction* i) const {
    auto* l = llvm::dyn_cast< llvm::LoadInst >(i);
    auto* s = llvm::dyn_cast< llvm::StoreInst >(i);
    auto* al = dyn_cast_or_null< ar::Load >(original(i));
    auto* as = dyn_cast_or_null< ar::Store >(original(i));
    if ((!l && !s) || (l && (!al || !l->isSimple() || al->is_atomic() || al->is_volatile())) ||
        (s && (!as || !s->isSimple() || as->is_atomic() || as->is_volatile()))) return false;
    auto* nt = l ? l->getType() : s->getValueOperand()->getType();
    ar::Value* av = l ? static_cast< ar::Value* >(al->result()) : as->value();
    llvm::Value* nv = l ? static_cast< llvm::Value* >(l) : s->getValueOperand();
    if ((!nt->isIntegerTy() && !nt->isPointerTy()) || !nt->isSized() ||
        (nt->isIntegerTy() && nt->getIntegerBitWidth() > 64)) return false;
    auto size = i->getModule()->getDataLayout().getTypeStoreSize(nt);
    return !size.isScalable() && core::ZNumber(size.getFixedValue()) ==
        ctx.bundle->data_layout().store_size_in_bytes(av->type()) &&
        issued_pointer(l ? al->operand() : as->pointer(), l ? l->getPointerOperand() : s->getPointerOperand()) &&
        (nt->isPointerTy() ? issued_pointer(av, nv) : issued_expression(av, nv));
  }
  bool issued_malloc(llvm::CallBase* c) const {
    auto* a = binding(c); auto* f = callee(c);
    auto* target = a ? native_handle_callee(a) : nullptr;
    const unsigned bits = ctx.bundle->data_layout().pointers.bit_width;
    return a && f && f->isDeclaration() && f->getName() == "malloc" && target &&
        target->intrinsic_id() == ar::Intrinsic::LibcMalloc && c->arg_size() == 1 &&
        c->getType() == llvm::Type::getInt8PtrTy(c->getContext()) &&
        c->getArgOperand(0)->getType()->isIntegerTy(bits) && a->has_result() &&
        issued_pointer(a->result(), c) && issued_expression(a->argument(0), c->getArgOperand(0));
  }
  bool issued_free(llvm::CallBase* c) const {
    auto* a = binding(c); auto* f = callee(c); auto* target = a ? native_handle_callee(a) : nullptr;
    return a && f && f->isDeclaration() && f->getName() == "free" && target &&
        target->intrinsic_id() == ar::Intrinsic::LibcFree && c->arg_size() == 1 &&
        c->getType()->isVoidTy() && issued_pointer(a->argument(0), c->getArgOperand(0));
  }
  // All uses of a source pointer and its actual AR counterpart are closed.
  // Only full pointer bitcasts/copies are transparent; GEP leaves are explicit.
  bool issued_uses(llvm::Value* root, ar::Value* imported,
                   const std::set< llvm::Instruction* >& leaves,
                   const std::set< ar::Statement* >& actual) {
    std::vector< llvm::Value* > todo{root}; std::set< llvm::Value* > seen;
    while (!todo.empty()) {
      auto* v = todo.back(); todo.pop_back();
      if (!spend()) return false;
      if (!seen.insert(v).second) continue;
      for (auto* u : v->users()) {
        if (auto* bit = llvm::dyn_cast< llvm::BitCastOperator >(u)) {
          if (!bit->getType()->isPointerTy() || bit->getType()->getPointerAddressSpace()) return false;
          todo.push_back(u); continue;
        }
        auto* i = llvm::dyn_cast< llvm::Instruction >(u);
        if (!i || !leaves.count(i)) return false;
      }
    }
    std::set< ar::Value* > aliases{imported}; bool changed = true;
    auto codes = [&]() {
      std::vector< ar::Code* > out;
      for (auto fi = ctx.bundle->function_begin(); fi != ctx.bundle->function_end(); ++fi)
        if ((*fi)->is_definition()) out.push_back((*fi)->body());
      for (auto gi = ctx.bundle->global_begin(); gi != ctx.bundle->global_end(); ++gi)
        if ((*gi)->is_definition()) out.push_back((*gi)->initializer());
      return out;
    }();
    while (changed) {
      changed = false;
      for (auto* code : codes) for (auto* b : *code) for (auto* s : *b) {
        if (!spend()) return false;
        ar::Value* operand = nullptr;
        if (auto* a = dyn_cast< ar::Assignment >(s)) operand = a->operand();
        else if (auto* u = dyn_cast< ar::UnaryOperation >(s))
          if (u->op() == ar::UnaryOperation::Bitcast) operand = u->operand();
        auto* result = s->result_or_null();
        if (operand && aliases.count(operand)) {
          if (!result || !result->type()->is_pointer() ||
              issued_uncast(result) != issued_uncast(imported)) return false;
          changed |= aliases.insert(result).second;
        }
      }
    }
    for (auto* code : codes) for (auto* b : *code) for (auto* s : *b) {
      if (!spend()) return false;
      for (auto op = s->op_begin(); op != s->op_end(); ++op)
        if (aliases.count(*op) && !actual.count(s) && !aliases.count(s->result_or_null())) return false;
    }
    return true;
  }

  bool issued_worker(IssuedIndexCertificate& p, llvm::Function* worker) {
    if (!functions.count(worker) || !issued_acyclic(worker)) return false;
    p.worker = worker;
    llvm::CallBase *lock = nullptr, *unlock = nullptr;
    llvm::StoreInst* update = nullptr;
    std::set< llvm::Instruction* > reads;
    llvm::DominatorTree dom(*worker);
    for (auto& b : *worker) for (auto& i : b) {
      if (!spend()) return false;
      if (auto* c = llvm::dyn_cast< llvm::CallBase >(&i)) {
        if (llvm::isa< llvm::DbgInfoIntrinsic >(c)) continue;
        auto m = mutex_call(c);
        if (m.source) {
          if (!globals.count(m.source) || m.object != globals.at(m.source)) return false;
          if (p.mutex && p.mutex != m.source) return false;
          p.mutex = m.source; auto*& slot = m.lock ? lock : unlock;
          if (slot) return false;
          slot = c;
        } else {
          auto* target = callee(c); auto* a = binding(c);
          if (!a || !target || !target->isDeclaration() ||
              !(unused_freeze(c) || terminal(c) ||
                (target->getName() == "__VERIFIER_nondet_int" && c->arg_empty() &&
                 c->getType()->isIntegerTy(32) && a->has_result() &&
                 issued_expression(a->result(), c)))) return false;
        }
      } else if (!llvm::isa< llvm::LoadInst >(i) && !llvm::isa< llvm::StoreInst >(i) &&
                 !llvm::isa< llvm::CastInst >(i) && !llvm::isa< llvm::BinaryOperator >(i) &&
                 !llvm::isa< llvm::ICmpInst >(i) && !llvm::isa< llvm::GetElementPtrInst >(i) &&
                 !llvm::isa< llvm::BranchInst >(i) && !llvm::isa< llvm::ReturnInst >(i) &&
                 !llvm::isa< llvm::UnreachableInst >(i)) return false;
    }
    if (!lock || !unlock || !globals.count(p.mutex) || !dom.dominates(lock, unlock) ||
        !issued_before(binding(lock), binding(unlock))) return false;
    std::set< ar::Statement* > mutex_uses{binding(lock), binding(unlock)};
    for (auto* b : *globals.at(p.mutex)->initializer()) for (auto* s : *b) mutex_uses.insert(s);
    if (!issued_uses(p.mutex, globals.at(p.mutex), {lock, unlock}, mutex_uses)) return false;
    for (auto& b : *worker) for (auto& i : b) {
      auto* st = llvm::dyn_cast< llvm::StoreInst >(&i);
      auto* g = st ? llvm::dyn_cast< llvm::GlobalVariable >(st->getPointerOperand()) : nullptr;
      if (!g || !g->getValueType()->isIntegerTy(32)) continue;
      auto* op = llvm::dyn_cast< llvm::BinaryOperator >(st->getValueOperand());
      auto* old = op ? llvm::dyn_cast< llvm::LoadInst >(op->getOperand(0)) : nullptr;
      auto* one = op ? llvm::dyn_cast< llvm::ConstantInt >(op->getOperand(1)) : nullptr;
      if (update || !op || op->getOpcode() != llvm::Instruction::Add || !old ||
          old->getPointerOperand() != g || !one || !one->isOne() || !token_object(g) ||
          !issued_memory(st) || !issued_memory(old) || !dom.dominates(lock, old) ||
          !dom.dominates(old, op) || !dom.dominates(op, st) || !dom.dominates(st, unlock)) return false;
      auto* as = cast< ar::Store >(original(st));
      auto* add = dyn_cast_or_null< ar::BinaryOperation >(issued_definition(as->value()));
      auto* ct = dyn_cast< ar::IntegerType >(globals.at(g)->type()->pointee());
      if (!ct || !ct->is_signed() || ct->bit_width() != 32 || !add ||
          add->op() != ar::BinaryOperation::SAdd ||
          !issued_before(original(old), add) || !issued_before(add, as) ||
          !issued_before(as, binding(unlock))) return false;
      update = st; p.counter = g;
    }
    if (!update) return false;
    std::set< ar::Statement* > counter_uses{original(update)};
    for (auto* b : *globals.at(p.counter)->initializer()) for (auto* s : *b) counter_uses.insert(s);
    for (auto* u : p.counter->users()) {
      if (u == update) continue;
      auto* l = llvm::dyn_cast< llvm::LoadInst >(u);
      if (!l || l->getPointerOperand() != p.counter || l->getFunction() != worker ||
          !issued_memory(l) || !dom.dominates(lock, l) || !dom.dominates(l, update) ||
          !issued_before(binding(lock), original(l)) || !issued_before(original(l), original(update))) return false;
      auto* t = dyn_cast< ar::IntegerType >(cast< ar::Load >(original(l))->result()->type());
      if (!t || !t->is_signed() || t->bit_width() != 32) return false;
      reads.insert(l); counter_uses.insert(original(l));
    }
    auto counter_leaves = reads; counter_leaves.insert(update);
    if (reads.empty() || !issued_uses(p.counter, globals.at(p.counter), counter_leaves, counter_uses)) return false;
    const unsigned bits = ctx.bundle->data_layout().pointers.bit_width;
    std::set< llvm::LoadInst* > bases;
    std::map< llvm::LoadInst*, std::set< llvm::Instruction* > > base_geps;
    std::map< llvm::GetElementPtrInst*, std::set< llvm::Instruction* > > gep_endpoints;
    for (auto& b : *worker) for (auto& i : b) {
      if (auto* ret = llvm::dyn_cast< llvm::ReturnInst >(&i))
        if (!ret->getReturnValue() || !llvm::isa< llvm::ConstantPointerNull >(ret->getReturnValue()) ||
            !dom.dominates(unlock, ret)) return false;
      auto* l = llvm::dyn_cast< llvm::LoadInst >(&i);
      auto* st = llvm::dyn_cast< llvm::StoreInst >(&i);
      if (!l && !st) continue;
      auto* pointer = l ? l->getPointerOperand() : st->getPointerOperand();
      if (pointer == p.counter) continue;
      if (l && llvm::isa< llvm::GlobalVariable >(pointer) && l->getType()->isPointerTy()) {
        bases.insert(l); continue;
      }
      auto* gep = llvm::dyn_cast< llvm::GetElementPtrInst >(pointer);
      auto* base = gep ? llvm::dyn_cast< llvm::LoadInst >(gep->getPointerOperand()) : nullptr;
      auto* data = base ? llvm::dyn_cast< llvm::GlobalVariable >(base->getPointerOperand()) : nullptr;
      auto* type = l ? l->getType() : st->getValueOperand()->getType();
      if (!gep || !base || !data || !issued_memory(&i) || !issued_memory(base) ||
          gep->getAddressSpace() || gep->getNumIndices() != 1 ||
          !(type->isIntegerTy(32) || type->isIntegerTy(64)) || gep->getSourceElementType() != type ||
          !dom.dominates(unlock, &i) || !issued_before(binding(unlock), original(&i))) return false;
      llvm::Value* index = gep->getOperand(1);
      if (!index->getType()->isIntegerTy(bits)) return false;
      if (bits == 64) {
        auto* ext = llvm::dyn_cast< llvm::CastInst >(index);
        if (!ext || (ext->getOpcode() != llvm::Instruction::SExt && ext->getOpcode() != llvm::Instruction::ZExt) ||
            !ext->getOperand(0)->getType()->isIntegerTy(32)) return false;
        index = ext->getOperand(0);
      }
      auto* capture = llvm::dyn_cast< llvm::Instruction >(index);
      if (!capture || !reads.count(capture)) return false;
      auto stride = i.getModule()->getDataLayout().getTypeAllocSize(type);
      if (stride.isScalable() || (p.data && p.data != data) ||
          (p.stride && p.stride != stride.getFixedValue())) return false;
      p.data = data; p.stride = stride.getFixedValue();
      ar::Value* address = l ? cast< ar::Load >(original(l))->operand() : cast< ar::Store >(original(st))->pointer();
      auto* shift = dyn_cast_or_null< ar::PointerShift >(issued_definition(address));
      if (!shift || shift->num_terms() != 1 || shift->term(0).first.bit_width() != bits ||
          shift->term(0).first.to_z_number() != core::ZNumber(p.stride) ||
          !issued_pointer(shift->pointer(), base) ||
          !issued_expression(shift->term(0).second, gep->getOperand(1)) ||
          !issued_before(original(base), shift) || !issued_before(original(capture), shift) ||
          !issued_before(shift, original(&i))) return false;
      base_geps[base].insert(gep); gep_endpoints[gep].insert(&i); p.endpoints.insert(&i);
    }
    if (p.endpoints.empty() || !p.data || !p.stride || bases.size() != base_geps.size()) return false;
    for (const auto& entry : base_geps) {
      std::set< ar::Statement* > actual;
      for (auto* g : entry.second) {
        auto* endpoint = *gep_endpoints.at(llvm::cast< llvm::GetElementPtrInst >(g)).begin();
        auto* s = original(endpoint);
        actual.insert(issued_definition(isa< ar::Load >(s) ? cast< ar::Load >(s)->operand() : cast< ar::Store >(s)->pointer()));
      }
      if (!issued_uses(entry.first, cast< ar::Load >(original(entry.first))->result(), entry.second, actual)) return false;
    }
    for (const auto& entry : gep_endpoints) {
      std::set< ar::Statement* > actual;
      for (auto* i : entry.second) actual.insert(original(i));
      auto* first = original(*entry.second.begin());
      auto* pointer = isa< ar::Load >(first) ? cast< ar::Load >(first)->operand() : cast< ar::Store >(first)->pointer();
      if (!issued_uses(entry.first, issued_uncast(pointer), entry.second, actual)) return false;
    }
    // Actual AR dependencies must dominate their consumers independently of
    // the source CFG. This also rejects fabricated/multiply-defined captures.
    for (auto* b : *functions.at(worker)->body()) for (auto* s : *b) {
      if (!spend()) return false;
      if (auto* ret = dyn_cast< ar::ReturnValue >(s)) {
        if (!ret->has_operand() || !isa< ar::NullConstant >(ret->operand()) ||
            !issued_before(binding(unlock), ret)) return false;
      } else if (!isa< ar::Assignment >(s) && !isa< ar::UnaryOperation >(s) &&
                 !isa< ar::BinaryOperation >(s) && !isa< ar::Comparison >(s) &&
                 !isa< ar::PointerShift >(s) && !isa< ar::Load >(s) && !isa< ar::Store >(s) &&
                 !isa< ar::CallBase >(s) && !isa< ar::Unreachable >(s)) return false;
      for (auto it = s->op_begin(); it != s->op_end(); ++it)
        if (auto* v = dyn_cast< ar::InternalVariable >(*it))
          if (!unique_def(v) || !issued_before(unique_def(v), s)) return false;
    }
    return true;
  }

  bool issued_dependencies(ar::Value* value, ar::Statement* use, unsigned depth = 0) {
    if (depth > 64 || !spend()) return false;
    auto* v = dyn_cast< ar::InternalVariable >(value);
    if (!v) return true;
    auto* d = unique_def(v);
    if (!d || !issued_before(d, use)) return false;
    for (auto i = d->op_begin(); i != d->op_end(); ++i)
      if (!issued_dependencies(*i, d, depth + 1)) return false;
    return true;
  }
  bool issued_payload(IssuedIndexCertificate& p, llvm::CallBase* creation) {
    auto* data = p.data; auto* main = creation->getFunction();
    if (!globals.count(data) || !data->hasInitializer() || !data->getInitializer()->isNullValue() ||
        data->isConstant() || data->isThreadLocal() || data->isExternallyInitialized() || data->getAddressSpace()) return false;
    auto* ag = globals.at(data);
    if (!ag->is_definition() || ag->is_thread_local() || !ag->type()->pointee()->is_pointer()) return false;
    std::set< ar::Statement* > data_uses;
    for (auto* b : *ag->initializer()) for (auto* s : *b) {
      auto* st = dyn_cast< ar::Store >(s);
      if (!st || st->pointer() != ag || !isa< ar::NullConstant >(st->value())) return false;
      data_uses.insert(s);
    }
    if (data_uses.size() != 1) return false;
    llvm::StoreInst* publication = nullptr;
    std::set< llvm::Instruction* > data_leaves;
    std::vector< llvm::LoadInst* > final_loads;
    for (auto* u : data->users()) {
      auto* l = llvm::dyn_cast< llvm::LoadInst >(u);
      auto* st = llvm::dyn_cast< llvm::StoreInst >(u);
      if (l && l->getPointerOperand() == data && issued_memory(l)) {
        if (l->getFunction() == main) final_loads.push_back(l);
        else if (l->getFunction() != p.worker) return false;
        data_leaves.insert(l); data_uses.insert(original(l)); continue;
      }
      if (!st || st->getPointerOperand() != data || publication || st->getFunction() != main || !issued_memory(st)) return false;
      publication = st; data_leaves.insert(st); data_uses.insert(original(st));
    }
    if (!publication || !issued_uses(data, ag, data_leaves, data_uses)) return false;
    auto* allocation = llvm::dyn_cast< llvm::CallBase >(publication->getValueOperand()->stripPointerCasts());
    auto* handles = p.family.source_allocation;
    if (!allocation || !handles || allocation == handles || allocation->getFunction() != main ||
        handles->getFunction() != main || !issued_malloc(allocation) || !issued_malloc(handles) ||
        binding(handles) != p.family.allocation || binding(allocation) == p.family.allocation) return false;
    p.source_allocation = allocation; p.allocation = binding(allocation);
    llvm::DominatorTree dom(*main); llvm::LoopInfo loops(dom);
    for (auto* i : std::vector< llvm::Instruction* >{allocation, publication})
      if (loops.getLoopFor(i->getParent()) || !issued_once(original(i))) return false;
    if (!dom.dominates(allocation, publication) || !dom.dominates(publication, creation) ||
        !issued_before(p.allocation, original(publication)) || !issued_before(original(publication), p.creation)) return false;
    const unsigned bits = ctx.bundle->data_layout().pointers.bit_width;
    if (p.maximum < 0 || p.maximum > (core::ZNumber(1) << 31) - 1 ||
        p.maximum * p.stride > (core::ZNumber(1) << bits) - 1) return false;
    auto* size = allocation->getArgOperand(0);
    auto* op = llvm::dyn_cast< llvm::BinaryOperator >(size);
    llvm::Value* n = nullptr; ar::Value* an = nullptr;
    if (op) {
      auto* coefficient = llvm::dyn_cast< llvm::ConstantInt >(op->getOperand(1));
      if (!coefficient ||
          !((op->getOpcode() == llvm::Instruction::Mul && coefficient->getZExtValue() == p.stride) ||
            (op->getOpcode() == llvm::Instruction::Shl && coefficient->getZExtValue() < 63 &&
             (std::uint64_t(1) << coefficient->getZExtValue()) == p.stride))) return false;
      auto* product = dyn_cast_or_null< ar::BinaryOperation >(issued_definition(p.allocation->argument(0)));
      if (!product || !dom.dominates(handles, op) || !dom.dominates(op, allocation) ||
          !issued_before(p.family.allocation, product) || !issued_before(product, p.allocation)) return false;
      if ((op->hasNoSignedWrap() ||
           ((product->op() == ar::BinaryOperation::SMul || product->op() == ar::BinaryOperation::SShl) &&
            product->has_no_wrap())) && p.maximum * p.stride > (core::ZNumber(1) << (bits - 1)) - 1) return false;
      // The family's cap was observed before H's multiplication, hence before
      // this D multiplication. Later-only guards cannot repair an earlier wrap.
      n = op->getOperand(0); an = product->left();
    } else {
      auto* c = llvm::dyn_cast< llvm::ConstantInt >(size);
      auto* nc = llvm::dyn_cast< llvm::ConstantInt >(p.family.source_count);
      if (!c || !nc || nc->isNegative() || core::ZNumber(c->getZExtValue()) !=
          core::ZNumber(nc->getZExtValue()) * p.stride ||
          !dom.dominates(handles, allocation) || !issued_before(p.family.allocation, p.allocation)) return false;
      n = p.family.source_count; an = p.family.count;
    }
    if (n != p.family.source_count) {
      auto* ext = llvm::dyn_cast< llvm::CastInst >(n);
      auto* ae = dyn_cast_or_null< ar::UnaryOperation >(issued_definition(an));
      if (bits != 64 || !ext || !ae || ext->getOperand(0) != p.family.source_count ||
          !ext->getOperand(0)->getType()->isIntegerTy(32) ||
          !((ext->getOpcode() == llvm::Instruction::SExt && ae->op() == ar::UnaryOperation::SExt) ||
            (ext->getOpcode() == llvm::Instruction::ZExt && ae->op() == ar::UnaryOperation::ZExt))) return false;
      an = ae->operand();
    }
    if (issued_uncast(an) != issued_uncast(p.family.count) ||
        !issued_dependencies(p.allocation->argument(0), p.allocation) ||
        !issued_dependencies(cast< ar::Store >(original(publication))->value(), original(publication))) return false;
    std::set< llvm::Instruction* > allocation_uses{publication};
    std::set< ar::Statement* > actual_uses{original(publication)};
    bool guarded = false;
    for (auto& b : *main) {
      auto* br = llvm::dyn_cast< llvm::BranchInst >(b.getTerminator());
      auto* cmp = br && br->isConditional() ? llvm::dyn_cast< llvm::ICmpInst >(br->getCondition()) : nullptr;
      if (!cmp || !cmp->isEquality()) continue;
      bool match = (cmp->getOperand(0)->stripPointerCasts() == allocation &&
                    llvm::isa< llvm::ConstantPointerNull >(cmp->getOperand(1))) ||
                   (cmp->getOperand(1)->stripPointerCasts() == allocation &&
                    llvm::isa< llvm::ConstantPointerNull >(cmp->getOperand(0)));
      if (!match) continue;
      const unsigned nonnull = cmp->getPredicate() == llvm::CmpInst::ICMP_NE ? 0 : 1;
      bool source_guard = dom.dominates(llvm::BasicBlockEdge(&b, br->getSuccessor(nonnull)), publication->getParent());
      allocation_uses.insert(cmp);
      for (auto* ab : *functions.at(main)->body()) for (auto* s : *ab) {
        auto* ac = dyn_cast< ar::Comparison >(s);
        if (!ac || !ac->has_frontend() || ac->frontend< llvm::Value >() != cmp) continue;
        if (ac->predicate() != ar::Comparison::PEQ && ac->predicate() != ar::Comparison::PNE) return false;
        bool exact = (issued_pointer(ac->left(), allocation) && isa< ar::NullConstant >(ac->right())) ||
                     (issued_pointer(ac->right(), allocation) && isa< ar::NullConstant >(ac->left()));
        if (!exact) return false;
        actual_uses.insert(ac);
        guarded |= source_guard && ac->predicate() == ar::Comparison::PNE &&
                   issued_before(p.allocation, ac) && issued_before(ac, original(publication));
      }
    }
    if (!guarded) return false;
    llvm::CallBase *data_free = nullptr, *handle_free = nullptr;
    for (auto& b : *main) for (auto& i : b) {
      auto* c = llvm::dyn_cast< llvm::CallBase >(&i); if (!c) continue;
      auto* target = callee(c); if (!target) return false;
      if (target->getName() == "malloc" && c != allocation && c != handles) return false;
      if (target->getName() != "free") continue;
      if (!issued_free(c) || loops.getLoopFor(c->getParent()) || !issued_once(binding(c)) ||
          !lifecycle->completes_before(p.creation, functions.at(p.worker), binding(c),
                                      ctx.call_context_factory->get_empty())) return false;
      auto* root = c->getArgOperand(0)->stripPointerCasts();
      auto* base_load = llvm::dyn_cast< llvm::LoadInst >(root);
      if (root == handles) { if (handle_free) return false; handle_free = c; }
      else if (root == allocation || (base_load && base_load->getPointerOperand() == data &&
                                     std::find(final_loads.begin(), final_loads.end(), base_load) != final_loads.end())) {
        if (data_free) return false;
        data_free = c;
        if (root == allocation) { allocation_uses.insert(c); actual_uses.insert(binding(c)); }
      } else return false;
      if (!issued_dependencies(binding(c)->argument(0), binding(c))) return false;
    }
    if (!data_free && !final_loads.empty()) return false;
    for (auto* l : final_loads)
      if (!dom.dominates(publication, l) || !issued_before(original(publication), original(l)) ||
          !dom.dominates(l, data_free) || !issued_before(original(l), binding(data_free)) ||
          !issued_uses(l, cast< ar::Load >(original(l))->result(), {data_free}, {binding(data_free)})) return false;
    return issued_uses(allocation, p.allocation->result(), allocation_uses, actual_uses);
  }

  bool issued_frame(llvm::Function* main) {
    auto* module = main->getParent();
    if (!module->getModuleInlineAsm().empty() || !module->alias_empty() ||
        !mutex_effect_pointer_representations_closed(*module)) return false;
    for (auto fi = ctx.bundle->function_begin(); fi != ctx.bundle->function_end(); ++fi) {
      auto* f = *fi;
      if (f->is_definition() && (!f->has_frontend() ||
          !functions.count(f->frontend< llvm::Function >()) ||
          functions.at(f->frontend< llvm::Function >()) != f)) return false;
    }
    for (const auto& entry : functions) {
      for (auto& b : *entry.first) for (auto& i : b) {
        if (!spend()) return false;
        if (llvm::isa< llvm::LoadInst >(i) || llvm::isa< llvm::StoreInst >(i)) {
          if (!issued_memory(&i)) return false;
        } else if (auto* c = llvm::dyn_cast< llvm::CallBase >(&i)) {
          if (!llvm::isa< llvm::DbgInfoIntrinsic >(c) && !binding(c)) return false;
        } else if (auto* c = llvm::dyn_cast< llvm::CastInst >(&i)) {
          if ((c->getSrcTy()->isPointerTy() || c->getDestTy()->isPointerTy()) &&
              (c->getOpcode() != llvm::Instruction::BitCast || !c->getSrcTy()->isPointerTy() ||
               !c->getDestTy()->isPointerTy() || c->getSrcTy()->getPointerAddressSpace() ||
               c->getDestTy()->getPointerAddressSpace())) return false;
        } else if (llvm::isa< llvm::AtomicRMWInst >(i) || llvm::isa< llvm::AtomicCmpXchgInst >(i) ||
                   llvm::isa< llvm::InvokeInst >(i) || llvm::isa< llvm::CallBrInst >(i) ||
                   llvm::isa< llvm::IndirectBrInst >(i)) return false;
      }
      for (auto* b : *entry.second->body()) for (auto* s : *b) {
        if (!spend()) return false;
        if (isa< ar::Load >(s) || isa< ar::Store >(s) || isa< ar::CallBase >(s)) {
          if (!s->has_frontend() || original(s->frontend< llvm::Value >()) != s) return false;
          auto* native = llvm::dyn_cast< llvm::Instruction >(s->frontend< llvm::Value >());
          if (!native || native->getFunction() != entry.first) return false;
        }
      }
    }
    return true;
  }
  void prepare_issued_indices(llvm::Function* main, const std::vector< AccessRecord >& records) {
    if (!lifecycle || std::getenv("IKOS_DISABLE_MONOTONE_ISSUED_INDEX")) return;
    const unsigned bits = ctx.bundle->data_layout().pointers.bit_width;
    const auto& layout = main->getParent()->getDataLayout();
    if ((bits != 32 && bits != 64) || layout.getPointerSizeInBits(0) != bits ||
        layout.getIndexSizeInBits(0) != bits || records.empty()) return;
    const unsigned saved = budget; budget = 131072;
    auto prove = [&]() {
      llvm::CallBase* creation = nullptr;
      for (auto& b : *main) for (auto& i : b)
        if (auto* c = llvm::dyn_cast< llvm::CallBase >(&i))
          if (native_handle_api(c, true)) { if (creation) return; creation = c; }
      if (!creation || !binding(creation)) return;
      auto* worker = llvm::dyn_cast< llvm::Function >(creation->getArgOperand(2)->stripPointerCasts());
      if (!worker || !functions.count(worker)) return;
      IssuedIndexCertificate p; p.creation = binding(creation);
      if (!lifecycle->indexed_family(p.creation, functions.at(worker), p.family)) return;
      p.maximum = p.family.count_upper;
      if (!p.family.source_count || !p.family.count || !p.family.source_allocation || !p.family.allocation ||
          p.family.handle_stride != bits / 8 || !issued_worker(p, worker) ||
          !issued_payload(p, creation) || !issued_frame(main) || !budget) return;
      issued_indices.push_back(std::move(p));
    };
    prove(); budget = saved;
  }
  const IssuedIndexCertificate* issued_index_pair(const AccessRecord& a, const AccessRecord& b) const {
    auto endpoint = [&](const AccessRecord& r, const IssuedIndexCertificate& p) {
      if (!r.stmt || !r.stmt->has_frontend() || !r.call_context || !r.call_context->empty() ||
          r.thread_id != p.worker->getName().str() || !r.pts.is_set() || r.pts.is_empty() ||
          r.width != p.stride || r.is_atomic) return false;
      auto* i = llvm::dyn_cast< llvm::Instruction >(r.stmt->frontend< llvm::Value >());
      if (!i || original(i) != r.stmt || !p.endpoints.count(i) ||
          (llvm::isa< llvm::LoadInst >(i) ? r.kind != AccessKind::Read : r.kind != AccessKind::Write)) return false;
      for (auto* object : r.pts) {
        auto* heap = dyn_cast< DynAllocMemoryLocation >(object);
        if (!heap || heap->call() != p.allocation) return false;
      }
      return true;
    };
    if (!lifecycle || std::getenv("IKOS_DISABLE_MONOTONE_ISSUED_INDEX")) return nullptr;
    for (const auto& p : issued_indices) {
      IndexedThreadFamily now;
      if (!lifecycle->indexed_family(p.creation, functions.at(p.worker), now) ||
          now.source_count != p.family.source_count || now.count != p.family.count ||
          now.source_allocation != p.family.source_allocation || now.allocation != p.family.allocation ||
          now.count_upper != p.family.count_upper || now.handle_stride != p.family.handle_stride) continue;
      // At most N successful activations issue distinct j in [0,N). For two
      // different j,k and 0<=u,v<stride, stride*(j-k)+(u-v) is nonzero and has
      // magnitude < stride*N < 2^bits. Full cells stay disjoint modulo 2^bits.
      // This consumes the closed frame; it supplies no pointer-validity fact.
      if (endpoint(a, p) && endpoint(b, p)) return &p;
    }
    return nullptr;
  }
