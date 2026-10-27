// Copyright and license: see execution_engine/numerical.hpp.
#include <ikos/analyzer/analysis/execution_engine/pthread_attributes.hpp>
#include <ikos/analyzer/analysis/source_call_binding.hpp>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Operator.h>
#include <cstdlib>
#include <set>
#include <utility>
#include <vector>

namespace ikos {
namespace analyzer {
namespace {
struct Refusal {};
void need(bool premise) { if (!premise) throw Refusal{}; }
struct Work {
  std::size_t remaining;
  bool exhausted = false;
  void step() {
    if (remaining == 0) { exhausted = true; throw Refusal{}; }
    --remaining;
  }
};

// No module-global mutable cache; this object lives only during one immutable pass.
struct Proof {
  ar::Bundle* bundle;
  ar::Function* main;
  llvm::Function* native_main;
  llvm::Module* module;
  SourceProgramFacts& facts;
  Work work;
  std::map< ar::Variable*, std::vector< ar::Statement* > > definitions;
  std::vector< ar::Statement* > statements;
  std::map< ar::BasicBlock*, std::vector< ar::Statement* > > blocks;
  std::map< ar::Statement*, std::pair< ar::BasicBlock*, std::size_t > > positions;
  unsigned bits, bytes, alignment;

  Proof(ar::Bundle* b, ar::Function* m, SourceProgramFacts& f, std::size_t limit)
      : bundle(b), main(m), native_main(nullptr), module(nullptr), facts(f),
        work{limit}, bits(b->data_layout().pointers.bit_width), bytes(0), alignment(0) {}

  ar::Statement* definition(ar::Value* v) {
    auto* variable = dyn_cast< ar::Variable >(v);
    need(variable != nullptr);
    auto found = definitions.find(variable);
    need(found != definitions.end() && found->second.size() == 1);
    return found->second.front();
  }
  static bool i32(ar::Type* t) {
    auto* integer = dyn_cast< ar::IntegerType >(t);
    return integer && integer->bit_width() == 32 && integer->is_signed();
  }
  static bool zero(ar::Value* v) {
    auto* c = dyn_cast< ar::IntegerConstant >(v);
    return c && c->value().is_zero();
  }
  static bool pointer(llvm::Type* t) {
    auto* p = llvm::dyn_cast< llvm::PointerType >(t);
    return p && p->getAddressSpace() == 0;
  }

