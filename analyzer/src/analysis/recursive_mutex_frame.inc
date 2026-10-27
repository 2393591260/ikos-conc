// Included in PthreadInitializationFrame::Impl. This is a separate finite
// source capability: it never widens the existing heap/array frame policies.
  std::map< ar::CallBase*, PthreadRecursiveMutexEffect > recursive_effects;
  std::map< std::pair< ar::GlobalVariable*, ar::Function* >, PthreadRecursiveMutexRegion > recursive_regions;
  std::set< ar::GlobalVariable* > recursive_objects;
  std::set< ar::LocalVariable* > recursive_attributes;

  void recursive_run() {
    need(std::getenv("IKOS_RACE_DISABLE_RECURSIVE_MUTEX") == nullptr,
         "recursive mutex source facts disabled");
    need(module->alias_empty() && mutex_effect_pointer_representations_closed(*module),
         "recursive closed pointer representations");
    std::set< llvm::Function* > roots{main};
    std::set< llvm::CallBase* > creations;
    // A defined root is entered only as main or as an actual pthread callback.
    // Synchronous calls and indirect/recursive activation cannot borrow depth0.
    for (auto& f : *module) for (auto& b : f) for (auto& i : b) {
      step(); auto* c = llvm::dyn_cast< llvm::CallBase >(&i);
      auto* target = c ? llvm::dyn_cast< llvm::Function >(c->getCalledOperand()->stripPointerCasts()) : nullptr;
      if (!target || target->getName() != "pthread_create") continue;
      auto* a = binding(c);
      need(a && c->getFunction() == main && native_handle_api(c, true) &&
           native_handle_callee(a) && native_handle_callee(a)->is_declaration() &&
           value(a->argument(2), c->getArgOperand(2)) &&
           llvm::isa< llvm::ConstantPointerNull >(c->getArgOperand(1)) &&
           llvm::isa< llvm::ConstantPointerNull >(c->getArgOperand(3)) &&
           isa< ar::NullConstant >(a->argument(1)) && isa< ar::NullConstant >(a->argument(3)),
           "recursive direct main creation");
      auto* child = llvm::dyn_cast< llvm::Function >(c->getArgOperand(2)->stripPointerCasts());
      need(child && !child->isDeclaration() && child != main && functions.count(child),
           "recursive actual callback body");
      roots.insert(child); creations.insert(c);
    }
    need(roots.size() > 1 && main->getReturnType()->isIntegerTy(32), "recursive roots");
    for (const auto& item : functions) {
      auto* f = item.first;
      need(roots.count(f) && f->size() == 1 &&
           llvm::isa< llvm::ReturnInst >(f->getEntryBlock().getTerminator()) &&
           std::distance(item.second->body()->begin(), item.second->body()->end()) == 1 &&
           !f->isVarArg() && f->getCallingConv() == llvm::CallingConv::C &&
           ordinary_source_function_abi_attributes(f), "recursive single-block complete roots");
      if (f == main) continue;
      need(f->arg_size() == 1 && f->getArg(0)->getType()->isPointerTy() &&
           f->getReturnType()->isPointerTy(), "recursive callback ABI");
      for (auto* user : f->users()) {
        auto* c = llvm::dyn_cast< llvm::CallBase >(user);
        need(c && creations.count(c) && c->getArgOperand(2) == f,
             "recursive callback activation closure");
      }
    }
    std::map< llvm::Value*, ar::Value* > storage;
    std::map< llvm::Value*, unsigned > kinds;
    std::set< llvm::Value* > initialized_attrs, initialized_mutexes;
    std::set< ar::CallBase* > lifecycle;
    llvm::Instruction* occurrence = nullptr;
    std::set< ar::Statement* > covered;
    auto base = [&](llvm::Value* value) {
      for (unsigned depth = 0; depth < 32; ++depth) {
        step();
        if (auto* cast = llvm::dyn_cast< llvm::BitCastOperator >(value)) value = cast->getOperand(0);
        else if (auto* gep = llvm::dyn_cast< llvm::GEPOperator >(value)) {
          need(gep->getPointerAddressSpace() == 0 && gep->hasAllZeroIndices(), "recursive whole object alias");
          value = gep->getPointerOperand();
        } else return value;
      }
      need(false, "recursive alias depth"); return value;
    };
    auto object = [&](llvm::Value* actual, ar::Value* imported, bool mutex) -> ar::Value* {
      auto* source = base(actual); ar::Value* result = nullptr; llvm::Type* native_type = nullptr;
      const unsigned bytes = mutex ? abi.mutex : 4, alignment = mutex ? abi.word : 4;
      if (mutex) {
        auto* g = llvm::dyn_cast< llvm::GlobalVariable >(source);
        need(g && globals.count(g) && g->hasInitializer() && !g->isConstant() && !g->isThreadLocal() &&
             !g->isExternallyInitialized() && g->getAddressSpace() == 0 &&
             globals.at(g)->is_definition() && !globals.at(g)->is_thread_local(), "recursive global mutex object");
        result = globals.at(g); native_type = g->getValueType();
        need((g->getAlign() ? g->getAlign()->value() : module->getDataLayout().getABITypeAlign(native_type).value()) >= alignment &&
             globals.at(g)->alignment() >= alignment, "recursive global alignment");
      } else {
        auto* local = llvm::dyn_cast< llvm::AllocaInst >(source);
        auto* allocation = local ? dyn_cast_or_null< ar::Allocate >(original(local)) : nullptr;
        auto* count = local ? llvm::dyn_cast< llvm::ConstantInt >(local->getArraySize()) : nullptr;
        auto* imported_count = allocation ? dyn_cast< ar::IntegerConstant >(allocation->array_size()) : nullptr;
        need(local && local->getFunction() == main && local->getParent() == &main->getEntryBlock() &&
             local->getAddressSpace() == 0 && local->getAlign().value() >= alignment &&
             occurrence && occurrence->getFunction() == main && local->comesBefore(occurrence) &&
             count && count->isOne() && allocation && unique_allocation_definition(allocation) &&
             imported_count && imported_count->value().to_z_number() == 1 &&
             allocation->result()->alignment() >= alignment, "recursive initial main attribute allocation");
        result = allocation->result(); native_type = local->getAllocatedType();
        recursive_attributes.insert(allocation->result());
      }
      auto* native = llvm::dyn_cast< llvm::StructType >(native_type);
      auto* pointer = dyn_cast< ar::PointerType >(result->type());
      auto* ar_type = pointer ? pointer->pointee() : nullptr;
      need(native && !native->isOpaque() && native->hasName() &&
           native->getName() == (mutex ? "union.pthread_mutex_t" : "union.pthread_mutexattr_t") &&
           native_leaf(actual, mutex ? "union.pthread_mutex_t" : "union.pthread_mutexattr_t", bytes) &&
           module->getDataLayout().getTypeAllocSize(native) == bytes &&
           module->getDataLayout().getTypeStoreSize(native) == bytes &&
           module->getDataLayout().getABITypeAlign(native).value() == alignment &&
           ar_type && ar_type->is_struct() &&
           ctx.bundle->data_layout().alloc_size_in_bytes(ar_type) == bytes &&
           ctx.bundle->data_layout().store_size_in_bytes(ar_type) == bytes &&
           ctx.bundle->data_layout().abi_alignment(ar_type) == alignment &&
           exact_handle_slot(imported, result), "recursive native/AR exact declared storage");
      storage.emplace(source, result); return result;
    };
    std::vector< llvm::Function* > order{main};
    for (auto* f : roots) if (f != main) order.push_back(f);
    bool created = false;
    for (auto* f : order) {
      std::map< ar::GlobalVariable*, unsigned > depth;
      for (auto& i : f->getEntryBlock()) {
        step();
        if (llvm::isa< llvm::DbgInfoIntrinsic >(i)) continue;
        if (auto* allocation = llvm::dyn_cast< llvm::AllocaInst >(&i)) {
          auto* a = dyn_cast_or_null< ar::Allocate >(original(allocation));
          auto* n = llvm::dyn_cast< llvm::ConstantInt >(allocation->getArraySize());
          auto* an = a ? dyn_cast< ar::IntegerConstant >(a->array_size()) : nullptr;
          need(a && n && n->isOne() && an && an->value().to_z_number() == 1 &&
               allocation->getAddressSpace() == 0 && unique_allocation_definition(a) &&
               value(a->result(), allocation) &&
               ctx.bundle->data_layout().alloc_size_in_bytes(a->allocated_type()) ==
                   module->getDataLayout().getTypeAllocSize(allocation->getAllocatedType()).getFixedValue(),
               "recursive source/AR stack object");
          covered.insert(a); continue;
        }
        auto* c = llvm::dyn_cast< llvm::CallBase >(&i);
        if (!c) {
          if (llvm::isa< llvm::LoadInst >(i) || llvm::isa< llvm::StoreInst >(i)) {
            auto* a = original(&i); need(a != nullptr, "recursive actual scalar memory"); covered.insert(a);
          } else need(llvm::isa< llvm::BinaryOperator >(i) || llvm::isa< llvm::CastInst >(i) ||
                      llvm::isa< llvm::CmpInst >(i) || llvm::isa< llvm::GetElementPtrInst >(i) ||
                      llvm::isa< llvm::SelectInst >(i) || llvm::isa< llvm::ReturnInst >(i),
                      "recursive complete finite instruction inventory");
          continue;
        }
        occurrence = c;
        auto* a = binding(c); covered.insert(a);
        auto* target = llvm::dyn_cast< llvm::Function >(c->getCalledOperand()->stripPointerCasts());
        auto* imported = a ? native_handle_callee(a) : nullptr;
        need(llvm::isa< llvm::CallInst >(c) && target && target->isDeclaration() && !target->isVarArg() &&
             !c->hasOperandBundles() && c->getCallingConv() == llvm::CallingConv::C &&
             target->getCallingConv() == llvm::CallingConv::C && c->getFunctionType() == target->getFunctionType() &&
             imported && imported->is_declaration() &&
             ordinary_source_call_abi_attributes(c, target), "recursive exact external invocation");
        auto name = target->getName();
        if (name.startswith("pthread_mutex")) {
          auto* result = dyn_cast< ar::IntegerType >(imported->type()->return_type());
          need(result && result->bit_width() == 32 && result->is_signed() &&
               imported->num_parameters() == c->arg_size() &&
               (!a->has_result() || a->result()->type() == result), "recursive imported result/prototype");
          for (unsigned n = 0; n < c->arg_size(); ++n)
            need(imported->type()->param_type(n) == a->argument(n)->type(), "recursive imported parameter type");
        }
        if (creations.count(c)) { need(f == main, "recursive creator root"); created = true; continue; }
        if (name == "pthread_join") {
          need(f == main && native_handle_api(c, false) && c->arg_size() == 2 &&
               llvm::isa< llvm::ConstantPointerNull >(c->getArgOperand(1)) &&
               isa< ar::NullConstant >(a->argument(1)), "recursive nonwriting join"); continue;
        }
        if (name == "pthread_mutexattr_init" || name == "pthread_mutexattr_settype" ||
            name == "pthread_mutexattr_destroy" || name == "pthread_mutex_init") {
          need(f == main && !created && pthread_initialization_signature(a), "recursive mandatory initialization prefix");
          const bool mutex = name == "pthread_mutex_init";
          auto* dest = object(c->getArgOperand(0), a->argument(0), mutex);
          auto* source = base(c->getArgOperand(0)); unsigned result = 0;
          if (name == "pthread_mutexattr_init") {
            need(initialized_attrs.insert(source).second, "recursive attr one live init"); kinds[source] = 0;
          } else if (name == "pthread_mutexattr_settype") {
            auto* n = llvm::dyn_cast< llvm::ConstantInt >(c->getArgOperand(1));
            auto* an = dyn_cast< ar::IntegerConstant >(a->argument(1));
            auto* at = an ? dyn_cast< ar::IntegerType >(an->type()) : nullptr;
            need(initialized_attrs.count(source) && n && n->getBitWidth() == 32 && an && at &&
                 at->bit_width() == 32 && at->is_signed() && source_integer_binding(an, n), "recursive actual setter literal/lifetime");
            auto kind = n->getSExtValue();
            if (kind < 0 || kind > 3) result = 22;
            else { need(kind != 3, "recursive adaptive type excluded"); kinds[source] = static_cast< unsigned >(kind); }
          } else if (name == "pthread_mutexattr_destroy") {
            need(initialized_attrs.erase(source) == 1, "recursive live attribute destroy");
          } else {
            auto* attr = object(c->getArgOperand(1), a->argument(1), false);
            auto* native_attr = base(c->getArgOperand(1)); (void)attr;
            need(initialized_attrs.count(native_attr) && kinds.at(native_attr) == 1 &&
                 initialized_mutexes.insert(source).second, "recursive kind snapshot/one mutex init");
            recursive_objects.insert(cast< ar::GlobalVariable >(dest));
          }
          PthreadInitializationEffect effect{mutex ? abi.mutex : 4u, mutex ? 4u : 0u, {}};
          effect.result_known = true; effect.result = result;
          if (auto* g = dyn_cast< ar::GlobalVariable >(dest)) effect.destinations.push_back({nullptr, g, ZNumber(0), nullptr});
          else effect.destinations.push_back({nullptr, nullptr, ZNumber(0), cast< ar::LocalVariable >(dest)});
          effects.emplace(a, std::move(effect)); lifecycle.insert(a); continue;
        }
        if (name == "pthread_mutex_lock" || name == "pthread_mutex_unlock") {
          need(c->arg_size() == 1 && target->arg_size() == 1 &&
               c->getArgOperand(0)->getType()->isPointerTy() &&
               target->getFunctionType()->getParamType(0)->isPointerTy() &&
               c->getType()->isIntegerTy(32) && target->getReturnType()->isIntegerTy(32) &&
               imported->intrinsic_id() == (name == "pthread_mutex_lock" ? ar::Intrinsic::PthreadMutexLock : ar::Intrinsic::PthreadMutexUnlock),
               "recursive mutex operation identity");
          auto* global = cast< ar::GlobalVariable >(object(c->getArgOperand(0), a->argument(0), true));
          need(initialized_mutexes.count(base(c->getArgOperand(0))) && recursive_objects.count(global), "recursive initialized operation");
          auto& count = depth[global]; const unsigned before = count; unsigned result = 0;
          const bool lock = name == "pthread_mutex_lock";
          auto key = std::make_pair(global, functions.at(f));
          auto inserted = recursive_regions.emplace(key, PthreadRecursiveMutexRegion{global, functions.at(f)});
          auto& region = inserted.first->second;
          if (lock) {
            if (!count) { need(!region.lock, "recursive one outer interval per root/object"); region.lock = a; }
            if (count == 0xffffffffu) result = 11; else ++count;
          } else if (!count) result = 1;
          else if (--count == 0) region.unlock = a;
          recursive_effects.emplace(a, PthreadRecursiveMutexEffect{global, before, count, result, lock}); continue;
        }
        const bool nondet = c->arg_empty() && c->getType()->isIntegerTy() &&
            ((name == "__VERIFIER_nondet_int" && c->getType()->isIntegerTy(32) &&
              imported->has_frontend() && imported->frontend< llvm::Value >() == target) ||
             (imported->intrinsic_id() == ar::Intrinsic::IkosNonDet && c->getMetadata("ikos.freeze_uninit") &&
              (name == "__ikos_nondet_int" || name == "__ikos_nondet_int64")));
        need(nondet, "recursive closed external effects");
      }
    }
    need(!recursive_objects.empty(), "recursive initialized objects");
    // Both source and imported uses are closed, including aliases and dormant
    // statements. No raw observation or hidden write can mutate the type/flags.
    for (const auto& item : storage) {
      std::vector< llvm::Value* > todo{item.first}; std::set< llvm::Value* > seen;
      while (!todo.empty()) {
        step(); auto* v = todo.back(); todo.pop_back(); if (!seen.insert(v).second) continue;
        for (auto* user : v->users()) {
          if (auto* cast = llvm::dyn_cast< llvm::BitCastOperator >(user)) { todo.push_back(cast); continue; }
          if (auto* gep = llvm::dyn_cast< llvm::GEPOperator >(user)) {
            need(gep->getPointerOperand() == v && gep->hasAllZeroIndices(), "recursive no byte arithmetic"); todo.push_back(gep); continue;
          }
          auto* c = llvm::dyn_cast< llvm::CallBase >(user);
          auto* a = c ? dyn_cast_or_null< ar::CallBase >(original(c)) : nullptr;
          need(c && a && (lifecycle.count(a) || recursive_effects.count(a)), "recursive source storage use closure");
          for (unsigned i = 0; i < c->arg_size(); ++i)
            if (c->getArgOperand(i) == v)
              need(i == 0 || (i == 1 && lifecycle.count(a) && effects.at(a).read_bytes == 4), "recursive storage argument role");
        }
      }
    }
    for (const auto& item : functions) {
      llvm::Instruction* previous = nullptr;
      bool returned = false;
      for (auto* block : *item.second->body()) for (auto* s : *block) {
      step();
      need(!returned, "recursive no imported effect after return");
      if (auto* ret = dyn_cast< ar::ReturnValue >(s)) {
        auto* actual = llvm::cast< llvm::ReturnInst >(item.first->getEntryBlock().getTerminator());
        need(ret->has_frontend() && ret->frontend< llvm::Value >() == actual &&
             ret->has_operand() && actual->getReturnValue() &&
             value(ret->operand(), actual->getReturnValue()), "recursive actual imported return");
        returned = true;
      } else need(isa< ar::Load >(s) || isa< ar::Store >(s) || isa< ar::CallBase >(s) ||
                  isa< ar::Allocate >(s) || isa< ar::Assignment >(s) ||
                  isa< ar::UnaryOperation >(s) || isa< ar::BinaryOperation >(s) ||
                  isa< ar::Comparison >(s) || isa< ar::PointerShift >(s),
                  "recursive imported finite instruction inventory");
      if (isa< ar::Load >(s) || isa< ar::Store >(s) || isa< ar::CallBase >(s) || isa< ar::Allocate >(s)) {
        auto* source = s->has_frontend() ? llvm::dyn_cast< llvm::Instruction >(s->frontend< llvm::Value >()) : nullptr;
        need(source && source->getFunction() == item.first && original(source) == s &&
             (!previous || previous->comesBefore(source)), "recursive imported/source effect order");
        previous = source;
      }
      if (auto* load = dyn_cast< ar::Load >(s)) {
        auto* base = direct_byte_object(load->operand());
        auto* source = load->has_frontend() ? llvm::dyn_cast< llvm::LoadInst >(load->frontend< llvm::Value >()) : nullptr;
        auto* global = dyn_cast_or_null< ar::GlobalVariable >(base);
        need(covered.count(s) && base && (!global || global->is_definition()) &&
             load->result()->type()->is_integer() && !load->is_volatile() && !load->is_atomic() &&
             source && !source->isVolatile() && !source->isAtomic() &&
             value(load->operand(), source->getPointerOperand()) && value(load->result(), source) &&
             width(load->result()->type(), source->getType()), "recursive closed scalar load");
        for (const auto& runtime : storage) need(base != runtime.second, "recursive no raw runtime read");
      } else if (auto* store = dyn_cast< ar::Store >(s)) {
        auto* base = direct_byte_object(store->pointer());
        auto* source = store->has_frontend() ? llvm::dyn_cast< llvm::StoreInst >(store->frontend< llvm::Value >()) : nullptr;
        auto* global = dyn_cast_or_null< ar::GlobalVariable >(base);
        need(covered.count(s) && base && (!global || global->is_definition()) &&
             store->value()->type()->is_integer() && !store->is_volatile() && !store->is_atomic() &&
             source && !source->isVolatile() && !source->isAtomic() &&
             value(store->pointer(), source->getPointerOperand()) && value(store->value(), source->getValueOperand()) &&
             width(store->value()->type(), source->getValueOperand()->getType()), "recursive closed scalar store");
        for (const auto& runtime : storage) need(base != runtime.second, "recursive no raw runtime write");
      } else if (isa< ar::CallBase >(s) || isa< ar::Allocate >(s)) need(covered.count(s), "recursive complete imported effects");
      else if (auto* unary = dyn_cast< ar::UnaryOperation >(s)) {
        need(unary->op() != ar::UnaryOperation::UIToPtr && unary->op() != ar::UnaryOperation::SIToPtr, "recursive no numeric alias");
      }
      if (lifecycle.count(dyn_cast< ar::CallBase >(s)) || recursive_effects.count(dyn_cast< ar::CallBase >(s))) continue;
      for (auto op = s->op_begin(); op != s->op_end(); ++op) for (const auto& runtime : storage) {
        if (!mentions_handle_slot(*op, runtime.second)) continue;
        auto* alias = s->result_or_null();
        const bool pointer_alias = isa< ar::Assignment >(s) || isa< ar::PointerShift >(s) ||
            (isa< ar::UnaryOperation >(s) && cast< ar::UnaryOperation >(s)->op() == ar::UnaryOperation::Bitcast);
        need(pointer_alias && alias && alias->type()->is_pointer() && exact_handle_slot(alias, runtime.second), "recursive imported runtime use closure");
      }
      }
      need(returned, "recursive imported complete root return");
    }
    valid = true;
  }
