// Copyright and license: see data_race.hpp.
// Included in ThreadLifecycleObservations::Impl. This is a separate schema
// inside the existing observation owner, not a new analysis or points-to fact.
  struct ObjectFamily {
    bool active = false, create_seen = false, join_seen = false;
    bool own_free_seen = false, table_free_seen = false;
    llvm::StructType* type = nullptr;
    std::uint64_t size = 0, tid_offset = 0;
    llvm::CallBase *birth = nullptr, *own_free = nullptr, *table_free = nullptr;
    ar::CallBase *birth_ar = nullptr, *own_free_ar = nullptr, *table_free_ar = nullptr;
    llvm::StoreInst* publish = nullptr;
    ar::Store* publish_ar = nullptr;
    llvm::LoadInst *object_load = nullptr, *release_load = nullptr;
    ar::Load *object_load_ar = nullptr, *release_load_ar = nullptr;
    llvm::GetElementPtrInst *create_tid = nullptr, *join_tid = nullptr, *release_cell = nullptr;
    ar::PointerShift *create_tid_ar = nullptr, *join_tid_ar = nullptr, *release_cell_ar = nullptr;
    llvm::BasicBlock* birth_block = nullptr;
    ar::BasicBlock* birth_block_ar = nullptr;
    struct Field { std::uint64_t offset, width; };
    std::map< ar::Statement*, Field > fields;
    std::set< ar::Statement* > globals;
  } family;

  // Deliberately do not relax native_handle_api's historical NULL-argument
  // contract. This private gate is consumed only by the fully closed schema.
  bool object_create_api(llvm::CallBase* call) const {
    auto* api = callee(call);
    if (!llvm::isa< llvm::CallInst >(call) || !api || !api->isDeclaration() ||
        api->getName() != "pthread_create" || api->isVarArg() ||
        api->arg_size() != 4 || call->arg_size() != 4 || !call->use_empty() ||
        call->isInlineAsm() || call->hasOperandBundles() ||
        api->getCallingConv() != llvm::CallingConv::C ||
        call->getCallingConv() != llvm::CallingConv::C ||
        !api->getReturnType()->isIntegerTy(32) || !call->getType()->isIntegerTy(32) ||
        call->getFunctionType() != api->getFunctionType() ||
        !ordinary_source_call_abi_attributes(call, api) ||
        !llvm::isa< llvm::ConstantPointerNull >(call->getArgOperand(1)))
      return false;
    for (unsigned n = 0; n < 4; ++n) {
      auto* type = llvm::dyn_cast< llvm::PointerType >(call->getArgOperand(n)->getType());
      if (!type || type->getAddressSpace() ||
          type != api->getFunctionType()->getParamType(n)) return false;
      for (auto attr : {llvm::Attribute::ByVal, llvm::Attribute::StructRet,
                        llvm::Attribute::InAlloca, llvm::Attribute::Preallocated})
        if (call->paramHasAttr(n, attr) || api->hasParamAttribute(n, attr)) return false;
    }
    auto* entry = llvm::dyn_cast< llvm::Function >(
        call->getArgOperand(2)->stripPointerCasts());
    auto* output = llvm::cast< llvm::PointerType >(call->getArgOperand(0)->getType());
    auto* signature = llvm::cast< llvm::PointerType >(call->getArgOperand(2)->getType());
    return output->getPointerElementType()->isIntegerTy(word * 8) &&
        !llvm::isa< llvm::ConstantPointerNull >(call->getArgOperand(3)) &&
        entry && !entry->isDeclaration() && !entry->isVarArg() &&
        entry->arg_size() == 1 && entry->getName() != "main" &&
        entry->getCallingConv() == llvm::CallingConv::C &&
        ordinary_source_function_abi_attributes(entry) &&
        entry->getReturnType()->isPointerTy() &&
        entry->getReturnType()->getPointerAddressSpace() == 0 &&
        entry->getArg(0)->getType()->isPointerTy() &&
        entry->getArg(0)->getType()->getPointerAddressSpace() == 0 &&
        signature->getPointerElementType() == entry->getFunctionType();
  }
  static llvm::GetElementPtrInst* object_gep(llvm::Value* value) {
    // Preserve zero-offset field GEPs: stripPointerCasts removes them too.
    for (unsigned n = 0; n < 32; ++n) {
      auto* cast = llvm::dyn_cast< llvm::BitCastOperator >(value);
      if (!cast) return llvm::dyn_cast< llvm::GetElementPtrInst >(value);
      value = cast->getOperand(0);
    }
    return nullptr;
  }
  bool object_same_index(llvm::Value* a, llvm::Value* b) const {
    for (unsigned n = 0; n < 32; ++n) {
      if (a == b) return true;
      auto* ac = llvm::dyn_cast< llvm::CastInst >(a);
      auto* bc = llvm::dyn_cast< llvm::CastInst >(b);
      if (!ac || !bc || ac->getOpcode() != bc->getOpcode() ||
          (ac->getOpcode() != llvm::Instruction::SExt &&
           ac->getOpcode() != llvm::Instruction::ZExt) ||
          ac->getType() != bc->getType() ||
          ac->getOperand(0)->getType() != bc->getOperand(0)->getType()) return false;
      a = ac->getOperand(0); b = bc->getOperand(0);
    }
    return false;
  }
  bool object_same_index(ar::Value* a, ar::Value* b) const {
    for (unsigned n = 0; n < 32; ++n) {
      a = uncast(a); b = uncast(b);
      if (!a || !b) return false;
      if (a == b) return true;
      auto* av = dyn_cast< ar::InternalVariable >(a);
      auto* bv = dyn_cast< ar::InternalVariable >(b);
      auto* ac = av ? dyn_cast_or_null< ar::UnaryOperation >(unique_def(av)) : nullptr;
      auto* bc = bv ? dyn_cast_or_null< ar::UnaryOperation >(unique_def(bv)) : nullptr;
      if (!ac || !bc || ac->op() != bc->op() ||
          (ac->op() != ar::UnaryOperation::SExt && ac->op() != ar::UnaryOperation::ZExt) ||
          a->type() != b->type() ||
          ac->operand()->type() != bc->operand()->type()) return false;
      a = ac->operand(); b = bc->operand();
    }
    return false;
  }
  ar::PointerShift* object_shift(ar::Value* value, llvm::GetElementPtrInst* gep) {
    auto* v = dyn_cast_or_null< ar::InternalVariable >(uncast(value));
    auto* shift = v ? dyn_cast_or_null< ar::PointerShift >(unique_def(v)) : nullptr;
    require(shift && gep && shift->has_frontend() &&
            shift->frontend< llvm::Value >() == gep &&
            pointer_binding(value, gep) &&
            pointer_binding(shift->pointer(), gep->getPointerOperand()));
    return shift;
  }
  std::uint64_t object_field(llvm::GetElementPtrInst* gep, ar::PointerShift* shift,
                             llvm::Value* source_root, ar::Value* imported_root,
                             unsigned width) {
    require(gep && shift && gep->getAddressSpace() == 0 &&
            gep->getSourceElementType() == family.type && gep->getNumIndices() == 2 &&
            gep->getPointerOperand()->stripPointerCasts() == source_root->stripPointerCasts() &&
            uncast(shift->pointer()) == uncast(imported_root));
    auto* zero = llvm::dyn_cast< llvm::ConstantInt >(gep->getOperand(1));
    auto* field = llvm::dyn_cast< llvm::ConstantInt >(gep->getOperand(2));
    require(zero && zero->isZero() && field && field->getValue().getActiveBits() <= 32 &&
            field->getZExtValue() < family.type->getNumElements());
    const auto& dl = source_parent->getParent()->getDataLayout();
    auto* field_type = family.type->getElementType(field->getZExtValue());
    const auto size = dl.getTypeStoreSize(field_type);
    require(!size.isScalable() && size.getFixedValue() == width &&
            field_type->isIntegerTy(width * 8));
    const auto offset = dl.getStructLayout(family.type)->getElementOffset(field->getZExtValue());
    require(offset <= family.size && width <= family.size - offset);
    Z imported_offset(0);
    require(shift->num_terms() > 0);
    for (unsigned n = 0; n < shift->num_terms(); ++n) {
      const auto& term = shift->term(n);
      auto* index = dyn_cast< ar::IntegerConstant >(term.second);
      require(index && term.first.bit_width() == word * 8);
      imported_offset += term.first.to_z_number() * index->value().to_z_number();
    }
    require(imported_offset == Z(offset));
    return offset;
  }
  void object_source_loop(Loop& loop, bool creating) {
    // Source CFG: header -> malloc/NULL test -> successful latch -> header.
    // Join has one body/latch. Neither graph admits a bypass or early break.
    auto* body = loop.source->getParent();
    auto* index = loop.gep->getOperand(1);
    for (unsigned n = 0; n < 32; ++n) {
      auto* cast = llvm::dyn_cast< llvm::CastInst >(index);
      if (!cast) break;
      require(cast->getOpcode() == llvm::Instruction::SExt ||
              cast->getOpcode() == llvm::Instruction::ZExt);
      require(cast->getType()->isIntegerTy() && cast->getOperand(0)->getType()->isIntegerTy());
      loop.source_adapters.push_back(cast); index = cast->getOperand(0);
    }
    loop.phi = llvm::dyn_cast< llvm::PHINode >(index);
    require(loop.phi && loop.phi->getNumIncomingValues() == 2 &&
            loop.phi->getType()->isIntegerTy() &&
            loop.phi->getType()->getIntegerBitWidth() <= word * 8);
    auto* header = loop.phi->getParent();
    auto* branch = llvm::dyn_cast< llvm::BranchInst >(header->getTerminator());
    auto* guard = branch && branch->isConditional()
        ? llvm::dyn_cast< llvm::ICmpInst >(branch->getCondition()) : nullptr;
    auto* first = creating ? family.birth->getParent() : body;
    require(guard && guard->getParent() == header && guard->getOperand(0) == loop.phi &&
            (guard->getPredicate() == llvm::ICmpInst::ICMP_SLT ||
             guard->getPredicate() == llvm::ICmpInst::ICMP_ULT) &&
            branch->getSuccessor(0) == first && first->getSinglePredecessor() == header);
    loop.source_condition = guard; loop.source_bound = guard->getOperand(1);
    loop.source_exit = branch->getSuccessor(1);
    require(loop.source_exit->getSinglePredecessor() == header);
    for (unsigned n = 0; n < 2; ++n) {
      auto* zero = llvm::dyn_cast< llvm::ConstantInt >(loop.phi->getIncomingValue(n));
      if (zero && zero->isZero()) {
        require(!loop.source_pre); loop.source_pre = loop.phi->getIncomingBlock(n);
      } else {
        require(!loop.source_step);
        loop.source_step = llvm::dyn_cast< llvm::BinaryOperator >(loop.phi->getIncomingValue(n));
        require(loop.source_step &&
                loop.source_step->getParent() == loop.phi->getIncomingBlock(n));
      }
    }
    auto* step = loop.source_step;
    auto* one = step ? llvm::dyn_cast< llvm::ConstantInt >(step->getOperand(1)) : nullptr;
    auto* pre = loop.source_pre
        ? llvm::dyn_cast< llvm::BranchInst >(loop.source_pre->getTerminator()) : nullptr;
    auto* latch = step ? step->getParent() : nullptr;
    auto* back = latch ? llvm::dyn_cast< llvm::BranchInst >(latch->getTerminator()) : nullptr;
    auto* body_branch = llvm::dyn_cast< llvm::BranchInst >(body->getTerminator());
    require(pre && pre->isUnconditional() && pre->getSuccessor(0) == header &&
            loop.source_pre != header && loop.source_pre != body && loop.source_pre != first &&
            loop.source_pre != latch && body_branch && body_branch->isUnconditional() &&
            back && back->isUnconditional() && back->getSuccessor(0) == header &&
            step && one && one->isOne() &&
            step->getOpcode() == llvm::Instruction::Add && step->getOperand(0) == loop.phi);
    if (body == latch) require(loop.source->comesBefore(step));
    else {
      require(body_branch->getSuccessor(0) == latch && latch->getSinglePredecessor() == body);
      for (auto& instruction : *latch)
        require(&instruction == step || &instruction == back ||
                llvm::isa< llvm::DbgInfoIntrinsic >(instruction));
    }
    if (creating) {
      family.birth_block = first;
      auto* test = llvm::dyn_cast< llvm::BranchInst >(first->getTerminator());
      auto* null = test && test->isConditional()
          ? llvm::dyn_cast< llvm::ICmpInst >(test->getCondition()) : nullptr;
      require(first != body && body->getSinglePredecessor() == first &&
              null && null->isEquality() && null->getOperand(0) == family.birth &&
              llvm::isa< llvm::ConstantPointerNull >(null->getOperand(1)));
      unsigned success = null->getPredicate() == llvm::ICmpInst::ICMP_NE ? 0 : 1;
      require(test->getSuccessor(success) == body);
      auto* fail = test->getSuccessor(1 - success);
      require(fail->getSinglePredecessor() == first &&
              llvm::isa< llvm::UnreachableInst >(fail->getTerminator()));
      bool aborted = false;
      for (auto& ins : *fail) {
        auto* call = llvm::dyn_cast< llvm::CallBase >(&ins);
        auto* target = call ? callee(call) : nullptr;
        if (call) {
          require(target && target->isDeclaration() && target->getName() == "abort" &&
                  call->arg_empty() && call->getType()->isVoidTy() && binding(call));
          aborted = true;
        } else require(llvm::isa< llvm::UnreachableInst >(ins) ||
                       llvm::isa< llvm::DbgInfoIntrinsic >(ins));
      }
      require(aborted);
    } else require(body->getSinglePredecessor() == header);
    require(!reaches(loop.source_exit, header) &&
            !reaches(loop.source_exit, loop.source_pre));
    auto* bound = llvm::dyn_cast< llvm::Instruction >(loop.source_bound);
    require(!bound || (!llvm::isa< llvm::PHINode >(bound) &&
                       !llvm::isa< llvm::LoadInst >(bound) &&
                       facts.dominators(source_parent).dominates(bound, loop.phi)));
  }

  void object_imported_loop(Loop& loop, bool creating) {
    loop.address = object_shift(creating ? family.publish_ar->pointer()
                                        : family.object_load_ar->operand(), loop.gep);
    require(loop.address->num_terms() == 1 &&
            loop.address->term(0).first.to_z_number() == Z(word) &&
            loop.address->term(0).first.bit_width() == word * 8 &&
            loop.address->term(0).first.sign() == core::Unsigned &&
            source_integer_binding(loop.address->term(0).second, loop.gep->getOperand(1)));
    ar::Value* index = loop.address->term(0).second;
    for (unsigned n = 0; n < 32; ++n) {
      auto* v = dyn_cast< ar::InternalVariable >(index);
      auto* cast = v ? dyn_cast_or_null< ar::UnaryOperation >(unique_def(v)) : nullptr;
      if (!cast) break;
      require(cast->op() == ar::UnaryOperation::SExt ||
              cast->op() == ar::UnaryOperation::ZExt ||
              cast->op() == ar::UnaryOperation::Bitcast);
      require(cast->operand()->type()->is_integer());
      loop.adapters.push_back(cast); index = cast->operand();
    }
    loop.counter = dyn_cast< ar::InternalVariable >(index);
    require(loop.counter && source_integer_binding(loop.counter, loop.phi));
    loop.body = loop.call->parent();
    auto* first = creating ? family.birth_ar->parent() : loop.body;
    if (creating) family.birth_block_ar = first;
    require(loop.body->num_predecessors() == 1 && loop.body->num_successors() == 1 &&
            first->num_predecessors() == 1);
    loop.header = *first->predecessor_begin();
    require(*loop.body->successor_begin() == loop.header &&
            loop.header->num_predecessors() == 2 && loop.header->num_successors() == 2);
    for (auto it = loop.header->predecessor_begin(); it != loop.header->predecessor_end(); ++it)
      if (*it != loop.body) loop.pre = *it;
    for (auto it = loop.header->successor_begin(); it != loop.header->successor_end(); ++it)
      if (*it != first) loop.exit = *it;
    require(loop.pre && loop.exit && loop.pre != loop.exit &&
            loop.exit->num_predecessors() == 1 &&
            *loop.exit->predecessor_begin() == loop.header &&
            loop.pre->num_successors() == 1 && *loop.pre->successor_begin() == loop.header &&
            !reaches(loop.exit, loop.header) && !reaches(loop.exit, loop.pre));
    if (creating) {
      require(first != loop.body && first->num_successors() == 2 &&
              *loop.body->predecessor_begin() == first);
      auto* success = dyn_cast< ar::Comparison >(loop.body->front());
      require(success && success->predicate() == ar::Comparison::PNE &&
              success->left() == family.birth_ar->result() &&
              isa< ar::NullConstant >(success->right()));
      ar::BasicBlock* failed = nullptr;
      for (auto it = first->successor_begin(); it != first->successor_end(); ++it)
        if (*it != loop.body) failed = *it;
      require(failed && failed->num_predecessors() == 1 &&
              *failed->predecessor_begin() == first);
      bool tested = false, aborted = false, unreachable = false;
      for (auto* statement : *failed) {
        if (auto* cmp = dyn_cast< ar::Comparison >(statement)) {
          require(!tested && !aborted && cmp->predicate() == ar::Comparison::PEQ &&
                  cmp->left() == family.birth_ar->result() &&
                  isa< ar::NullConstant >(cmp->right()));
          tested = true;
        } else if (auto* call = dyn_cast< ar::CallBase >(statement)) {
          auto* target = native_handle_callee(call);
          require(tested && !aborted && target &&
                  target->intrinsic_id() == ar::Intrinsic::LibcAbort);
          aborted = true;
        } else { require(aborted && isa< ar::Unreachable >(statement)); unreachable = true; }
      }
      require(tested && aborted && unreachable);
    }
    ar::Assignment *init = nullptr, *update = nullptr;
    for (auto* block : *parent->body()) for (auto* statement : *block) {
      spend();
      if (statement->result_or_null() != loop.counter) continue;
      auto* assign = dyn_cast< ar::Assignment >(statement);
      require(assign);
      if (block == loop.pre && !init) init = assign;
      else if (block == loop.body && !update) update = assign;
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
    const bool sign = loop.source_condition->isSigned();
    auto* guard = dyn_cast< ar::Comparison >(first->front());
    auto* exit_guard = dyn_cast< ar::Comparison >(loop.exit->front());
    require(guard && guard->predicate() == (sign ? ar::Comparison::SILT : ar::Comparison::UILT) &&
            source_integer_binding(guard->left(), loop.phi) &&
            source_integer_binding(guard->right(), loop.source_bound) &&
            exit_guard &&
            exit_guard->predicate() == (sign ? ar::Comparison::SIGE : ar::Comparison::UIGE) &&
            source_integer_binding(exit_guard->left(), loop.phi) &&
            source_integer_binding(exit_guard->right(), loop.source_bound));
    loop.bound = guard->right();
    for (auto* statement : *loop.header) require(isa< ar::UnaryOperation >(statement));
    for (auto* block : {first, loop.body}) {
      for (auto* statement : *block) {
        spend();
        if (statement == guard || statement == update || statement == loop.step ||
            statement == loop.call || (creating && statement == family.birth_ar) ||
            (creating && statement == family.publish_ar) ||
            (!creating && (statement == family.object_load_ar ||
                           statement == join.load || statement == family.release_load_ar ||
                           statement == family.own_free_ar))) continue;
        if (creating && block == loop.body && statement == loop.body->front()) continue;
        if (auto* call = dyn_cast< ar::CallBase >(statement)) {
          auto* native = call->has_frontend()
              ? llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >()) : nullptr;
          require(native && frozen_seed(native)); continue;
        }
        require(isa< ar::UnaryOperation >(statement) || isa< ar::PointerShift >(statement));
      }
    }
    if (creating) {
      require(statement_dominates(family.birth_ar, family.publish_ar) &&
              statement_dominates(family.publish_ar, loop.call) &&
              statement_dominates(loop.call, loop.step));
    } else {
      require(statement_dominates(family.object_load_ar, join.load) &&
              statement_dominates(join.load, loop.call) &&
              statement_dominates(loop.call, family.release_load_ar) &&
              statement_dominates(family.release_load_ar, family.own_free_ar) &&
              statement_dominates(family.own_free_ar, loop.step));
    }
    auto* bound = dyn_cast_or_null< ar::InternalVariable >(uncast(loop.bound));
    if (bound) {
      auto* def = unique_def(bound);
      require(def && def->parent() != loop.header && def->parent() != loop.body &&
              def->parent() != first && dominates(def->parent(), loop.pre));
    } else require(isa< ar::IntegerConstant >(loop.bound));
  }

  // Bidirectional use closure: all aliases are transparent, uniquely defined
  // pointer copies. Allowed edges are explicit and operand-sensitive.
  using SourceEdges = std::map< llvm::User*, std::set< unsigned > >;
  using ImportedEdges = std::map< ar::Statement*, std::set< ar::Value* > >;
  void object_close(llvm::Value* native_root, ar::Value* imported_root,
                    const SourceEdges& native_edges, const ImportedEdges& imported_edges,
                    bool null_tests = false) {
    std::vector< llvm::Value* > todo{native_root};
    std::set< llvm::Value* > native_aliases;
    while (!todo.empty()) {
      auto* value = todo.back(); todo.pop_back(); spend();
      if (!native_aliases.insert(value).second) continue;
      for (auto* user : value->users()) {
        spend();
        if (auto* cast = llvm::dyn_cast< llvm::BitCastInst >(user)) {
          require(cast->getType()->isPointerTy() && cast->getType()->getPointerAddressSpace() == 0 &&
                  cast->getFunction() == (llvm::isa< llvm::Argument >(native_root)
                      ? source_child : llvm::cast< llvm::Instruction >(native_root)->getFunction()));
          todo.push_back(cast); continue;
        }
        if (null_tests) if (auto* cmp = llvm::dyn_cast< llvm::ICmpInst >(user)) {
          require(cmp->isEquality() && cmp->getOperand(0) == value &&
                  llvm::isa< llvm::ConstantPointerNull >(cmp->getOperand(1)));
          continue;
        }
        auto found = native_edges.find(user);
        require(found != native_edges.end());
        for (unsigned n = 0; n < user->getNumOperands(); ++n)
          if (user->getOperand(n) == value) require(found->second.count(n));
      }
    }
    std::set< ar::Value* > aliases{imported_root};
    auto* root_var = dyn_cast< ar::InternalVariable >(imported_root);
    require(root_var && (imported_root == child->param(0) || unique_def(root_var)));
    bool changed = true;
    while (changed) {
      changed = false;
      for (auto* block : *root_var->code()) for (auto* statement : *block) {
        spend(); ar::Value* operand = nullptr;
        if (auto* copy = dyn_cast< ar::Assignment >(statement)) operand = copy->operand();
        if (auto* cast = dyn_cast< ar::UnaryOperation >(statement))
          if (cast->op() == ar::UnaryOperation::Bitcast &&
              cast->result()->type()->is_pointer()) operand = cast->operand();
        if (!operand || !aliases.count(operand)) continue;
        auto* result = dyn_cast_or_null< ar::InternalVariable >(statement->result_or_null());
        require(result && result->type()->is_pointer() && unique_def(result) == statement);
        changed |= aliases.insert(result).second;
      }
    }
    auto close_code = [&](ar::Code* code) {
      for (auto* block : *code) for (auto* statement : *block) {
        spend();
        for (auto it = statement->op_begin(); it != statement->op_end(); ++it) {
          if (!aliases.count(*it)) continue;
          if (statement->result_or_null() && aliases.count(statement->result_or_null())) continue;
          if (null_tests) if (auto* cmp = dyn_cast< ar::Comparison >(statement)) {
            require((cmp->predicate() == ar::Comparison::PEQ ||
                     cmp->predicate() == ar::Comparison::PNE) &&
                    cmp->left() == *it && isa< ar::NullConstant >(cmp->right()));
            continue;
          }
          auto found = imported_edges.find(statement);
          require(found != imported_edges.end() && found->second.count(*it));
        }
      }
    };
    for (auto it = ctx.bundle->function_begin(); it != ctx.bundle->function_end(); ++it)
      if ((*it)->is_definition()) close_code((*it)->body());
    for (auto it = ctx.bundle->global_begin(); it != ctx.bundle->global_end(); ++it)
      if ((*it)->is_definition()) close_code((*it)->initializer());
  }

  void object_worker() {
    require(source_child->size() == 1);
    SourceEdges native_edges;
    ImportedEdges imported_edges;
    std::set< ar::Statement* > accepted;
    unsigned native_returns = 0, imported_returns = 0;
    for (auto& block : *source_child) for (auto& ins : block) {
      spend();
      if (auto* ret = llvm::dyn_cast< llvm::ReturnInst >(&ins)) {
        require(llvm::isa< llvm::ConstantPointerNull >(ret->getReturnValue()));
        ++native_returns; continue;
      }
      auto* load = llvm::dyn_cast< llvm::LoadInst >(&ins);
      auto* store = llvm::dyn_cast< llvm::StoreInst >(&ins);
      if (load || store) {
        require(load ? load->isSimple() : store->isSimple());
        auto* pointer = load ? load->getPointerOperand() : store->getPointerOperand();
        auto* type = load ? load->getType() : store->getValueOperand()->getType();
        require(type->isIntegerTy() && type->getIntegerBitWidth() > 0 &&
                type->getIntegerBitWidth() <= 64 && type->getIntegerBitWidth() % 8 == 0);
        const auto width = type->getIntegerBitWidth() / 8;
        auto* statement = original(&ins);
        auto* ar_load = dyn_cast_or_null< ar::Load >(statement);
        auto* ar_store = dyn_cast_or_null< ar::Store >(statement);
        require((bool(load) == bool(ar_load)) && (bool(store) == bool(ar_store)) &&
                statement && statement->code() == child->body());
        auto* address = ar_load ? ar_load->operand() : ar_store->pointer();
        auto* ar_type = ar_load ? ar_load->result()->type() : ar_store->value()->type();
        require(ar_type->is_integer() &&
                cast< ar::IntegerType >(ar_type)->bit_width() == width * 8 &&
                !(ar_load ? ar_load->is_atomic() || ar_load->is_volatile()
                          : ar_store->is_atomic() || ar_store->is_volatile()));
        if (auto* global = llvm::dyn_cast< llvm::GlobalVariable >(pointer->stripPointerCasts())) {
          auto* actual = dyn_cast_or_null< ar::GlobalVariable >(uncast(address));
          require(actual && actual->has_frontend() &&
                  actual->frontend< llvm::Value >() == global);
          family.globals.insert(statement);
        } else {
          // Initial scope is scalar writes; an own-object read additionally
          // needs an initializedness certificate and is intentionally refused.
          require(store);
          auto* gep = object_gep(pointer);
          auto* shift = object_shift(address, gep);
          const auto offset = object_field(gep, shift, source_child->getArg(0),
                                            child->param(0), width);
          require(offset + width <= family.tid_offset ||
                  family.tid_offset + word <= offset);
          require(store->getAlign().value() <=
                  source_parent->getParent()->getDataLayout().getABITypeAlign(family.type).value() &&
                  offset % store->getAlign().value() == 0);
          family.fields.emplace(statement, ObjectFamily::Field{offset, width});
          native_edges[gep].insert(0);
          imported_edges[shift].insert(shift->pointer());
          object_close(gep, shift->result(), {{store, {1}}}, {{statement, {address}}});
        }
        accepted.insert(statement); continue;
      }
      if (auto* call = llvm::dyn_cast< llvm::CallBase >(&ins)) {
        auto* target = callee(call);
        auto* actual = binding(call);
        require(target && target->isDeclaration() && actual &&
                actual->code() == child->body());
        const auto name = target->getName();
        if (name == "pthread_mutex_lock" || name == "pthread_mutex_unlock") {
          require(call->arg_size() == 1 &&
                  llvm::isa< llvm::GlobalVariable >(call->getArgOperand(0)->stripPointerCasts()));
        } else require(frozen_seed(call) ||
                       ((name == "__VERIFIER_nondet_int" || name == "__VERIFIER_nondet_uint") &&
                        call->arg_empty() && call->getType()->isIntegerTy(32)));
        accepted.insert(actual); continue;
      }
      require(llvm::isa< llvm::BitCastInst >(ins) ||
              llvm::isa< llvm::GetElementPtrInst >(ins) ||
              llvm::isa< llvm::BinaryOperator >(ins) ||
              llvm::isa< llvm::CastInst >(ins) || llvm::isa< llvm::DbgInfoIntrinsic >(ins));
    }
    for (auto* block : *child->body()) for (auto* statement : *block) {
      spend();
      if (accepted.count(statement)) continue;
      if (auto* ret = dyn_cast< ar::ReturnValue >(statement)) {
        require(ret->has_operand() && isa< ar::NullConstant >(ret->operand()));
        ++imported_returns; continue;
      }
      require(isa< ar::UnaryOperation >(statement) || isa< ar::BinaryOperation >(statement) ||
              isa< ar::PointerShift >(statement));
    }
    require(native_returns == 1 && imported_returns == 1);
    object_close(source_child->getArg(0), child->param(0), native_edges, imported_edges);
  }

  void object_storage() {
    // All source and imported alias graphs close independently. The two join
    // table reloads name the same immutable cell; MAY heap sites are not used
    // to equate their dynamic objects.
    object_close(source_allocation, base,
        {{create.gep, {0}}, {join.gep, {0}}, {family.release_cell, {0}},
         {family.table_free, {0}}},
        {{create.address, {create.address->pointer()}},
         {join.address, {join.address->pointer()}},
         {family.release_cell_ar, {family.release_cell_ar->pointer()}},
         {family.table_free_ar, {family.table_free_ar->argument(0)}}}, true);
    object_close(create.gep, create.address->result(),
        {{family.publish, {1}}}, {{family.publish_ar, {family.publish_ar->pointer()}}});
    object_close(join.gep, join.address->result(),
        {{family.object_load, {0}}}, {{family.object_load_ar, {family.object_load_ar->operand()}}});
    object_close(family.release_cell, family.release_cell_ar->result(),
        {{family.release_load, {0}}}, {{family.release_load_ar, {family.release_load_ar->operand()}}});
    object_close(family.birth, family.birth_ar->result(),
        {{family.publish, {0}}, {family.create_tid, {0}}, {create.source, {3}}},
        {{family.publish_ar, {family.publish_ar->value()}},
         {family.create_tid_ar, {family.create_tid_ar->pointer()}},
         {create.call, {create.call->argument(3)}}}, true);
    object_close(family.create_tid, family.create_tid_ar->result(),
        {{create.source, {0}}}, {{create.call, {create.call->argument(0)}}});
    object_close(family.object_load, family.object_load_ar->result(),
        {{family.join_tid, {0}}}, {{family.join_tid_ar, {family.join_tid_ar->pointer()}}});
    object_close(family.join_tid, family.join_tid_ar->result(),
        {{join.source_load, {0}}}, {{join.load, {join.load->operand()}}});
    object_close(family.release_load, family.release_load_ar->result(),
        {{family.own_free, {0}}}, {{family.own_free_ar, {family.own_free_ar->argument(0)}}});
    // Native handles are opaque and only the actual typed load reaches join.
    require(join.source_load->hasOneUse() && *join.source_load->user_begin() == join.source);
    for (auto* block : *parent->body()) for (auto* statement : *block)
      for (auto it = statement->op_begin(); it != statement->op_end(); ++it)
        if (*it == join.load->result())
          require(statement == join.call && join.call->argument(0) == *it);
  }

  void object_prepare() {
    require(!std::getenv("IKOS_RACE_DISABLE_INDEXED_OBJECT_PRODUCER"));
    family.active = true;
    stage = "object family source/AR identities";
    require(object_create_api(create.source) && native_handle_api(join.source, false));
    stage = "object family imported call identities";
    create.call = binding(create.source); join.call = binding(join.source);
    require(create.call && join.call && create.call->num_arguments() == 4 &&
            join.call->num_arguments() == 2 && create.call->code() == parent->body() &&
            join.call->code() == parent->body() &&
            isa< ar::NullConstant >(create.call->argument(1)) &&
            isa< ar::NullConstant >(join.call->argument(1)));
    auto* output_pointer = dyn_cast< ar::PointerType >(create.call->argument(0)->type());
    auto* output_integer = output_pointer ? dyn_cast< ar::IntegerType >(output_pointer->pointee()) : nullptr;
    require(output_integer && output_integer->bit_width() == word * 8 &&
            create.call->argument(3)->type()->is_pointer());
    auto* actual_entry = dyn_cast< ar::FunctionPointerConstant >(create.call->argument(2));
    require(actual_entry && actual_entry->function() == child &&
            synchronization_ignored_arguments(create.call, native_handle_callee(create.call)) == 12);
    stage = "object family allocation identity";
    family.birth = llvm::dyn_cast< llvm::CallBase >(
        create.source->getArgOperand(3)->stripPointerCasts());
    family.birth_ar = family.birth ? binding(family.birth) : nullptr;
    auto* allocator = family.birth ? callee(family.birth) : nullptr;
    auto* native_allocator = family.birth_ar ? native_handle_callee(family.birth_ar) : nullptr;
    require(allocator && allocator->isDeclaration() && allocator->getName() == "malloc" &&
            family.birth->arg_size() == 1 && family.birth_ar && family.birth_ar->has_result() &&
            family.birth_ar->num_arguments() == 1 && family.birth_ar->result()->type()->is_pointer() &&
            native_allocator &&
            native_allocator->intrinsic_id() == ar::Intrinsic::LibcMalloc &&
            family.birth->getFunction() == source_parent &&
            family.birth_ar->code() == parent->body() &&
            uncast(create.call->argument(3)) == family.birth_ar->result());
    stage = "object family allocation extent";
    auto* size = llvm::dyn_cast< llvm::ConstantInt >(family.birth->getArgOperand(0));
    auto* actual_size = dyn_cast< ar::IntegerConstant >(family.birth_ar->argument(0));
    require(size && size->getBitWidth() == word * 8 && !size->isZero() &&
            size->getValue().getActiveBits() <= 63 && actual_size &&
            actual_size->value().to_z_number() == Z(size->getZExtValue()));
    family.size = size->getZExtValue();
    stage = "object family typed output layout";
    family.create_tid = object_gep(create.source->getArgOperand(0));
    family.type = family.create_tid
        ? llvm::dyn_cast< llvm::StructType >(family.create_tid->getSourceElementType()) : nullptr;
    require(family.type && !family.type->isOpaque() && !family.type->isPacked());
    const auto& dl = source_parent->getParent()->getDataLayout();
    auto allocation_size = dl.getTypeAllocSize(family.type);
    require(!allocation_size.isScalable() && allocation_size.getFixedValue() == family.size);
    stage = "object family imported output layout";
    family.create_tid_ar = object_shift(create.call->argument(0), family.create_tid);
    family.tid_offset = object_field(family.create_tid, family.create_tid_ar,
                                      family.birth, family.birth_ar->result(), word);
    require(family.tid_offset % word == 0 && dl.getABITypeAlign(family.type).value() >= word);
    stage = "object family table publication";
    for (auto& block : *source_parent) for (auto& ins : block) {
      auto* store = llvm::dyn_cast< llvm::StoreInst >(&ins);
      if (store && store->getValueOperand()->getType()->isPointerTy() &&
          store->getValueOperand()->stripPointerCasts() == family.birth) {
        require(!family.publish); family.publish = store;
      }
    }
    require(family.publish && family.publish->isSimple() &&
            family.publish->getAlign().value() >= word &&
            family.publish->getParent() == create.source->getParent() &&
            family.publish->comesBefore(create.source));
    family.publish_ar = dyn_cast_or_null< ar::Store >(original(family.publish));
    require(family.publish_ar && !family.publish_ar->is_atomic() &&
            !family.publish_ar->is_volatile() && family.publish_ar->alignment() >= word &&
            uncast(family.publish_ar->value()) == family.birth_ar->result() &&
            family.publish_ar->value()->type()->is_pointer());
    create.gep = object_gep(family.publish->getPointerOperand());
    stage = "object family join handle identity";
    join.source_load = llvm::dyn_cast< llvm::LoadInst >(join.source->getArgOperand(0));
    require(join.source_load && join.source_load->isSimple() &&
            join.source_load->getType()->isIntegerTy(word * 8) &&
            join.source_load->getAlign().value() >= word);
    join.load = dyn_cast_or_null< ar::Load >(original(join.source_load));
    require(join.load && !join.load->is_atomic() && !join.load->is_volatile() &&
            join.load->alignment() >= word && join.call->argument(0) == join.load->result() &&
            join.load->result()->type()->is_integer() &&
            cast< ar::IntegerType >(join.load->result()->type())->bit_width() == word * 8);
    stage = "object family join object identity";
    family.join_tid = object_gep(join.source_load->getPointerOperand());
    family.object_load = family.join_tid ? llvm::dyn_cast< llvm::LoadInst >(
        family.join_tid->getPointerOperand()->stripPointerCasts()) : nullptr;
    require(family.object_load && family.object_load->isSimple() &&
            family.object_load->getType()->isPointerTy() &&
            family.object_load->getAlign().value() >= word);
    family.object_load_ar = dyn_cast_or_null< ar::Load >(original(family.object_load));
    require(family.object_load_ar && !family.object_load_ar->is_atomic() &&
            !family.object_load_ar->is_volatile() &&
            family.object_load_ar->alignment() >= word &&
            family.object_load_ar->result()->type()->is_pointer());
    join.gep = object_gep(family.object_load->getPointerOperand());
    family.join_tid_ar = object_shift(join.load->operand(), family.join_tid);
    require(object_field(family.join_tid, family.join_tid_ar, family.object_load,
                          family.object_load_ar->result(), word) == family.tid_offset);
    stage = "object family release identity";
    for (auto& ins : *join.source->getParent()) {
      auto* call = llvm::dyn_cast< llvm::CallBase >(&ins);
      auto* target = call ? callee(call) : nullptr;
      if (!target || target->getName() != "free") continue;
      require(!family.own_free && target->isDeclaration() && call->arg_size() == 1);
      family.own_free = call;
    }
    require(family.own_free && family.own_free->getType()->isVoidTy() &&
            join.source->comesBefore(family.own_free));
    family.own_free_ar = binding(family.own_free);
    require(family.own_free_ar && family.own_free_ar->num_arguments() == 1 &&
            native_handle_callee(family.own_free_ar) &&
            native_handle_callee(family.own_free_ar)->intrinsic_id() == ar::Intrinsic::LibcFree);
    family.release_load = llvm::dyn_cast< llvm::LoadInst >(
        family.own_free->getArgOperand(0)->stripPointerCasts());
    require(family.release_load && family.release_load != family.object_load &&
            family.release_load->isSimple() && family.release_load->getType()->isPointerTy() &&
            family.release_load->getAlign().value() >= word);
    family.release_load_ar = dyn_cast_or_null< ar::Load >(original(family.release_load));
    require(family.release_load_ar && !family.release_load_ar->is_atomic() &&
            !family.release_load_ar->is_volatile() &&
            family.release_load_ar->alignment() >= word &&
            family.release_load_ar->result()->type()->is_pointer() &&
            uncast(family.own_free_ar->argument(0)) == family.release_load_ar->result());
    family.release_cell = object_gep(family.release_load->getPointerOperand());
    for (auto* gep : {create.gep, join.gep, family.release_cell})
      require(gep && gep->getNumIndices() == 1 && gep->getAddressSpace() == 0 &&
              gep->getSourceElementType()->isPointerTy() &&
              gep->getSourceElementType()->getPointerElementType() == family.type);
    require(object_same_index(family.release_cell->getOperand(1), join.gep->getOperand(1)) &&
            family.release_cell->getPointerOperand() == join.gep->getPointerOperand());
    stage = "object family source loop topology";
    object_source_loop(create, true);
    stage = "object family source join loop topology";
    object_source_loop(join, false);
    require(create.source_bound == join.source_bound &&
            create.source_condition->getPredicate() == join.source_condition->getPredicate() &&
            facts.dominators(source_parent).dominates(create.source_exit, join.source_pre));
    stage = "object family actual AR loop topology";
    object_imported_loop(create, true); object_imported_loop(join, false);
    require(same_integer(create.bound, join.bound) && dominates(create.exit, join.pre));
    family.release_cell_ar = object_shift(family.release_load_ar->operand(), family.release_cell);
    require(family.release_cell_ar->num_terms() == 1 &&
            family.release_cell_ar->term(0).first == join.address->term(0).first &&
            source_integer_binding(family.release_cell_ar->term(0).second,
                                   family.release_cell->getOperand(1)) &&
            object_same_index(family.release_cell_ar->term(0).second,
                              join.address->term(0).second) &&
            uncast(family.release_cell_ar->pointer()) == uncast(join.address->pointer()));
    nonnull_before(family.birth, create.source, family.birth_ar, create.call);
    stage = "object family stable table and extent";
    heap_factory(); prepare_bound_guards();
    // N itself must be defined outside all revisited blocks, not just be a
    // source instruction with a convenient name or frontend annotation.
    auto* native_bound = llvm::dyn_cast< llvm::Instruction >(create.source_bound);
    if (native_bound) for (auto* next : llvm::successors(native_bound->getParent()))
      require(!reaches(next, native_bound->getParent()));
    auto* bound_var = dyn_cast_or_null< ar::InternalVariable >(uncast(create.bound));
    auto* bound_def = bound_var ? unique_def(bound_var) : nullptr;
    if (bound_def) for (auto it = bound_def->parent()->successor_begin();
                       it != bound_def->parent()->successor_end(); ++it)
      require(!reaches(*it, bound_def->parent()));
    for (auto& block : *source_parent) for (auto& ins : block) {
      auto* call = llvm::dyn_cast< llvm::CallBase >(&ins);
      auto* target = call ? callee(call) : nullptr;
      if (target && target->getName() == "free" && call->arg_size() == 1 &&
          call->getArgOperand(0)->stripPointerCasts() == source_allocation) {
        require(!family.table_free && target->isDeclaration() && call->getType()->isVoidTy());
        family.table_free = call;
      }
    }
    require(family.table_free && after_join(family.table_free));
    family.table_free_ar = binding(family.table_free);
    require(family.table_free_ar && family.table_free_ar->num_arguments() == 1 &&
            native_handle_callee(family.table_free_ar) &&
            native_handle_callee(family.table_free_ar)->intrinsic_id() == ar::Intrinsic::LibcFree &&
            uncast(family.table_free_ar->argument(0)) == base && after_join(family.table_free_ar));
    stage = "object family complete effect and activation inventory"; inventory();
    stage = "object family worker field/global effects"; object_worker();
    stage = "object family source and AR use closure"; object_storage();
    effect_frame = true; prepared = true;
  }

  void object_observe(ar::Statement* statement, const value::AbstractDomain& inv,
                      CallContext* cc) {
    if (statement == family.publish_ar || statement == family.object_load_ar) {
      stage = "object family live table cell observation";
      observation(statement == family.publish_ar ? create : join, statement, inv, cc);
    }
    for (auto* loop : {&create, &join}) if (statement == loop->step) {
      stage = "object family raw counter observation"; observation(*loop, statement, inv, cc);
    }
    if (statement == create.call || statement == join.call ||
        statement == family.own_free_ar || statement == family.table_free_ar) {
      require(!cc || !cc->has_parent());
      if (statement == create.call) family.create_seen = true;
      if (statement == join.call) family.join_seen = true;
      if (statement == family.own_free_ar) family.own_free_seen = true;
      if (statement == family.table_free_ar) family.table_free_seen = true;
    }
  }
  bool object_record(const AccessRecord& record, ar::Statement* statement,
                     ar::Function* root, AccessKind kind, std::uint64_t width) const {
    return record.stmt == statement && record.stmt && record.stmt->code() == root->body() &&
        record.thread_id == root->name() && record.kind == kind && record.width == width &&
        !(record.call_context && record.call_context->has_parent());
  }
  bool object_child_field(const AccessRecord& record) const {
    auto it = family.fields.find(record.stmt);
    return it != family.fields.end() &&
        object_record(record, record.stmt, child, AccessKind::Write, it->second.width);
  }
  bool object_child_global(const AccessRecord& record) const {
    return family.globals.count(record.stmt) && endpoint(record, false);
  }
  bool object_table_record(const AccessRecord& record) const {
    return object_record(record, family.publish_ar, parent, AccessKind::Write, word) ||
        object_record(record, family.object_load_ar, parent, AccessKind::Read, word) ||
        object_record(record, family.release_load_ar, parent, AccessKind::Read, word);
  }
  bool object_tid_record(const AccessRecord& record) const {
    if (object_record(record, join.load, parent, AccessKind::Read, word)) return true;
    // check_extern_call emits a width-zero write for each unignored pointer
    // argument. Exact native API + ignored mask 12 + NULL attributes leaves
    // precisely arg0. This does not reinterpret any other zero-width call.
    return object_record(record, create.call, parent, AccessKind::Write, 0) &&
        synchronization_ignored_arguments(create.call, native_handle_callee(create.call)) == 12;
  }
  unsigned object_reason(const AccessRecord& a, const AccessRecord& b) const {
    if (!ready() || !family.active) return 0;
    if (!std::getenv("IKOS_RACE_DISABLE_INDEXED_OBJECT_COMPLETION") &&
        ((endpoint(a, true) && endpoint(b, false)) ||
         (endpoint(b, true) && endpoint(a, false)))) return 1;
    const bool ac = object_child_field(a), bc = object_child_field(b);
    const bool ag = object_child_global(a), bg = object_child_global(b);
    if ((object_table_record(a) && (bc || bg)) ||
        (object_table_record(b) && (ac || ag)) ||
        (object_tid_record(a) && bg) || (object_tid_record(b) && ag)) return 2;
    if (std::getenv("IKOS_RACE_DISABLE_INDEXED_OBJECT_FIELDS")) return 0;
    if ((object_tid_record(a) && bc) || (object_tid_record(b) && ac)) return 3;
    if (ac && bc) return 4;
    return 0;
  }
  JsonDict object_info(const AccessRecord& a, const AccessRecord& b) const {
    const auto reason = object_reason(a, b);
    const char* contracts[] = {"invalid", "indexed-object-family-completion-v1",
      "indexed-object-family-frame-v1", "indexed-object-family-fields-v1",
      "indexed-object-family-sibling-v1"};
    const char* reasons[] = {"no certificate",
      "each successful child completes at its own returning join before the parent endpoint",
      "closed pointer table or typed handle object is disjoint from the worker endpoint",
      "actual native create output or join load occupies a disjoint typed handle field",
      "distinct child activations receive distinct simultaneously live allocation instances"};
    return {{"thread_lifecycle_contract", contracts[reason]}, {"safe_reason", reasons[reason]},
            {"thread_a", a.thread_id}, {"thread_b", b.thread_id},
            {"object_size_bytes", family.size}, {"tid_offset_bytes", family.tid_offset},
            {"handle_width_bytes", word}, {"table_stride_bytes", word},
            {"extent_count_upper", extent_count_upper.str()},
            {"coordinate_contract", "same-N full pointer cells; pre-multiply extent proof; source and AR exact 0,+1,<N"},
            {"provenance_contract", "one nonNULL birth per create iteration; exact actual argument; closed invariant table; MAY points-to never supplies MUST identity"},
            {"lifetime_contract", "all births precede joins; each own join precedes its own free; complete joins precede table free"},
            {"scope", "ordinary GNU conditional completion; no numeric-ID uniqueness claim; no worker payload reads; no coverage waiver"}};
  }