  void native_constant(llvm::Value* root) {
    std::vector< llvm::Value* > pending{root};
    std::set< llvm::Value* > seen;
    while (!pending.empty()) {
      work.step();
      auto* value = pending.back(); pending.pop_back();
      if (!seen.insert(value).second) continue;
      need(!llvm::isa< llvm::InlineAsm >(value));
      if (auto* expression = llvm::dyn_cast< llvm::ConstantExpr >(value)) {
        need(expression->getOpcode() != llvm::Instruction::PtrToInt);
        if (expression->getOpcode() == llvm::Instruction::IntToPtr) {
          auto* n = llvm::dyn_cast< llvm::ConstantInt >(expression->getOperand(0));
          need(n && n->isZero());
        }
      }
      // Global definitions are visited separately, rather than following cycles.
      if (llvm::isa< llvm::GlobalValue >(value)) continue;
      if (auto* constant = llvm::dyn_cast< llvm::Constant >(value))
        for (auto& operand : constant->operands()) { work.step(); pending.push_back(operand.get()); }
    }
  }
  void ar_value(ar::Value* root) {
    std::vector< ar::Value* > pending{root};
    std::set< ar::Value* > seen;
    while (!pending.empty()) {
      work.step();
      auto* value = pending.back(); pending.pop_back();
      if (!seen.insert(value).second) continue;
      need(!isa< ar::InlineAssemblyConstant >(value));
      if (auto* f = dyn_cast< ar::FunctionPointerConstant >(value)) {
        need(f->function() != main);
        const auto& name = f->function()->name();
        for (const char* forbidden : {"setjmp", "longjmp", "frameaddress", "localaddress", "localrecover", "stacksave", "stackrestore"}) {
          work.step(); need(name.find(forbidden) == std::string::npos);
        }
      }
      if (auto* s = dyn_cast< ar::StructConstant >(value))
        for (const auto& field : s->values()) { work.step(); pending.push_back(field.value); }
      if (auto* s = dyn_cast< ar::SequentialConstant >(value))
        for (auto* element : s->values()) { work.step(); pending.push_back(element); }
    }
  }
  bool has_alias(ar::Value* root, const std::set< ar::Value* >& aliases) {
    std::vector< ar::Value* > pending{root};
    std::set< ar::Value* > seen;
    while (!pending.empty()) {
      work.step(); auto* value = pending.back(); pending.pop_back();
      if (aliases.count(value)) return true;
      if (!seen.insert(value).second) continue;
      if (auto* aggregate = dyn_cast< ar::StructConstant >(value))
        for (const auto& field : aggregate->values()) { work.step(); pending.push_back(field.value); }
      if (auto* aggregate = dyn_cast< ar::SequentialConstant >(value))
        for (auto* element : aggregate->values()) { work.step(); pending.push_back(element); }
    }
    return false;
  }
  void code(ar::Code* code) {
    for (auto* block : *code) {
      work.step();
      auto& body = blocks[block];
      for (auto* statement : *block) {
        work.step();
        need(!isa< ar::Invoke >(statement) && !isa< ar::Resume >(statement) && !isa< ar::LandingPad >(statement));
        positions.emplace(statement, std::make_pair(block, body.size()));
        body.push_back(statement);
        statements.push_back(statement);
        if (auto* result = statement->result_or_null()) definitions[result].push_back(statement);
        if (auto* call = dyn_cast< ar::CallBase >(statement)) {
          if (auto* target = dyn_cast< ar::FunctionPointerConstant >(call->called())) {
            const auto id = target->function()->intrinsic_id();
            need(id != ar::Intrinsic::StackSave && id != ar::Intrinsic::StackRestore);
          }
        }
        if (auto* cast = dyn_cast< ar::UnaryOperation >(statement)) {
          need(cast->op() != ar::UnaryOperation::PtrToUI && cast->op() != ar::UnaryOperation::PtrToSI);
          if (cast->op() == ar::UnaryOperation::UIToPtr || cast->op() == ar::UnaryOperation::SIToPtr)
            need(zero(cast->operand()));
        }
        for (auto it = statement->op_begin(); it != statement->op_end(); ++it) {
          work.step(); ar_value(*it);
        }
      }
    }
  }
  void inventory() {
    need(main && main->is_definition() && main->name() == "main" && main->has_frontend() &&
         main->body()->has_entry_block());
    native_main = llvm::dyn_cast< llvm::Function >(main->frontend< llvm::Value >());
    need(native_main && !native_main->isDeclaration() && native_main->use_empty());
    module = native_main->getParent();
    const auto abi = pthread_object_layout(bundle, module);
    need(bool(abi) && module->getDataLayout().getIndexSizeInBits(0) == bits &&
         module->getModuleInlineAsm().empty() && module->alias_empty() && module->ifunc_empty());
    alignment = abi.word;
    bytes = bits == 32 ? 36 : 56;
    need(!module->getGlobalVariable("llvm.global_ctors") &&
         !module->getGlobalVariable("llvm.global_dtors") &&
         !bundle->global_or_null("ar.global_ctors") && !bundle->global_or_null("ar.global_dtors"));
    for (auto& g : module->globals()) {
      work.step(); if (g.hasInitializer()) native_constant(g.getInitializer());
    }
    for (auto& f : *module) {
      work.step();
      need(!f.hasFnAttribute(llvm::Attribute::ReturnsTwice));
      if (f.isIntrinsic() && !f.use_empty()) {
        const auto id = f.getIntrinsicID();
        need(id == llvm::Intrinsic::dbg_declare || id == llvm::Intrinsic::dbg_value ||
             id == llvm::Intrinsic::dbg_label || id == llvm::Intrinsic::lifetime_start ||
             id == llvm::Intrinsic::lifetime_end);
      }
      auto name = f.getName();
      need(!name.contains("setjmp") && !name.contains("longjmp"));
      for (auto& block : f) {
        work.step();
        for (auto& instruction : block) {
          work.step();
          need(!llvm::isa< llvm::PtrToIntInst >(instruction) &&
               !llvm::isa< llvm::IndirectBrInst >(instruction) &&
               !llvm::isa< llvm::CallBrInst >(instruction) &&
               !llvm::isa< llvm::InvokeInst >(instruction) && !llvm::isa< llvm::ResumeInst >(instruction) &&
               !llvm::isa< llvm::CatchReturnInst >(instruction) && !llvm::isa< llvm::CleanupReturnInst >(instruction));
          if (auto* cast = llvm::dyn_cast< llvm::IntToPtrInst >(&instruction)) {
            auto* n = llvm::dyn_cast< llvm::ConstantInt >(cast->getOperand(0)); need(n && n->isZero());
          }
          if (auto* call = llvm::dyn_cast< llvm::CallBase >(&instruction)) {
            need(!call->isInlineAsm() && !call->hasFnAttr(llvm::Attribute::ReturnsTwice));
            auto* target = call->getCalledFunction();
            if (target && target->isIntrinsic()) {
              auto id = target->getIntrinsicID();
              // No frameaddress/localaddress/localrecover/stacksave or unclassified intrinsic.
              need(llvm::isa< llvm::DbgInfoIntrinsic >(call) ||
                   id == llvm::Intrinsic::lifetime_start || id == llvm::Intrinsic::lifetime_end);
            }
          }
          for (auto& operand : instruction.operands()) { work.step(); native_constant(operand.get()); }
        }
      }
    }
    for (auto it = bundle->function_begin(); it != bundle->function_end(); ++it) {
      work.step(); if ((*it)->is_definition()) code((*it)->body());
    }
    for (auto it = bundle->global_begin(); it != bundle->global_end(); ++it) {
      work.step(); if ((*it)->is_definition()) code((*it)->initializer());
    }
    // The preceding bounded raw scan includes empty blocks and declarations.
    // The shared partial index is reused only after that complete inventory.
    need(facts.index(work.remaining) && facts.memory_calls_allocations().unique);
    need(facts.statement_count() < work.remaining);
    work.remaining -= facts.statement_count();
  }

