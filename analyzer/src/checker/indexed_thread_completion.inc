// Copyright and license: see data_race.hpp.
// Same-owner replay carrier. Preparation is entirely structural: no dirty/clean
// result is cached before the analysis fixed point has completed.
class ThreadLifecycleObservations::Impl {
  using Z = core::ZNumber;
  using Interval = core::machine_int::Interval;
  Context& ctx;
  SourceProgramFacts& facts;
  ar::Function* parent = nullptr;
  ar::Function* child = nullptr;
  llvm::Function* source_parent = nullptr;
  llvm::Function* source_child = nullptr;
  ar::Value* base = nullptr;
  ar::CallBase* allocation = nullptr;
  llvm::CallBase* source_allocation = nullptr;
  llvm::CallBase* source_malloc = nullptr;
  llvm::BinaryOperator* source_extent = nullptr;
  ar::BinaryOperation* extent_product = nullptr;
  std::vector< ar::UnaryOperation* > extent_adapters;
  bool extent_seen = false, effect_frame = false;
  Z extent_count_upper{0};
  struct BoundGuard { ar::InternalVariable* truth; Z upper; };
  std::vector< BoundGuard > bound_guards;
  MemoryLocation* object = nullptr;
  unsigned word = 0, budget = 131072;
  bool prepared = false, valid = true;
  const char* stage = "static preparation";
  struct Loop {
    ar::CallBase* call = nullptr;
    ar::Load* load = nullptr;
    ar::PointerShift* address = nullptr;
    ar::InternalVariable* counter = nullptr;
    ar::Value* bound = nullptr;
    ar::BinaryOperation* step = nullptr;
    ar::BasicBlock *pre = nullptr, *header = nullptr, *body = nullptr, *exit = nullptr;
    llvm::CallBase* source = nullptr;
    llvm::GetElementPtrInst* gep = nullptr;
    llvm::LoadInst* source_load = nullptr;
    llvm::PHINode* phi = nullptr;
    llvm::ICmpInst* source_condition = nullptr;
    llvm::Value* source_bound = nullptr;
    llvm::BinaryOperator* source_step = nullptr;
    llvm::BasicBlock *source_pre = nullptr, *source_exit = nullptr;
    std::vector< ar::UnaryOperation* > adapters;
    std::vector< llvm::CastInst* > source_adapters;
    bool call_seen = false, step_seen = false;
    Z lower{0}, upper{0};
  } create, join;
  void require(bool condition) const { if (!condition) throw Refuse{}; }
  void spend() { require(budget != 0); --budget; }
  ar::Statement* original(llvm::Value* source) const {
    const auto& index = facts.memory_calls_allocations().statements;
    auto found = index.find(source);
    return found == index.end() ? nullptr : found->second;
  }
  ar::CallBase* binding(llvm::CallBase* source) const {
    return source_call_binding(source, original(source));
  }
  bool frozen_seed(llvm::CallBase* call) const {
    auto* imported = binding(call);
    auto* target = callee(call);
    auto* native = imported ? native_handle_callee(imported) : nullptr;
    return call->getMetadata("ikos.freeze_uninit") && call->arg_empty() &&
           call->use_empty() && target && target->isDeclaration() && native &&
           native->intrinsic_id() == ar::Intrinsic::IkosNonDet &&
           ((target->getName() == "__ikos_nondet_int" && call->getType()->isIntegerTy(32)) ||
            (target->getName() == "__ikos_nondet_int64" && call->getType()->isIntegerTy(64)));
  }
  static ar::Value* uncast(ar::Value* value) {
    for (unsigned n = 0; n < 32; ++n) {
      auto* variable = dyn_cast< ar::InternalVariable >(value);
      auto* definition = variable ? unique_def(variable) : nullptr;
      if (auto* copy = dyn_cast_or_null< ar::Assignment >(definition))
        value = copy->operand();
      else if (auto* cast = dyn_cast_or_null< ar::UnaryOperation >(definition)) {
        if (cast->op() != ar::UnaryOperation::Bitcast) return value;
        value = cast->operand();
      } else return value;
    }
    return nullptr;
  }
  static bool pointer_binding(ar::Value* value, llvm::Value* source) {
    if (!source || !source->getType()->isPointerTy() ||
        source->getType()->getPointerAddressSpace() != 0) return false;
    source = source->stripPointerCasts();
    for (unsigned n = 0; n < 32; ++n) {
      auto* variable = dyn_cast_or_null< ar::InternalVariable >(value);
      if (!variable || !variable->type()->is_pointer()) return false;
      if (variable->has_frontend() &&
          variable->frontend< llvm::Value >()->stripPointerCasts() == source) return true;
      auto* definition = unique_def(variable);
      if (auto* copy = dyn_cast_or_null< ar::Assignment >(definition))
        value = copy->operand();
      else if (auto* cast = dyn_cast_or_null< ar::UnaryOperation >(definition)) {
        if (cast->op() != ar::UnaryOperation::Bitcast) return false;
        value = cast->operand();
      } else return false;
    }
    return false;
  }
  static bool reaches(ar::BasicBlock* from, ar::BasicBlock* target,
                      ar::BasicBlock* forbidden = nullptr) {
    std::vector< ar::BasicBlock* > todo{from};
    std::set< ar::BasicBlock* > seen;
    while (!todo.empty()) {
      auto* block = todo.back(); todo.pop_back();
      if (block == forbidden) continue;
      if (block == target || seen.size() >= 8192) return true;
      if (!seen.insert(block).second) continue;
      todo.insert(todo.end(), block->successor_begin(), block->successor_end());
    }
    return false;
  }
  static bool reaches(llvm::BasicBlock* from, llvm::BasicBlock* target) {
    std::vector< llvm::BasicBlock* > todo{from};
    std::set< llvm::BasicBlock* > seen;
    while (!todo.empty()) {
      auto* block = todo.back(); todo.pop_back();
      if (block == target || seen.size() >= 8192) return true;
      if (!seen.insert(block).second) continue;
      auto* terminal = block->getTerminator();
      for (unsigned n = 0; n < terminal->getNumSuccessors(); ++n)
        todo.push_back(terminal->getSuccessor(n));
    }
    return false;
  }
  bool dominates(ar::BasicBlock* a, ar::BasicBlock* b) const {
    return reaches(parent->body()->entry_block(), b) &&
           !reaches(parent->body()->entry_block(), b, a);
  }
  bool same_integer(ar::Value* a, ar::Value* b) const {
    return uncast(a) && uncast(a) == uncast(b);
  }
  void source_loop(Loop& loop) {
    auto* pointer = loop.source == create.source
        ? loop.source->getArgOperand(0) : nullptr;
    if (!pointer) {
      loop.source_load = llvm::dyn_cast< llvm::LoadInst >(loop.source->getArgOperand(0));
      require(loop.source_load && loop.source_load->isSimple() &&
              loop.source_load->getType()->isIntegerTy(word * 8) &&
              loop.source_load->getAlign().value() >= word &&
              loop.source_load->hasOneUse() &&
              loop.source_load->getParent() == loop.source->getParent() &&
              loop.source_load->comesBefore(loop.source));
      pointer = loop.source_load->getPointerOperand();
    }
    loop.gep = llvm::dyn_cast< llvm::GetElementPtrInst >(pointer->stripPointerCasts());
    require(loop.gep && loop.gep->getNumIndices() == 1 &&
            loop.gep->getAddressSpace() == 0 &&
            loop.gep->getSourceElementType()->isIntegerTy(word * 8) &&
            loop.gep->getParent() == loop.source->getParent() &&
            loop.gep->comesBefore(loop.source));
    auto* index = loop.gep->getOperand(1);
    for (unsigned n = 0; n < 32; ++n) {
      auto* conversion = llvm::dyn_cast< llvm::CastInst >(index);
      if (!conversion) break;
      require(conversion->getOpcode() == llvm::Instruction::SExt ||
              conversion->getOpcode() == llvm::Instruction::ZExt);
      require(conversion->getType()->isIntegerTy() &&
              conversion->getOperand(0)->getType()->isIntegerTy());
      loop.source_adapters.push_back(conversion);
      index = conversion->getOperand(0);
    }
    loop.phi = llvm::dyn_cast< llvm::PHINode >(index);
    require(loop.phi && loop.phi->getNumIncomingValues() == 2 &&
            loop.phi->getType()->isIntegerTy() &&
            loop.phi->getType()->getIntegerBitWidth() <= word * 8);
    auto* header = loop.phi->getParent();
    auto* branch = llvm::dyn_cast< llvm::BranchInst >(header->getTerminator());
    auto* condition = branch && branch->isConditional()
        ? llvm::dyn_cast< llvm::ICmpInst >(branch->getCondition()) : nullptr;
    require(condition && condition->getParent() == header &&
            condition->getOperand(0) == loop.phi &&
            (condition->getPredicate() == llvm::ICmpInst::ICMP_SLT ||
             condition->getPredicate() == llvm::ICmpInst::ICMP_ULT) &&
            branch->getSuccessor(0) == loop.source->getParent());
    loop.source_condition = condition;
    loop.source_bound = condition->getOperand(1);
    loop.source_exit = branch->getSuccessor(1);
    require(loop.source_exit->getSinglePredecessor() == header);
    auto* body = loop.source->getParent();
    require(body != header && body->getSinglePredecessor() == header);
    for (unsigned n = 0; n < 2; ++n) {
      auto* zero = llvm::dyn_cast< llvm::ConstantInt >(loop.phi->getIncomingValue(n));
      if (zero && zero->isZero()) {
        require(!loop.source_pre);
        loop.source_pre = loop.phi->getIncomingBlock(n);
      } else {
        require(!loop.source_step);
        loop.source_step = llvm::dyn_cast< llvm::BinaryOperator >(
            loop.phi->getIncomingValue(n));
        require(loop.source_step &&
                loop.source_step->getParent() == loop.phi->getIncomingBlock(n));
      }
    }
    auto* step = loop.source_step;
    auto* one = step ? llvm::dyn_cast< llvm::ConstantInt >(step->getOperand(1)) : nullptr;
    require(loop.source_pre && step && one && one->isOne() &&
            step->getOpcode() == llvm::Instruction::Add && step->getOperand(0) == loop.phi);
    auto* latch = step->getParent();
    auto* pre_branch = llvm::dyn_cast< llvm::BranchInst >(loop.source_pre->getTerminator());
    auto* body_branch = llvm::dyn_cast< llvm::BranchInst >(body->getTerminator());
    auto* latch_branch = llvm::dyn_cast< llvm::BranchInst >(latch->getTerminator());
    require(pre_branch && pre_branch->isUnconditional() &&
            pre_branch->getSuccessor(0) == header && body_branch &&
            body_branch->isUnconditional() && latch_branch &&
            latch_branch->isUnconditional() && latch_branch->getSuccessor(0) == header &&
            loop.source_pre != header && loop.source_pre != body && loop.source_pre != latch);
    if (body == latch) require(loop.source->comesBefore(step));
    else {
      require(body_branch->getSuccessor(0) == latch && latch->getSinglePredecessor() == body);
      for (auto& instruction : *latch)
        require(&instruction == step || &instruction == latch_branch ||
                llvm::isa< llvm::DbgInfoIntrinsic >(instruction));
    }
    require(!reaches(loop.source_exit, header) && !reaches(loop.source_exit, loop.source_pre));
    // The bound is one fixed dynamic value, never an induction phi or reload.
    auto* bound = llvm::dyn_cast< llvm::Instruction >(loop.source_bound);
    require(!bound || (!llvm::isa< llvm::PHINode >(bound) &&
                       !llvm::isa< llvm::LoadInst >(bound) &&
                       facts.dominators(source_parent).dominates(bound, loop.phi)));
  }
  void imported_loop(Loop& loop) {
    loop.call = binding(loop.source);
    auto* target = loop.call ? native_handle_callee(loop.call) : nullptr;
    const bool creating = loop.source == create.source;
    // Native pthread_t signatures can retain their actual declaration rather
    // than become an AR intrinsic; source_call_binding checks that identity.
    require(loop.call && target && target->is_declaration() &&
            (target->has_frontend() || target->intrinsic_id() ==
                (creating ? ar::Intrinsic::PthreadCreate : ar::Intrinsic::PthreadJoin)) &&
            loop.call->code() == parent->body() &&
            isa< ar::NullConstant >(loop.call->argument(1)));
    if (loop.source == create.source) {
      auto* pointer_type = dyn_cast< ar::PointerType >(loop.call->argument(0)->type());
      auto* handle_type = pointer_type ? dyn_cast< ar::IntegerType >(pointer_type->pointee()) : nullptr;
      auto* entry = dyn_cast< ar::FunctionPointerConstant >(loop.call->argument(2));
      require(handle_type && handle_type->bit_width() == word * 8 &&
              entry && entry->function() == child &&
              isa< ar::NullConstant >(loop.call->argument(3)));
    } else {
      auto* handle_type = dyn_cast< ar::IntegerType >(loop.call->argument(0)->type());
      require(handle_type && handle_type->bit_width() == word * 8);
    }
    ar::Value* pointer = loop.call->argument(0);
    if (loop.source_load) {
      loop.load = dyn_cast_or_null< ar::Load >(original(loop.source_load));
      require(loop.load && !loop.load->is_atomic() && !loop.load->is_volatile() &&
              uncast(pointer) == loop.load->result() &&
              cast< ar::IntegerType >(loop.load->result()->type())->bit_width() == word * 8 &&
              loop.load->parent() == loop.call->parent());
      pointer = loop.load->operand();
    }
    auto* variable = dyn_cast_or_null< ar::InternalVariable >(uncast(pointer));
    loop.address = variable ? dyn_cast_or_null< ar::PointerShift >(unique_def(variable)) : nullptr;
    require(loop.address && loop.address->num_terms() == 1 &&
            loop.address->term(0).first.to_z_number() == Z(word) &&
            loop.address->term(0).first.bit_width() == word * 8 &&
            loop.address->term(0).first.sign() == core::Unsigned &&
            loop.address->parent() == loop.call->parent() &&
            pointer_binding(pointer, loop.gep) &&
            pointer_binding(loop.address->pointer(), loop.gep->getPointerOperand()) &&
            source_integer_binding(loop.address->term(0).second, loop.gep->getOperand(1)));
    ar::Value* index = loop.address->term(0).second;
    for (unsigned n = 0; n < 32; ++n) {
      auto* v = dyn_cast< ar::InternalVariable >(index);
      auto* conversion = v ? dyn_cast_or_null< ar::UnaryOperation >(unique_def(v)) : nullptr;
      if (!conversion) break;
      require(conversion->op() == ar::UnaryOperation::SExt ||
              conversion->op() == ar::UnaryOperation::ZExt ||
              conversion->op() == ar::UnaryOperation::Bitcast);
      require(conversion->operand()->type()->is_integer());
      loop.adapters.push_back(conversion);
      index = conversion->operand();
    }
    loop.counter = dyn_cast< ar::InternalVariable >(index);
    require(loop.counter && source_integer_binding(loop.counter, loop.phi));
    loop.body = loop.call->parent();
    require(loop.body->num_predecessors() == 1 && loop.body->num_successors() == 1);
    loop.header = *loop.body->predecessor_begin();
    require(*loop.body->successor_begin() == loop.header &&
            loop.header->num_predecessors() == 2 && loop.header->num_successors() == 2);
    for (auto i = loop.header->predecessor_begin(); i != loop.header->predecessor_end(); ++i)
      if (*i != loop.body) loop.pre = *i;
    for (auto i = loop.header->successor_begin(); i != loop.header->successor_end(); ++i)
      if (*i != loop.body) loop.exit = *i;
    require(loop.pre && loop.exit && loop.pre != loop.exit &&
            loop.exit->num_predecessors() == 1 && *loop.exit->predecessor_begin() == loop.header &&
            loop.pre->num_successors() == 1 && *loop.pre->successor_begin() == loop.header &&
            !reaches(loop.exit, loop.pre) && !reaches(loop.exit, loop.header));
    ar::Assignment *init = nullptr, *update = nullptr;
    for (auto* block : *parent->body()) for (auto* statement : *block) {
      spend();
      if (statement->result_or_null() != loop.counter) continue;
      auto* assignment = dyn_cast< ar::Assignment >(statement);
      require(assignment);
      if (block == loop.pre && !init) init = assignment;
      else if (block == loop.body && !update) update = assignment;
      else throw Refuse{};
    }
    auto* zero = init ? dyn_cast< ar::IntegerConstant >(init->operand()) : nullptr;
    auto* next = update ? dyn_cast< ar::InternalVariable >(update->operand()) : nullptr;
    loop.step = next ? dyn_cast_or_null< ar::BinaryOperation >(unique_def(next)) : nullptr;
    auto* one = loop.step ? dyn_cast< ar::IntegerConstant >(loop.step->right()) : nullptr;
    require(zero && zero->value().is_zero() && loop.pre->back() == init &&
            loop.step && one && one->value().to_z_number() == 1 &&
            loop.step->left() == loop.counter &&
            loop.step->result()->type() == loop.counter->type() &&
            (loop.step->op() == ar::BinaryOperation::SAdd ||
             loop.step->op() == ar::BinaryOperation::UAdd) &&
            loop.step->parent() == loop.body && loop.body->back() == update &&
            loop.step->has_frontend() &&
            loop.step->frontend< llvm::Value >() == loop.source_step);
    const bool is_signed = llvm::cast< llvm::ICmpInst >(
        llvm::cast< llvm::BranchInst >(loop.phi->getParent()->getTerminator())->getCondition())
        ->isSigned();
    bool guard = false, called = false, advanced = false;
    for (auto* statement : *loop.body) {
      spend();
      if (auto* comparison = dyn_cast< ar::Comparison >(statement)) {
        require(!guard && !called &&
                comparison->predicate() == (is_signed ? ar::Comparison::SILT : ar::Comparison::UILT) &&
                source_integer_binding(comparison->left(), loop.phi) &&
                source_integer_binding(comparison->right(), loop.source_bound));
        loop.bound = comparison->right(); guard = true;
      } else if (statement == loop.call) { require(guard && !called); called = true; }
      else if (statement == loop.step) { require(called); advanced = true; }
      else if (statement == update) require(advanced);
      else if (statement == loop.load) require(guard && !called);
      else require(!called && (isa< ar::UnaryOperation >(statement) ||
                               isa< ar::PointerShift >(statement)));
    }
    require(guard && called && advanced);
    auto* exit_guard = dyn_cast< ar::Comparison >(loop.exit->front());
    require(exit_guard &&
            exit_guard->predicate() == (is_signed ? ar::Comparison::SIGE : ar::Comparison::UIGE) &&
            source_integer_binding(exit_guard->left(), loop.phi) &&
            source_integer_binding(exit_guard->right(), loop.source_bound));
    for (auto* statement : *loop.header) require(isa< ar::UnaryOperation >(statement));
    auto* b = dyn_cast_or_null< ar::InternalVariable >(uncast(loop.bound));
    if (b) {
      auto* definition = unique_def(b);
      require(definition && definition->parent() != loop.header &&
              definition->parent() != loop.body && dominates(definition->parent(), loop.pre));
    } else require(isa< ar::IntegerConstant >(loop.bound));
  }