  using Position = std::pair< ar::BasicBlock*, std::size_t >;
  bool ar_reaches(Position from, ar::Statement* target, ar::Statement* cut = nullptr) {
    std::vector< Position > pending{from};
    std::set< Position > seen;
    while (!pending.empty()) {
      work.step();
      auto position = pending.back(); pending.pop_back();
      if (!seen.insert(position).second) continue;
      auto found = blocks.find(position.first); need(found != blocks.end());
      const auto& body = found->second;
      if (position.second < body.size()) {
        auto* statement = body[position.second];
        if (statement == target) return true;
        if (statement != cut) pending.emplace_back(position.first, position.second + 1);
      } else {
        for (auto it = position.first->successor_begin(); it != position.first->successor_end(); ++it) {
          work.step(); pending.emplace_back(*it, 0);
        }
      }
    }
    return false;
  }
  bool ar_before(ar::Statement* first, ar::Statement* second) {
    const Position entry{main->body()->entry_block(), 0};
    return first != second && ar_reaches(entry, first) && ar_reaches(entry, second) &&
           !ar_reaches(entry, second, first);
  }
  bool native_reaches(llvm::BasicBlock* from, llvm::BasicBlock* target,
                      llvm::BasicBlock* cut_from = nullptr, llvm::BasicBlock* cut_to = nullptr) {
    std::vector< llvm::BasicBlock* > pending{from};
    std::set< llvm::BasicBlock* > seen;
    while (!pending.empty()) {
      work.step(); auto* block = pending.back(); pending.pop_back();
      if (block == target) return true;
      if (!seen.insert(block).second) continue;
      auto* end = block->getTerminator(); need(end != nullptr);
      for (unsigned n = 0; n < end->getNumSuccessors(); ++n) {
        work.step(); auto* next = end->getSuccessor(n);
        if (block != cut_from || next != cut_to) pending.push_back(next);
      }
    }
    return false;
  }
  void acyclic(llvm::CallBase* call, ar::CallBase* imported) {
    auto* end = call->getParent()->getTerminator();
    for (unsigned n = 0; n < end->getNumSuccessors(); ++n) {
      work.step(); need(!native_reaches(end->getSuccessor(n), call->getParent()));
    }
    auto position = positions.at(imported); ++position.second;
    need(!ar_reaches(position, imported));
  }
  bool result_origin(ar::Value* value, ar::CallBase* call, ar::Statement* use) {
    std::set< ar::Value* > seen;
    for (unsigned depth = 0; depth != 32; ++depth) {
      work.step();
      if (!i32(value->type()) || !seen.insert(value).second) return false;
      auto* d = definition(value);
      if (!ar_before(d, use)) return false;
      if (d == call) return call->has_result() && value == call->result();
      if (d->has_frontend() && d->frontend< llvm::Value >() != call->frontend< llvm::Value >()) return false;
      use = d;
      if (auto* copy = dyn_cast< ar::Assignment >(d)) value = copy->operand();
      else if (auto* cast = dyn_cast< ar::UnaryOperation >(d)) {
        if (cast->op() != ar::UnaryOperation::Bitcast) return false;
        value = cast->operand();
      } else return false;
    }
    return false;
  }
  void success_before(llvm::CallBase* call, ar::CallBase* imported,
                      llvm::CallBase* consumer, ar::CallBase* ar_consumer) {
    need(imported->has_result() && i32(imported->result()->type()) && definition(imported->result()) == imported);
    llvm::ICmpInst* comparison = nullptr;
    for (auto* user : call->users()) {
      work.step();
      auto* cmp = llvm::dyn_cast< llvm::ICmpInst >(user);
      need(cmp && (!comparison || comparison == cmp)); comparison = cmp;
    }
    need(comparison && (comparison->getPredicate() == llvm::CmpInst::ICMP_EQ ||
                        comparison->getPredicate() == llvm::CmpInst::ICMP_NE));
    auto* other = comparison->getOperand(comparison->getOperand(0) == call ? 1 : 0);
    auto* zero = llvm::dyn_cast< llvm::ConstantInt >(other); need(zero && zero->isZero());
    llvm::BranchInst* branch = nullptr;
    for (auto* user : comparison->users()) {
      work.step(); auto* b = llvm::dyn_cast< llvm::BranchInst >(user);
      need(b && b->isConditional() && b->getCondition() == comparison && (!branch || branch == b)); branch = b;
    }
    need(branch && branch->getSuccessor(0) != branch->getSuccessor(1));
    const auto& dom = facts.dominators(native_main);
    need(dom.dominates(call, comparison) && dom.dominates(comparison, branch) && dom.dominates(call, consumer));
    auto* success = branch->getSuccessor(comparison->getPredicate() == llvm::CmpInst::ICMP_EQ ? 0 : 1);
    need(native_reaches(&native_main->getEntryBlock(), consumer->getParent()) &&
         !native_reaches(&native_main->getEntryBlock(), consumer->getParent(), branch->getParent(), success));
    bool found = false;
    for (auto* statement : statements) {
      work.step();
      auto* guard = dyn_cast< ar::Comparison >(statement);
      if (!guard || statement->code() != main->body() || !guard->has_frontend()) continue;
      auto* original = guard->frontend< llvm::Value >();
      if (original != comparison && original != branch) continue;
      if (guard->predicate() != ar::Comparison::SIEQ) continue;
      ar::Value* result = nullptr;
      if (Proof::zero(guard->right())) result = guard->left();
      else if (Proof::zero(guard->left())) result = guard->right();
      if (result && result_origin(result, imported, guard) && ar_before(guard, ar_consumer)) found = true;
    }
    need(found);
  }

  llvm::Value* native_copy(llvm::Instruction* instruction) {
    if (auto* cast = llvm::dyn_cast< llvm::BitCastInst >(instruction)) {
      if (pointer(cast->getType()) && pointer(cast->getOperand(0)->getType())) return cast->getOperand(0);
    }
    if (auto* shift = llvm::dyn_cast< llvm::GetElementPtrInst >(instruction)) {
      for (auto& index : shift->indices()) {
        work.step(); auto* c = llvm::dyn_cast< llvm::ConstantInt >(index.get()); if (!c || !c->isZero()) return nullptr;
      }
      return shift->getPointerOperand();
    }
    return nullptr;
  }
  ar::Value* ar_copy(ar::Statement* statement) {
    if (!statement->has_result() || !isa< ar::PointerType >(statement->result()->type())) return nullptr;
    if (auto* copy = dyn_cast< ar::Assignment >(statement)) {
      if (isa< ar::PointerType >(copy->operand()->type())) return copy->operand();
    }
    if (auto* cast = dyn_cast< ar::UnaryOperation >(statement)) {
      if (cast->op() == ar::UnaryOperation::Bitcast && isa< ar::PointerType >(cast->operand()->type())) return cast->operand();
    }
    if (auto* shift = dyn_cast< ar::PointerShift >(statement)) {
      for (auto it = shift->term_begin(); it != shift->term_end(); ++it) {
        work.step(); if (!zero((*it).second)) return nullptr;
      }
      return shift->pointer();
    }
    return nullptr;
  }
  ar::CallBase* bind(llvm::CallBase* call) {
    auto found = facts.memory_calls_allocations().statements.find(call);
    need(found != facts.memory_calls_allocations().statements.end());
    auto* imported = source_call_binding(call, found->second);
    need(imported && imported->code() == main->body() && imported->has_frontend() &&
         imported->frontend< llvm::Value >() == call);
    // Real direct callee identity: copied/indirect/function-alias API forms refuse.
    auto* original = call->getCalledFunction();
    auto* target = dyn_cast< ar::FunctionPointerConstant >(imported->called());
    need(original && original->isDeclaration() && target && target->function()->is_declaration() &&
         target->function()->has_frontend() && target->function()->frontend< llvm::Value >() == original &&
         target->function()->name() == original->getName().str() &&
         i32(target->function()->type()->return_type()) && call->getType()->isIntegerTy(32));
    if (imported->has_result()) need(i32(imported->result()->type()) && definition(imported->result()) == imported);
    return imported;
  }