  static bool statement_dominates(ar::Statement* a, ar::Statement* b) {
    if (a->code() != b->code()) return false;
    if (a->parent() == b->parent()) {
      for (auto* statement : *a->parent()) {
        if (statement == a) return true;
        if (statement == b) return false;
      }
      return false;
    }
    return reaches(a->code()->entry_block(), b->parent()) &&
           !reaches(a->code()->entry_block(), b->parent(), a->parent());
  }
  void nonnull_before(llvm::CallBase* native_allocation,
                      llvm::Instruction* native_target,
                      ar::CallBase* imported_allocation,
                      ar::Statement* imported_target) {
    require(native_allocation->getFunction() == native_target->getFunction() &&
            imported_allocation->code() == imported_target->code());
    const auto& dom = facts.dominators(native_allocation->getFunction());
    for (auto& block : *native_allocation->getFunction()) {
      auto* branch = llvm::dyn_cast< llvm::BranchInst >(block.getTerminator());
      auto* cmp = branch && branch->isConditional()
          ? llvm::dyn_cast< llvm::ICmpInst >(branch->getCondition()) : nullptr;
      if (!cmp || !cmp->isEquality() || !dom.dominates(native_allocation, cmp)) continue;
      llvm::Value* pointer = nullptr;
      if (llvm::isa< llvm::ConstantPointerNull >(cmp->getOperand(1))) pointer = cmp->getOperand(0);
      else if (llvm::isa< llvm::ConstantPointerNull >(cmp->getOperand(0))) pointer = cmp->getOperand(1);
      if (!pointer || pointer->stripPointerCasts() != native_allocation) continue;
      auto* safe = branch->getSuccessor(cmp->getPredicate() == llvm::ICmpInst::ICMP_NE ? 0 : 1);
      if (safe->getSinglePredecessor() != &block ||
          !dom.dominates(safe, native_target->getParent())) continue;
      for (auto* ar_block : *imported_target->code()) for (auto* statement : *ar_block) {
        spend();
        auto* guard = dyn_cast< ar::Comparison >(statement);
        if (!guard || guard->predicate() != ar::Comparison::PNE ||
            !guard->has_frontend() || guard->frontend< llvm::Value >() != cmp ||
            !statement_dominates(imported_allocation, guard) ||
            !statement_dominates(guard, imported_target)) continue;
        auto* operand = isa< ar::NullConstant >(guard->right()) ? guard->left()
            : isa< ar::NullConstant >(guard->left()) ? guard->right() : nullptr;
        if (operand && uncast(operand) == imported_allocation->result() &&
            pointer_binding(operand, native_allocation)) return;
      }
    }
    // A singleton allocation-site points-to set does not discharge this gate.
    throw Refuse{};
  }
  void heap_factory() {
    auto* source_base = create.gep->getPointerOperand()->stripPointerCasts();
    require(source_base == join.gep->getPointerOperand()->stripPointerCasts());
    source_allocation = llvm::dyn_cast< llvm::CallBase >(source_base);
    allocation = source_allocation ? binding(source_allocation) : nullptr;
    require(allocation && source_allocation->getFunction() == source_parent &&
            allocation->code() == parent->body() && allocation->has_result() &&
            source_allocation->arg_size() == 1 &&
            source_allocation->getArgOperand(0)->getType()->isIntegerTy(word * 8) &&
            facts.dominators(source_parent).dominates(source_allocation, create.phi) &&
            dominates(allocation->parent(), create.pre));
    base = allocation->result();
    require(unique_def(allocation->result()) == allocation);
    require(uncast(create.address->pointer()) == base &&
            uncast(join.address->pointer()) == base);
    // One actual dynamic definition. The abstract allocation site is only a
    // consistency check; it never substitutes for this source/AR identity.
    for (auto* successor : llvm::successors(source_allocation->getParent()))
      require(!reaches(successor, source_allocation->getParent()));
    for (auto it = allocation->parent()->successor_begin();
         it != allocation->parent()->successor_end(); ++it)
      require(!reaches(*it, allocation->parent()));
    auto* extent = llvm::dyn_cast< llvm::BinaryOperator >(
        source_allocation->getArgOperand(0));
    source_extent = extent;
    auto* scale = extent ? llvm::dyn_cast< llvm::ConstantInt >(extent->getOperand(1)) : nullptr;
    auto* count = extent ? extent->getOperand(0) : nullptr;
    if (auto* cast = llvm::dyn_cast_or_null< llvm::CastInst >(count)) {
      require((cast->getOpcode() == llvm::Instruction::SExt ||
               cast->getOpcode() == llvm::Instruction::ZExt) &&
              cast->getType()->isIntegerTy(word * 8));
      count = cast->getOperand(0);
    }
    require(extent && extent->getOpcode() == llvm::Instruction::Mul &&
            scale && scale->getZExtValue() == word && count == create.source_bound &&
            source_integer_binding(allocation->argument(0), extent));
    auto* ar_extent = dyn_cast_or_null< ar::InternalVariable >(uncast(allocation->argument(0)));
    auto* multiply = ar_extent ? dyn_cast_or_null< ar::BinaryOperation >(unique_def(ar_extent)) : nullptr;
    auto* ar_scale = multiply ? dyn_cast< ar::IntegerConstant >(multiply->right()) : nullptr;
    require(multiply && ar_scale && ar_scale->value().to_z_number() == Z(word) &&
            cast< ar::IntegerType >(multiply->left()->type())->bit_width() == word * 8 &&
            cast< ar::IntegerType >(multiply->right()->type())->bit_width() == word * 8 &&
            cast< ar::IntegerType >(multiply->result()->type())->bit_width() == word * 8 &&
            (multiply->op() == ar::BinaryOperation::SMul ||
             multiply->op() == ar::BinaryOperation::UMul) &&
            multiply->has_frontend() && multiply->frontend< llvm::Value >() == extent &&
            source_integer_binding(multiply->left(), extent->getOperand(0)) &&
            statement_dominates(multiply, allocation));
    extent_product = multiply;
    ar::Value* imported_count = uncast(multiply->left());
    if (imported_count != uncast(create.bound)) {
      auto* value = dyn_cast_or_null< ar::InternalVariable >(imported_count);
      auto* extension = value ? dyn_cast_or_null< ar::UnaryOperation >(unique_def(value)) : nullptr;
      require(extension && (extension->op() == ar::UnaryOperation::SExt ||
                            extension->op() == ar::UnaryOperation::ZExt) &&
              uncast(extension->operand()) == uncast(create.bound) &&
              cast< ar::IntegerType >(extension->result()->type())->bit_width() == word * 8);
    }
    ar::Value* extent_value = multiply->left();
    for (unsigned depth = 0; depth < 32 && extent_value != create.bound; ++depth) {
      auto* variable = dyn_cast< ar::InternalVariable >(extent_value);
      auto* adapter = variable ? dyn_cast_or_null< ar::UnaryOperation >(unique_def(variable)) : nullptr;
      if (!adapter) break;
      require(adapter->op() == ar::UnaryOperation::Bitcast ||
              adapter->op() == ar::UnaryOperation::SExt ||
              adapter->op() == ar::UnaryOperation::ZExt);
      require(adapter->operand()->type()->is_integer());
      extent_adapters.push_back(adapter);
      extent_value = adapter->operand();
    }
    require(same_integer(extent_value, create.bound));
    auto* factory = callee(source_allocation);
    require(factory);
    if (factory->isDeclaration()) {
      source_malloc = source_allocation;
      nonnull_before(source_allocation, create.source, allocation, create.call);
      nonnull_before(source_allocation, join.source, allocation, join.call);
    } else {
      require(factory != source_parent && factory != source_child &&
              factory->arg_size() == 1 && !factory->isVarArg() &&
              factory->getArg(0)->getType()->isIntegerTy(word * 8));
      for (auto* user : factory->users()) require(user == source_allocation);
      for (auto& block : *factory) for (auto& instruction : block) {
        spend();
        if (auto* call = llvm::dyn_cast< llvm::CallBase >(&instruction)) {
          auto* target = callee(call);
          require(target);
          if (frozen_seed(call)) continue;
          if (target->getName() == "malloc") {
            require(!source_malloc && call->arg_size() == 1 &&
                    call->getArgOperand(0) == factory->getArg(0));
            source_malloc = call;
          } else require(target->getName() == "abort" ||
                         llvm::isa< llvm::DbgInfoIntrinsic >(instruction));
        } else require(llvm::isa< llvm::ICmpInst >(instruction) ||
                       llvm::isa< llvm::BranchInst >(instruction) ||
                       llvm::isa< llvm::ReturnInst >(instruction) ||
                       llvm::isa< llvm::UnreachableInst >(instruction));
      }
      require(source_malloc);
      for (auto* user : source_malloc->users()) {
        if (auto* ret = llvm::dyn_cast< llvm::ReturnInst >(user)) {
          require(ret->getReturnValue() == source_malloc);
        } else {
          auto* cmp = llvm::dyn_cast< llvm::ICmpInst >(user);
          require(cmp && cmp->isEquality() && cmp->getOperand(0) == source_malloc &&
                  llvm::isa< llvm::ConstantPointerNull >(cmp->getOperand(1)));
        }
      }
      auto* imported_malloc = binding(source_malloc);
      auto found = facts.functions().find(factory);
      require(imported_malloc && found != facts.functions().end() &&
              found->second->num_parameters() == 1 &&
              imported_malloc->num_arguments() == 1 &&
              imported_malloc->argument(0) == found->second->param(0));
      unsigned returned = 0;
      for (auto* block : *found->second->body()) for (auto* statement : *block) {
        spend();
        if (auto* ret = dyn_cast< ar::ReturnValue >(statement)) {
          require(ret->has_operand() && uncast(ret->operand()) == imported_malloc->result());
          ++returned;
        } else if (auto* call = dyn_cast< ar::CallBase >(statement)) {
          auto* target = native_handle_callee(call);
          auto* source = call->has_frontend()
              ? llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >()) : nullptr;
          require(call == imported_malloc || (target &&
                  target->intrinsic_id() == ar::Intrinsic::LibcAbort) ||
                  (source && frozen_seed(source)));
        } else require(isa< ar::Comparison >(statement) ||
                       isa< ar::Unreachable >(statement));
      }
      require(returned > 0);
      for (auto& block : *factory) if (auto* ret = llvm::dyn_cast< llvm::ReturnInst >(block.getTerminator())) {
        require(ret->getReturnValue() == source_malloc);
        for (auto* ar_block : *found->second->body()) for (auto* statement : *ar_block)
          if (isa< ar::ReturnValue >(statement))
            nonnull_before(source_malloc, ret, imported_malloc, statement);
      }
    }
    auto* imported_malloc = binding(source_malloc);
    auto* target = callee(source_malloc);
    require(imported_malloc && target && target->isDeclaration() &&
            target->getName() == "malloc" && source_malloc->arg_size() == 1 &&
            source_malloc->getArgOperand(0)->getType()->isIntegerTy(word * 8) &&
            native_handle_callee(imported_malloc)->intrinsic_id() == ar::Intrinsic::LibcMalloc);
  }
  bool after_join(llvm::Instruction* instruction) const {
    return instruction->getFunction() == source_parent &&
           facts.dominators(source_parent).dominates(join.source_exit, instruction->getParent());
  }
  bool after_join(ar::Statement* statement) const {
    return statement->code() == parent->body() && dominates(join.exit, statement->parent());
  }
  void close_storage() {
    std::vector< llvm::Value* > pending{source_allocation};
    std::set< llvm::Value* > aliases;
    while (!pending.empty()) {
      auto* pointer = pending.back(); pending.pop_back();
      spend();
      if (!aliases.insert(pointer).second) continue;
      for (auto* user : pointer->users()) {
        spend();
        if (auto* cast = llvm::dyn_cast< llvm::BitCastInst >(user)) {
          require(cast->getFunction() == source_parent &&
                  cast->getType()->isPointerTy() &&
                  cast->getType()->getPointerAddressSpace() == 0);
          pending.push_back(cast);
        } else if (auto* comparison = llvm::dyn_cast< llvm::ICmpInst >(user)) {
          require(comparison->isEquality() && comparison->getFunction() == source_parent &&
                  ((comparison->getOperand(0) == pointer &&
                    llvm::isa< llvm::ConstantPointerNull >(comparison->getOperand(1))) ||
                   (comparison->getOperand(1) == pointer &&
                    llvm::isa< llvm::ConstantPointerNull >(comparison->getOperand(0)))));
        } else if (user == create.gep || user == join.gep) {
          require(llvm::cast< llvm::GetElementPtrInst >(user)->getPointerOperand() == pointer);
        } else {
          auto* call = llvm::dyn_cast< llvm::CallBase >(user);
          auto* target = call ? callee(call) : nullptr;
          auto* imported = call ? binding(call) : nullptr;
          require(target && target->isDeclaration() && target->getName() == "free" &&
                  call->arg_size() == 1 && call->getArgOperand(0) == pointer &&
                  imported && native_handle_callee(imported)->intrinsic_id() == ar::Intrinsic::LibcFree &&
                  after_join(call) && after_join(imported));
        }
      }
    }
    for (const auto* loop : {&create, &join}) {
      for (auto* user : loop->gep->users())
        require(user == (loop->source_load ? static_cast< llvm::User* >(loop->source_load)
                                          : static_cast< llvm::User* >(loop->source)));
    }
    // Independent imported use closure; LLVM identity alone does not authorize
    // an extra AR store/copy/escape on a mutable native handle cell.
    std::set< ar::Value* > ar_aliases{base};
    bool changed = true;
    while (changed) {
      changed = false;
      for (auto* block : *parent->body()) for (auto* statement : *block) {
        spend();
        ar::Value* operand = nullptr;
        if (auto* copy = dyn_cast< ar::Assignment >(statement)) operand = copy->operand();
        if (auto* cast = dyn_cast< ar::UnaryOperation >(statement))
          if (cast->op() == ar::UnaryOperation::Bitcast && cast->result()->type()->is_pointer())
            operand = cast->operand();
        if (operand && ar_aliases.count(operand))
          changed |= ar_aliases.insert(statement->result_or_null()).second;
      }
    }
    auto close = [&](ar::Code* code) {
      for (auto* block : *code) for (auto* statement : *block) {
        spend();
        for (auto it = statement->op_begin(); it != statement->op_end(); ++it) {
          if (!ar_aliases.count(*it)) continue;
          if (statement->result_or_null() && ar_aliases.count(statement->result_or_null()))
            continue;
          if (statement == create.address || statement == join.address) {
            require(cast< ar::PointerShift >(statement)->pointer() == *it); continue;
          }
          if (auto* comparison = dyn_cast< ar::Comparison >(statement)) {
            require((comparison->predicate() == ar::Comparison::PEQ ||
                     comparison->predicate() == ar::Comparison::PNE) &&
                    ((comparison->left() == *it && isa< ar::NullConstant >(comparison->right())) ||
                     (comparison->right() == *it && isa< ar::NullConstant >(comparison->left()))));
            continue;
          }
          auto* call = dyn_cast< ar::CallBase >(statement);
          auto* target = call ? native_handle_callee(call) : nullptr;
          require(target && target->intrinsic_id() == ar::Intrinsic::LibcFree &&
                  call->num_arguments() == 1 && call->argument(0) == *it && after_join(call));
        }
        for (const auto* loop : {&create, &join}) {
          for (auto it = statement->op_begin(); it != statement->op_end(); ++it) {
            if (*it == loop->address->result())
              require(statement == (loop->load ? static_cast< ar::Statement* >(loop->load)
                                              : static_cast< ar::Statement* >(loop->call)));
            if (loop->load && *it == loop->load->result())
              require(statement == loop->call && loop->call->argument(0) == *it);
          }
        }
      }
    };
    for (auto fi = ctx.bundle->function_begin(); fi != ctx.bundle->function_end(); ++fi)
      if ((*fi)->is_definition()) close((*fi)->body());
    for (auto gi = ctx.bundle->global_begin(); gi != ctx.bundle->global_end(); ++gi)
      if ((*gi)->is_definition()) close((*gi)->initializer());
  }
  void inventory() {
    auto* module = source_parent->getParent();
    // Closed no-child/no-joiner effect envelope. Unknown or callback-taking
    // APIs, timed joins, detach, cancellation, signals and longjmp all refuse.
    for (auto& function : *module) for (auto& block : function) for (auto& instruction : block) {
      spend();
      require(!llvm::isa< llvm::IntToPtrInst >(instruction) &&
              !llvm::isa< llvm::PtrToIntInst >(instruction) &&
              !llvm::isa< llvm::InvokeInst >(instruction) &&
              !llvm::isa< llvm::CallBrInst >(instruction));
      auto* call = llvm::dyn_cast< llvm::CallBase >(&instruction);
      if (!call || llvm::isa< llvm::DbgInfoIntrinsic >(instruction)) continue;
      auto* target = callee(call);
      auto* imported = binding(call);
      require(target && imported);
      if (!target->isDeclaration()) {
        require(target != source_parent && target != source_child &&
                ordinary_source_function_abi_attributes(target));
        continue;
      }
      auto* native = native_handle_callee(imported);
      require(native);
      const auto id = native->intrinsic_id();
      if (call == create.source || call == join.source) {
        const bool creating = call == create.source;
        require(call->getFunction() == source_parent &&
                native_handle_api(call, creating) &&
                imported == (creating ? create.call : join.call) &&
                (native->has_frontend() ||
                 id == (creating ? ar::Intrinsic::PthreadCreate : ar::Intrinsic::PthreadJoin)) &&
                isa< ar::NullConstant >(imported->argument(1)) &&
                (creating || (join.load && imported->argument(0) == join.load->result())));
        continue;
      }
      if (frozen_seed(call)) continue;
      // Other actual malloc calls (including retained, now-unused wrapper
      // bodies after frontend inlining) do not create threads or alias this
      // sealed fresh object. They never supply this family's base identity.
      if (target->getName() == "malloc") require(id == ar::Intrinsic::LibcMalloc);
      else if (target->getName() == "free") require(id == ar::Intrinsic::LibcFree);
      else if (target->getName() == "abort") require(id == ar::Intrinsic::LibcAbort);
      else if (target->getName() == "pthread_mutex_lock")
        require(id == ar::Intrinsic::PthreadMutexLock && call->getFunction() == source_child &&
                imported->code() == child->body());
      else if (target->getName() == "pthread_mutex_unlock")
        require(id == ar::Intrinsic::PthreadMutexUnlock && call->getFunction() == source_child &&
                imported->code() == child->body());
      else require((target->getName() == "__VERIFIER_nondet_int" ||
                    target->getName() == "__VERIFIER_nondet_uint") &&
                   call->arg_empty() && call->getType()->isIntegerTy(32) &&
                   native->has_frontend() &&
                   native->frontend< llvm::Value >() == target);
    }
    std::vector< llvm::Value* > uses{source_child};
    std::set< llvm::Value* > seen;
    while (!uses.empty()) {
      auto* value = uses.back(); uses.pop_back();
      spend();
      if (!seen.insert(value).second) continue;
      for (auto* user : value->users()) {
        if (llvm::isa< llvm::BitCastOperator >(user)) uses.push_back(user);
        else require(user == create.source && create.source->getArgOperand(2) == value);
      }
    }
    auto close = [&](ar::Code* code) {
      for (auto* block : *code) for (auto* statement : *block) {
        spend();
        if (auto* call = dyn_cast< ar::CallBase >(statement)) {
          auto* source = call->has_frontend()
              ? llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >()) : nullptr;
          require(source && binding(source) == call);
        }
        if (auto* cast = dyn_cast< ar::UnaryOperation >(statement))
          require(cast->op() != ar::UnaryOperation::SIToPtr &&
                  cast->op() != ar::UnaryOperation::UIToPtr &&
                  cast->op() != ar::UnaryOperation::PtrToSI &&
                  cast->op() != ar::UnaryOperation::PtrToUI);
        for (auto op = statement->op_begin(); op != statement->op_end(); ++op) {
          auto* function = dyn_cast< ar::FunctionPointerConstant >(*op);
          if (!function) continue;
          require(function->function() != parent);
          if (function->function() == child)
            require(statement == create.call && *op == create.call->argument(2));
        }
      }
    };
    for (auto fi = ctx.bundle->function_begin(); fi != ctx.bundle->function_end(); ++fi)
      if ((*fi)->is_definition()) close((*fi)->body());
    for (auto gi = ctx.bundle->global_begin(); gi != ctx.bundle->global_end(); ++gi)
      if ((*gi)->is_definition()) close((*gi)->initializer());
  }
  void prepare() {
    require(!std::getenv("IKOS_RACE_DISABLE_INDEXED_JOIN_PRODUCER") &&
            ctx.opts.enable_thread_modular && ctx.opts.procedural != Procedural::Intraprocedural &&
            ctx.opts.entry_points.size() == 1 && ctx.opts.no_init_globals.empty() &&
            !ctx.bundle->global_or_null("ar.global_ctors") &&
            !ctx.bundle->global_or_null("ar.global_dtors"));
    parent = ctx.opts.entry_points.front();
    require(parent->name() == "main" && parent->num_parameters() == 0 &&
            parent->has_frontend());
    source_parent = llvm::dyn_cast< llvm::Function >(parent->frontend< llvm::Value >());
    require(source_parent && source_parent->arg_empty() && !source_parent->isVarArg() &&
            source_parent->getCallingConv() == llvm::CallingConv::C && source_parent->use_empty() &&
            source_parent->getParent()->getModuleInlineAsm().empty() &&
            ordinary_source_function_abi_attributes(source_parent) &&
            facts.index(budget) && facts.memory_calls_allocations().unique &&
            facts.memory_calls_allocations().complete);
    const auto abi = pthread_object_layout(ctx.bundle, source_parent->getParent());
    require(bool(abi)); word = abi.word;
    require(source_parent->getParent()->getDataLayout().getIndexSizeInBits(0) == word * 8);
    for (const auto& item : facts.memory_calls_allocations().statements) {
      spend();
      auto* call = llvm::dyn_cast< llvm::CallBase >(item.first);
      auto* target = call ? callee(call) : nullptr;
      if (!target) continue;
      if (target->getName() == "pthread_create") {
        require(!create.source); create.source = call;
      }
      if (target->getName() == "pthread_join") {
        require(!join.source); join.source = call;
      }
    }
    require(create.source && join.source &&
            create.source->getFunction() == source_parent &&
            join.source->getFunction() == source_parent &&
            native_handle_api(create.source, true) && native_handle_api(join.source, false));
    source_child = llvm::dyn_cast< llvm::Function >(
        create.source->getArgOperand(2)->stripPointerCasts());
    require(source_child && source_child->getArg(0)->use_empty());
    auto found = facts.functions().find(source_child);
    require(found != facts.functions().end()); child = found->second;
    require(child->is_definition() && child->num_parameters() == 1);
    stage = "source create loop"; source_loop(create);
    stage = "source join loop"; source_loop(join);
    require(create.source_condition->getPredicate() == join.source_condition->getPredicate() &&
            create.source_bound == join.source_bound &&
            facts.dominators(source_parent).dominates(create.source_exit, join.source_pre));
    stage = "AR create loop"; imported_loop(create);
    stage = "AR join loop"; imported_loop(join);
    require(same_integer(create.bound, join.bound) && dominates(create.exit, join.pre));
    auto* native_bound = llvm::dyn_cast< llvm::Instruction >(create.source_bound);
    auto* imported_bound = dyn_cast< ar::InternalVariable >(uncast(create.bound));
    auto* bound_definition = imported_bound ? unique_def(imported_bound) : nullptr;
    if (native_bound) {
      for (auto* next : llvm::successors(native_bound->getParent()))
        require(!reaches(next, native_bound->getParent()));
    }
    if (bound_definition) {
      for (auto it = bound_definition->parent()->successor_begin();
           it != bound_definition->parent()->successor_end(); ++it)
        require(!reaches(*it, bound_definition->parent()));
    }
    stage = "stable heap definition"; heap_factory();
    stage = "source/AR bound Boolean definitions"; prepare_bound_guards();
    stage = "closed call and activation inventory"; inventory();
    stage = "closed native handle storage"; close_storage();
    // Inherit the existing object-based create/output-only unknown-join model.
    // No numeric ID, successful failed-slot join, coverage waiver, or registry
    // entry is produced. Strict final coverage remains a separate condition.
    effect_frame = true;
    prepared = true;
  }

  Interval interval(ar::Value* value, const value::AbstractDomain& inv) const {
    const auto& literal = ctx.lit_factory->get_scalar(value);
    if (literal.is_machine_int()) return Interval(literal.machine_int());
    auto* type = cast< ar::IntegerType >(value->type());
    if (!literal.is_machine_int_var() ||
        !inv.first().normal().uninit_is_initialized(literal.var()))
      return Interval::bottom(type->bit_width(), type->sign());
    return inv.first().normal().int_to_interval(literal.var());
  }
  void prepare_bound_guards() {
    auto* bound_variable = dyn_cast_or_null< ar::InternalVariable >(uncast(create.bound));
    auto* bound_definition = bound_variable ? unique_def(bound_variable) : nullptr;
    auto bound_precedes = [&](ar::Statement* guard) {
      if (!bound_definition) return isa< ar::IntegerConstant >(uncast(create.bound));
      if (bound_definition->parent() != guard->parent())
        return dominates(bound_definition->parent(), guard->parent());
      for (auto* statement : *guard->parent()) {
        if (statement == bound_definition) return true;
        if (statement == guard) return false;
      }
      return false;
    };
    // Decode only complete source/AR Boolean definitions. The existing AI,
    // including actual helper execution, supplies the MUST truth at the query.
    for (auto& block : *source_parent) for (auto& instruction : block) {
      auto* cmp = llvm::dyn_cast< llvm::ICmpInst >(&instruction);
      if (!cmp || cmp->getOperand(0) != create.source_bound ||
          (cmp->getPredicate() != llvm::ICmpInst::ICMP_ULE &&
           cmp->getPredicate() != llvm::ICmpInst::ICMP_ULT) ||
          !facts.dominators(source_parent).dominates(cmp, source_extent)) continue;
      auto* constant = llvm::dyn_cast< llvm::ConstantInt >(cmp->getOperand(1));
      if (!constant || constant->getBitWidth() > 64) continue;
      Z cap(constant->getZExtValue());
      if (cmp->getPredicate() == llvm::ICmpInst::ICMP_ULT) --cap;
      if (cap < 0) continue;
      bool acyclic = true;
      for (auto* next : llvm::successors(cmp->getParent()))
        if (reaches(next, cmp->getParent())) acyclic = false;
      if (!acyclic) continue;
      std::set< ar::InternalVariable* > flags;
      for (auto* ar_block : *parent->body()) for (auto* statement : *ar_block) {
        spend();
        auto* result = dyn_cast_or_null< ar::InternalVariable >(statement->result_or_null());
        auto* type = result ? dyn_cast< ar::IntegerType >(result->type()) : nullptr;
        if (type && type->bit_width() == 1 && type->is_unsigned() &&
            result->has_frontend() && result->frontend< llvm::Value >() == cmp)
          flags.insert(result);
      }
      const auto pred = cmp->getPredicate() == llvm::ICmpInst::ICMP_ULE
          ? ar::Comparison::UILE : ar::Comparison::UILT;
      for (auto* flag : flags) {
        unsigned definitions = 0, values = 0;
        bool complete = true;
        for (auto* ar_block : *parent->body()) for (auto* statement : *ar_block) {
          spend();
          if (statement->result_or_null() != flag) continue;
          ++definitions;
          auto* assign = dyn_cast< ar::Assignment >(statement);
          auto* value = assign ? dyn_cast< ar::IntegerConstant >(assign->operand()) : nullptr;
          if (!value || (value->value().to_z_number() != 0 &&
                         value->value().to_z_number() != 1)) { complete = false; continue; }
          const bool truth = value->value().to_z_number() == 1;
          values |= truth ? 2u : 1u;
          bool guarded = false;
          for (auto* previous : *ar_block) {
            if (previous == statement) break;
            auto* guard = dyn_cast< ar::Comparison >(previous);
            if (guard && guard->has_frontend() && guard->frontend< llvm::Value >() == cmp &&
                guard->predicate() == (truth ? pred : ar::Comparison::inverse_predicate(pred)) &&
                same_integer(guard->left(), create.bound) && bound_precedes(guard) &&
                source_integer_binding(guard->left(), create.source_bound) &&
                isa< ar::IntegerConstant >(guard->right()) &&
                source_integer_binding(guard->right(), constant)) guarded = true;
          }
          complete &= guarded &&
              !reaches(extent_product->parent(), statement->parent());
          for (auto it = ar_block->successor_begin(); it != ar_block->successor_end(); ++it)
            if (reaches(*it, ar_block)) complete = false;
        }
        if (!complete || definitions != 2 || values != 3) continue;
        std::set< ar::InternalVariable* > carriers{flag};
        bool changed = true;
        while (changed) {
          changed = false;
          for (auto* ar_block : *parent->body()) for (auto* statement : *ar_block) {
            spend();
            auto* cast = dyn_cast< ar::UnaryOperation >(statement);
            if (!cast || !carriers.count(dyn_cast< ar::InternalVariable >(cast->operand())) ||
                (cast->op() != ar::UnaryOperation::ZExt &&
                 cast->op() != ar::UnaryOperation::Bitcast) ||
                !cast->result()->type()->is_integer() ||
                unique_def(cast->result()) != cast ||
                !statement_dominates(cast, extent_product)) continue;
            bool acyclic_carrier = true;
            for (auto it = ar_block->successor_begin(); it != ar_block->successor_end(); ++it)
              if (reaches(*it, ar_block)) acyclic_carrier = false;
            if (!acyclic_carrier) continue;
            const auto* in = ikos::core::cast< ar::IntegerType >(cast->operand()->type());
            const auto* out = ikos::core::cast< ar::IntegerType >(cast->result()->type());
            if ((cast->op() == ar::UnaryOperation::ZExt && in->bit_width() < out->bit_width()) ||
                (cast->op() == ar::UnaryOperation::Bitcast &&
                 in->bit_width() == out->bit_width() && out->bit_width() > 1))
              changed |= carriers.insert(cast->result()).second;
          }
        }
        for (auto* carrier : carriers) bound_guards.push_back({carrier, cap});
      }
    }
  }
  void extent_observation(const value::AbstractDomain& inv, CallContext* cc) {
    require(!cc || !cc->has_parent());
    auto count = interval(create.bound, inv);
    auto* queried_type = cast< ar::IntegerType >(create.bound->type());
    // Loop-header bitcasts may not exist yet here. Query a live equal-bit alias
    // at the actual extent, then explicitly check the nonnegative common range.
    auto query_alias = [&](ar::Value* candidate) {
      if (count.is_bottom() && same_integer(candidate, create.bound)) {
        count = interval(candidate, inv);
        queried_type = cast< ar::IntegerType >(candidate->type());
      }
    };
    query_alias(extent_product->left());
    for (auto* adapter : extent_adapters) {
      query_alias(adapter->operand());
      query_alias(adapter->result());
    }
    require(!count.is_bottom());
    Z lo = count.lb().to_z_number(), hi = count.ub().to_z_number();
    for (const auto& guard : bound_guards) {
      auto truth = interval(guard.truth, inv);
      auto singleton = truth.singleton();
      const auto* type = cast< ar::IntegerType >(create.bound->type());
      // A small unsigned upper bound also excludes negative signed bit patterns.
      if (singleton && singleton->to_z_number() == 1 &&
          guard.upper <= core::MachineInt::max(type->bit_width(), type->sign()).to_z_number() &&
          guard.upper <= core::MachineInt::max(queried_type->bit_width(),
                                               queried_type->sign()).to_z_number()) {
        lo = std::max(lo, Z(0));
        hi = std::min(hi, guard.upper);
      }
    }
    const auto* bound_type = cast< ar::IntegerType >(create.bound->type());
    require(lo >= 0 && lo <= hi &&
            hi <= core::MachineInt::max(bound_type->bit_width(), bound_type->sign()).to_z_number());
    const Z address_max = (Z(1) << (word * 8)) - 1;
    if (log::is_enabled_for(LogLevel::Debug))
      log::debug("Indexed thread completion: pre-multiply N upper=" + hi.str());
    require(hi <= address_max / Z(word));
    if (auto* cast = llvm::dyn_cast< llvm::CastInst >(source_extent->getOperand(0))) {
      const auto bits = cast->getOperand(0)->getType()->getIntegerBitWidth();
      require(hi <= core::MachineInt::max(bits,
          cast->getOpcode() == llvm::Instruction::SExt ? core::Signed : core::Unsigned).to_z_number());
    }
    for (auto* adapter : extent_adapters) {
      auto* input = cast< ar::IntegerType >(adapter->operand()->type());
      auto* output = cast< ar::IntegerType >(adapter->result()->type());
      require(hi <= core::MachineInt::max(input->bit_width(), input->sign()).to_z_number() &&
              hi <= core::MachineInt::max(output->bit_width(), output->sign()).to_z_number());
    }
    // Never use a poison/no-wrap product to narrow its own input interval.
    if (source_extent->hasNoSignedWrap() ||
        (extent_product->op() == ar::BinaryOperation::SMul && extent_product->has_no_wrap()))
      require(Z(word) * hi <= (Z(1) << (word * 8 - 1)) - 1);
    extent_count_upper = std::max(extent_count_upper, hi);
    extent_seen = true;
  }
  void observation(Loop& loop, ar::Statement* statement,
                   const value::AbstractDomain& inv, CallContext* cc) {
    require(!cc || !cc->has_parent());
    auto index = interval(loop.counter, inv);
    auto count = interval(loop.bound, inv);
    require(!index.is_bottom() && !count.is_bottom());
    const Z lo = index.lb().to_z_number(), hi = index.ub().to_z_number();
    require(lo >= 0);
    // Keep the raw union. All adapter/step checks use the FINAL extent cap,
    // so a later, wider extent observation cannot invalidate an early check.
    if (!loop.call_seen && !loop.step_seen) { loop.lower = lo; loop.upper = hi; }
    else { loop.lower = std::min(loop.lower, lo); loop.upper = std::max(loop.upper, hi); }
    if (statement == loop.step) { loop.step_seen = true; return; }
    // The raw allocation result may have been forgotten by liveness. This
    // live, uniquely bound alias is already proved a pure bitcast of that base.
    const auto& base_literal = ctx.lit_factory->get_scalar(loop.address->pointer());
    const auto& address_literal = ctx.lit_factory->get_scalar(loop.address->result());
    const auto& state = inv.first().normal();
    stage = "live base initialization/nullity";
    require(base_literal.is_pointer_var() && address_literal.is_pointer_var() &&
            state.uninit_is_initialized(base_literal.var()) &&
            state.uninit_is_initialized(address_literal.var()) &&
            state.nullity_is_non_null(base_literal.var()));
    auto points = state.pointer_to_points_to(base_literal.var());
    auto offset = state.pointer_offset_to_interval(base_literal.var()).singleton();
    stage = "live base points-to/offset/lifetime";
    require(points.is_set() && points.size() == 1 && offset &&
            offset->to_z_number() == 0 && isa< DynAllocMemoryLocation >(*points.begin()) &&
            state.pointer_to_points_to(address_literal.var()).equals(points) &&
            state.lifetime_to_lifetime(*points.begin()).is_allocated() &&
            (!object || object == *points.begin()));
    object = *points.begin();
    loop.call_seen = true;
  }
  bool final_range(const Loop& loop) const {
    if (extent_count_upper <= 0 || loop.lower < 0) return false;
    const auto* bound_type = cast< ar::IntegerType >(loop.bound->type());
    if (extent_count_upper > core::MachineInt::max(bound_type->bit_width(),
                                                  bound_type->sign()).to_z_number()) return false;
    if (loop.source_condition->isSigned() &&
        extent_count_upper > core::MachineInt::max(
            loop.phi->getType()->getIntegerBitWidth(), core::Signed).to_z_number()) return false;
    const Z hi = std::min(loop.upper, extent_count_upper - Z(1));
    const auto* type = cast< ar::IntegerType >(loop.counter->type());
    if (hi < 0 || hi >= core::MachineInt::max(type->bit_width(), type->sign()).to_z_number())
      return false;
    if (loop.source_step->hasNoSignedWrap() &&
        hi >= core::MachineInt::max(loop.source_step->getType()->getIntegerBitWidth(),
                                   core::Signed).to_z_number()) return false;
    if (loop.step->op() == ar::BinaryOperation::SAdd && loop.step->has_no_wrap() &&
        hi >= core::MachineInt::max(type->bit_width(), core::Signed).to_z_number())
      return false;
    for (auto* adapter : loop.source_adapters) {
      const auto bits = adapter->getOperand(0)->getType()->getIntegerBitWidth();
      if (hi > core::MachineInt::max(bits,
          adapter->getOpcode() == llvm::Instruction::SExt ? core::Signed : core::Unsigned).to_z_number())
        return false;
    }
    auto* gep_type = llvm::cast< llvm::IntegerType >(loop.gep->getOperand(1)->getType());
    if (hi > core::MachineInt::max(gep_type->getBitWidth(), core::Signed).to_z_number())
      return false;
    for (auto* adapter : loop.adapters) {
      auto* input = cast< ar::IntegerType >(adapter->operand()->type());
      auto* output = cast< ar::IntegerType >(adapter->result()->type());
      if (hi > core::MachineInt::max(input->bit_width(), input->sign()).to_z_number() ||
          hi > core::MachineInt::max(output->bit_width(), output->sign()).to_z_number())
        return false;
    }
    // Complete 0,+1,<N loops put actual i in [0,N); this final range preserves
    // every adapter and step. For bytes, |word*(i-j)+(u-v)|<word*N<2^w,
    // hence equality modulo 2^w implies the same index and byte.
    return true;
  }
  bool endpoint(const AccessRecord& record, bool is_parent) const {
    auto* function = is_parent ? parent : child;
    auto* source_function = is_parent ? source_parent : source_child;
    if (!record.stmt || record.stmt->code() != function->body() ||
        record.thread_id != function->name() || !record.stmt->has_frontend() ||
        record.width == 0 || (record.call_context && record.call_context->has_parent()))
      return false;
    auto* source = llvm::dyn_cast< llvm::Instruction >(record.stmt->frontend< llvm::Value >());
    if (!source || source->getFunction() != source_function ||
        original(source) != record.stmt) return false;
    auto* load = llvm::dyn_cast< llvm::LoadInst >(source);
    auto* store = llvm::dyn_cast< llvm::StoreInst >(source);
    auto* imported_load = dyn_cast< ar::Load >(record.stmt);
    auto* imported_store = dyn_cast< ar::Store >(record.stmt);
    auto* type = load ? load->getType() : store ? store->getValueOperand()->getType() : nullptr;
    ar::Type* imported_type = imported_load ? imported_load->result()->type()
        : imported_store ? imported_store->value()->type() : nullptr;
    if (!type || !imported_type || bool(load) != bool(imported_load) ||
        bool(store) != bool(imported_store) ||
        record.kind != (load ? AccessKind::Read : AccessKind::Write)) return false;
    auto size = source_parent->getParent()->getDataLayout().getTypeStoreSize(type);
    if (size.isScalable() || record.width != size.getFixedValue() ||
        record.width != ctx.bundle->data_layout().store_size_in_bytes(imported_type)) return false;
    return !is_parent || (after_join(source) && after_join(record.stmt));
  }