  ClosedPthreadAttributeEffects object(llvm::AllocaInst* storage) {
    need(storage->getParent() == &native_main->getEntryBlock() && !storage->isArrayAllocation() &&
         storage->getAddressSpace() == 0 && storage->getAlign().value() >= alignment);
    auto* count = llvm::dyn_cast< llvm::ConstantInt >(storage->getArraySize()); need(count && count->isOne());
    auto* type = llvm::dyn_cast< llvm::StructType >(storage->getAllocatedType());
    need(type && !type->isOpaque() && !type->isPacked() && type->hasName() &&
         type->getName() == "union.pthread_attr_t");
    const auto& dl = module->getDataLayout();
    need(!dl.getTypeAllocSize(type).isScalable() && dl.getTypeAllocSize(type).getFixedValue() == bytes &&
         dl.getABITypeAlign(type).value() == alignment);
    auto found = facts.memory_calls_allocations().statements.find(storage);
    need(found != facts.memory_calls_allocations().statements.end());
    auto* allocation = dyn_cast< ar::Allocate >(found->second);
    need(allocation && allocation->parent() == main->body()->entry_block() &&
         allocation->code() == main->body() && definition(allocation->result()) == allocation &&
         allocation->result()->alignment() >= alignment &&
         allocation->result()->has_frontend() && allocation->result()->frontend< llvm::Value >() == storage);
    auto* size = dyn_cast< ar::IntegerConstant >(allocation->array_size());
    need(size && size->value().to_z_number() == 1 &&
         allocation->result()->type()->pointee() == allocation->allocated_type() &&
         bundle->data_layout().alloc_size_in_bytes(allocation->allocated_type()) == bytes &&
         bundle->data_layout().abi_alignment(allocation->allocated_type()) == alignment);
    // The entry block itself must not be revisited, including malformed AR CFGs.
    auto after_allocation = positions.at(allocation); ++after_allocation.second;
    need(!ar_reaches(after_allocation, allocation));
    auto* entry_end = native_main->getEntryBlock().getTerminator();
    for (unsigned n = 0; n < entry_end->getNumSuccessors(); ++n) {
      work.step(); need(!native_reaches(entry_end->getSuccessor(n), &native_main->getEntryBlock()));
    }
    std::set< llvm::Value* > native_aliases{storage};
    std::vector< llvm::Value* > pending{storage};
    std::set< llvm::CallBase* > calls;
    while (!pending.empty()) {
      work.step(); auto* value = pending.back(); pending.pop_back();
      for (auto* user : value->users()) {
        work.step();
        auto* instruction = llvm::dyn_cast< llvm::Instruction >(user);
        need(instruction && instruction->getFunction() == native_main);
        if (native_copy(instruction) == value) {
          if (native_aliases.insert(instruction).second) pending.push_back(instruction);
        } else {
          // Lifetime intrinsics are deliberately unsupported for this object.
          // Their absence from the selected AR index never counts as elimination.
          auto* call = llvm::dyn_cast< llvm::CallBase >(instruction);
          need(call && !llvm::isa< llvm::IntrinsicInst >(call)); calls.insert(call);
        }
      }
    }
    std::set< ar::Value* > aliases{allocation->result()};
    bool changed = true;
    while (changed) {
      work.step(); changed = false;
      for (auto* statement : statements) {
        work.step(); auto* input = ar_copy(statement);
        if (!input || !aliases.count(input)) continue;
        need(statement->code() == main->body() && definition(statement->result()) == statement &&
             ar_before(definition(input), statement));
        if (statement->has_frontend()) need(native_aliases.count(statement->frontend< llvm::Value >()) != 0);
        changed |= aliases.insert(statement->result()).second;
      }
    }
    std::map< llvm::CallBase*, ar::CallBase* > mapped;
    llvm::CallBase *init = nullptr, *setter = nullptr, *destroy = nullptr;
    for (auto* call : calls) {
      work.step(); auto* imported = bind(call); mapped.emplace(call, imported);
      auto* target = call->getCalledFunction(); auto name = target->getName();
      const bool create = name == "pthread_create";
      unsigned expected = create ? 1 : 0;
      need(create || name == "pthread_attr_init" || name == "pthread_attr_setdetachstate" || name == "pthread_attr_destroy");
      need(call->arg_size() == (create ? 4u : name == "pthread_attr_setdetachstate" ? 2u : 1u));
      for (unsigned n = 0; n < call->arg_size(); ++n) {
        work.step();
        need((native_aliases.count(call->getArgOperand(n)) != 0) == (n == expected));
        need((aliases.count(imported->argument(n)) != 0) == (n == expected));
      }
      need(pointer(call->getArgOperand(expected)->getType()) &&
           llvm::cast< llvm::PointerType >(call->getArgOperand(expected)->getType())->getPointerElementType() == type);
      auto* ar_pointer = dyn_cast< ar::PointerType >(imported->argument(expected)->type());
      need(ar_pointer && ar_pointer->pointee() == allocation->allocated_type());
      if (create) {
        need(pointer(call->getArgOperand(0)->getType()) && pointer(call->getArgOperand(2)->getType()) &&
             pointer(call->getArgOperand(3)->getType()));
        auto* handle = llvm::cast< llvm::PointerType >(call->getArgOperand(0)->getType());
        need(handle->getPointerElementType()->isIntegerTy(bits));
        auto* entry = llvm::dyn_cast< llvm::FunctionType >(
            llvm::cast< llvm::PointerType >(call->getArgOperand(2)->getType())->getPointerElementType());
        need(entry && !entry->isVarArg() && entry->getNumParams() == 1 && pointer(entry->getParamType(0)) && pointer(entry->getReturnType()));
      } else if (name == "pthread_attr_init") { need(!init); init = call; }
      else if (name == "pthread_attr_destroy") { need(!destroy); destroy = call; }
      else {
        need(!setter); setter = call;
        auto* n = llvm::dyn_cast< llvm::ConstantInt >(call->getArgOperand(1));
        need(n && n->getType()->isIntegerTy(32) && n->isZero() &&
             i32(imported->argument(1)->type()) && zero(imported->argument(1)));
      }
    }
    need(init != nullptr);
    // Every AR operand use must be an accepted zero alias or the exact mapped API position.
    std::set< ar::CallBase* > seen_calls;
    for (auto* statement : statements) {
      work.step();
      bool used = false;
      for (auto it = statement->op_begin(); it != statement->op_end(); ++it) { work.step(); used |= has_alias(*it, aliases); }
      if (!used) continue;
      if (auto* input = ar_copy(statement)) {
        need(aliases.count(input) && aliases.count(statement->result())); continue;
      }
      auto* imported = dyn_cast< ar::CallBase >(statement); need(imported && imported->has_frontend());
      auto* source = llvm::dyn_cast< llvm::CallBase >(imported->frontend< llvm::Value >());
      need(source && mapped.count(source) && mapped.at(source) == imported && !aliases.count(imported->called()));
      unsigned expected = source->getCalledFunction()->getName() == "pthread_create" ? 1 : 0;
      for (unsigned n = 0; n < imported->num_arguments(); ++n) {
        work.step(); need((aliases.count(imported->argument(n)) != 0) == (n == expected));
      }
      seen_calls.insert(imported);
    }
    need(seen_calls.size() == calls.size());
    for (const auto& pair : mapped) {
      work.step(); auto* source = pair.first; auto* imported = pair.second;
      need(ar_before(allocation, imported) && facts.dominators(native_main).dominates(storage, source));
      if (source == init || source == setter || source == destroy) acyclic(source, imported);
      if (source != init) success_before(init, mapped.at(init), source, imported);
      if (setter && source != init && source != setter) success_before(setter, mapped.at(setter), source, imported);
      if (destroy && source != destroy) {
        auto pos = positions.at(mapped.at(destroy)); ++pos.second;
        need(!ar_reaches(pos, imported));
        auto* block = destroy->getParent();
        if (source->getParent() == block) need(source->comesBefore(destroy));
        else {
          auto* end = block->getTerminator();
          for (unsigned n = 0; n < end->getNumSuccessors(); ++n) {
            work.step(); need(!native_reaches(end->getSuccessor(n), source->getParent()));
          }
        }
      }
    }
    ClosedPthreadAttributeEffects result;
    for (auto* source : {init, setter, destroy}) {
      work.step(); if (source) result.emplace(mapped.at(source), ClosedPthreadAttributeEffect{allocation->result(), bytes, alignment});
    }
    return result;
  }
};
} // namespace

ClosedPthreadAttributeEffects build_closed_pthread_attribute_effects(
    ar::Bundle* bundle, ar::Function* main, SourceProgramFacts& facts, std::size_t work_limit) {
  Proof proof(bundle, main, facts, work_limit);
  ClosedPthreadAttributeEffects result;
  try {
    proof.inventory();
    for (auto& instruction : proof.native_main->getEntryBlock()) {
      proof.work.step();
      auto* allocation = llvm::dyn_cast< llvm::AllocaInst >(&instruction);
      if (!allocation) continue;
      try {
        auto effects = proof.object(allocation);
        result.insert(effects.begin(), effects.end());
      } catch (const Refusal&) {
        if (proof.work.exhausted) return {};
      }
    }
  } catch (const Refusal&) { return {}; }
  return result;
}

const ClosedPthreadAttributeEffect* closed_pthread_attribute_effect(ar::CallBase* call, Context& ctx) {
  // Diagnostic switch disables both consumers, never changes another mechanism.
  if (!ctx.opts.enable_thread_modular || std::getenv("IKOS_DISABLE_PTHREAD_ATTRIBUTES") ||
      ctx.opts.entry_points.size() != 1 || !ctx.opts.no_init_globals.empty()) return nullptr;
  auto* target = dyn_cast< ar::FunctionPointerConstant >(call->called());
  if (!target || !target->function()->is_declaration()) return nullptr;
  const auto& name = target->function()->name();
  if (name != "pthread_attr_init" && name != "pthread_attr_setdetachstate" && name != "pthread_attr_destroy") return nullptr;
  auto& facts = source_program_facts(ctx);
  auto& cache = facts.pthread_attributes();
  if (!cache.built) {
    cache.built = true;
    cache.effects = build_closed_pthread_attribute_effects(ctx.bundle, ctx.opts.entry_points.front(), facts);
  }
  auto found = cache.effects.find(call);
  return found == cache.effects.end() ? nullptr : &found->second;
}
} // namespace analyzer
} // namespace ikos