public:
  explicit Impl(Context& context) : ctx(context), facts(source_program_facts(context)) {
    try { prepare(); }
    catch (const Refuse&) {
      prepared = false;
      if (create.source && join.source && log::is_enabled_for(LogLevel::Debug))
        log::debug(std::string("Indexed thread completion: refused ") + stage);
    }
    catch (const std::out_of_range&) { prepared = false; }
  }
  void observe(ar::Statement* statement, const value::AbstractDomain& inv, CallContext* cc) {
    if (!prepared || !valid) return;
    try {
      if (statement == extent_product) {
        stage = "pre-multiply extent bound";
        extent_observation(inv, cc);
      }
      for (auto* loop : {&create, &join})
        if (statement == loop->call || statement == loop->step) {
          stage = loop == &create ? "create range/base/context" : "join range/base/context";
          observation(*loop, statement, inv, cc);
        }
    } catch (const Refuse&) {
      valid = false;
      if (log::is_enabled_for(LogLevel::Debug))
        log::debug(std::string("Indexed thread completion: invalid ") + stage);
    }
  }
  bool ready() const {
    return prepared && valid && effect_frame && extent_seen && object &&
        create.call_seen && create.step_seen && join.call_seen && join.step_seen &&
        final_range(create) && final_range(join) &&
        !std::getenv("IKOS_RACE_DISABLE_INDEXED_JOIN_CONSUMER") &&
        ctx.race_analysis_complete && !ctx.concurrent_env.is_dirty() &&
        ctx.concurrent_env.unmodeled_effects().empty() &&
        ctx.concurrent_env.unmodeled_thread_creations().empty();
  }
  bool indexed_family(ar::CallBase* spawning, ar::Function* entry,
                      IndexedThreadFamily& result) const {
    if (!ready() || spawning != create.call || entry != child) return false;
    result = {create.source_bound, create.bound, source_allocation, allocation,
              extent_count_upper, word};
    return true;
  }
  bool completes_before(ar::CallBase* spawning, ar::Function* entry,
                        ar::CallBase* release, CallContext* cc) const {
    if (!ready() || spawning != create.call || entry != child || !release ||
        (cc && cc->has_parent()) || !release->has_frontend()) return false;
    auto* source = llvm::dyn_cast< llvm::CallBase >(release->frontend< llvm::Value >());
    auto* target = source ? callee(source) : nullptr;
    auto* native = native_handle_callee(release);
    return source && target && target->isDeclaration() && target->getName() == "free" &&
        source->arg_size() == 1 && source->getType()->isVoidTy() &&
        binding(source) == release && native && native->intrinsic_id() == ar::Intrinsic::LibcFree &&
        release->num_arguments() == 1 && !release->has_result() &&
        pointer_binding(release->argument(0), source->getArgOperand(0)) &&
        after_join(source) && after_join(release);
  }
  bool excludes(const AccessRecord& a, const AccessRecord& b) const {
    if (!ready()) return false;
    // A family can be parallel with itself: only the parent after the completed
    // join loop is ordered. Never reuse the old same-root serial conclusion.
    return (endpoint(a, true) && endpoint(b, false)) ||
           (endpoint(b, true) && endpoint(a, false));
  }
  JsonDict info(const AccessRecord& a, const AccessRecord& b) const {
    return {{"thread_lifecycle_contract", "indexed-native-handle-completion-v1"},
            {"safe_reason", "all successful children of one exact indexed native handle family completed before the parent endpoint"},
            {"thread_a", a.thread_id}, {"thread_b", b.thread_id},
            {"handle_stride_bytes", word},
            {"extent_count_upper", extent_count_upper.str()},
            {"coordinate_contract", "same-N unsigned full-cell modular injection; extent proved before multiplication"},
            {"effect_contract", "inherited object-based create and output-only unknown join; no parent locks, cancellation, detach or competing joiner"},
            {"create_index_lower", create.lower.str()}, {"create_index_upper", create.upper.str()},
            {"join_index_lower", join.lower.str()}, {"join_index_upper", join.upper.str()},
            {"scope", "sole main activation; source+AR nonNULL continuation; live closed heap; complete same-bound loops; ordinary GNU conditional completion; no HB for no-child joins and no claim about arbitrary invalid glibc effects"}};
  }
};
ThreadLifecycleObservations::ThreadLifecycleObservations(Context& ctx) : _impl(new Impl(ctx)) {}
ThreadLifecycleObservations::~ThreadLifecycleObservations() = default;
void ThreadLifecycleObservations::observe(ar::Statement* s, const value::AbstractDomain& i, CallContext* c) {
  _impl->observe(s, i, c);
}
bool ThreadLifecycleObservations::excludes(const AccessRecord& a, const AccessRecord& b) const {
  return _impl->excludes(a, b);
}
JsonDict ThreadLifecycleObservations::info(const AccessRecord& a, const AccessRecord& b) const {
  return _impl->info(a, b);
}
bool ThreadLifecycleObservations::indexed_family(ar::CallBase* create, ar::Function* child,
                                                  IndexedThreadFamily& result) const {
  return _impl->indexed_family(create, child, result);
}
bool ThreadLifecycleObservations::completes_before(ar::CallBase* create, ar::Function* child,
                                                 ar::CallBase* release, CallContext* cc) const {
  return _impl->completes_before(create, child, release, cc);
}
