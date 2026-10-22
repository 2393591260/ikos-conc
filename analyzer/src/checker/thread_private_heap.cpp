// Copyright and license: see data_race.hpp.
#include <ikos/analyzer/checker/thread_private_heap.hpp>

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Operator.h>
#include <ikos/analyzer/analysis/execution_engine/allocation_continuation.hpp>
#include <ikos/analyzer/analysis/execution_engine/native_thread_handles.hpp>
#include <ikos/analyzer/checker/data_race.hpp>
#include <map>
#include <set>

namespace ikos {
namespace analyzer {
namespace {
// CFG reachability includes cycles and does not rely on LoopInfo recognizing
// reducible loops. In the same block instruction order matters as well.
bool may_follow(llvm::Instruction* from, llvm::Instruction* to) {
  if (from->getParent() == to->getParent() && from->comesBefore(to)) return true;
  std::vector< llvm::BasicBlock* > pending;
  for (auto* b : llvm::successors(from->getParent())) pending.push_back(b);
  std::set< llvm::BasicBlock* > seen;
  while (!pending.empty()) {
    auto* b = pending.back(); pending.pop_back();
    if (b == to->getParent()) return true;
    if (!seen.insert(b).second) continue;
    for (auto* next : llvm::successors(b)) pending.push_back(next);
  }
  return false;
}
bool plain_call(llvm::CallBase* call, llvm::Function* f) {
  if (!f || !llvm::isa< llvm::CallInst >(call) || call->isInlineAsm() ||
      call->hasOperandBundles() || call->getCallingConv() != llvm::CallingConv::C ||
      f->getCallingConv() != llvm::CallingConv::C ||
      call->getFunctionType() != f->getFunctionType() || f->isVarArg() ||
      call->arg_size() != f->arg_size()) return false;
  for (unsigned i = 0; i < call->arg_size(); ++i)
    for (auto attr : {llvm::Attribute::ByVal, llvm::Attribute::StructRet,
                     llvm::Attribute::InAlloca, llvm::Attribute::Preallocated})
      if (call->paramHasAttr(i, attr) || f->hasParamAttribute(i, attr)) return false;
  return true;
}
llvm::Function* callee(llvm::CallBase* call) {
  return llvm::dyn_cast< llvm::Function >(call->getCalledOperand()->stripPointerCasts());
}
// Harness nondeterminism permits independent integer values, no pointer effects.
bool nondet(llvm::CallBase* call, llvm::Function* f) {
  if (!f || !f->isDeclaration() || !call->getType()->isIntegerTy() ||
      call->getType() != f->getReturnType() || call->arg_size() != 0 ||
      f->arg_size() != 0 || call->getCallingConv() != llvm::CallingConv::C ||
      f->getCallingConv() != llvm::CallingConv::C) return false;
  if (f->getName() == "__VERIFIER_nondet_int") return call->getType()->isIntegerTy(32);
  return call->getMetadata("ikos.freeze_uninit") && !f->isVarArg() &&
      call->getFunctionType() == f->getFunctionType() &&
      ((f->getName() == "__ikos_nondet_int" && call->getType()->isIntegerTy(32)) ||
       (f->getName() == "__ikos_nondet_int64" && call->getType()->isIntegerTy(64)));
}
struct Offset {
  llvm::Value* index = nullptr;
  uint64_t stride = 0;
};
struct Certificate {
  ar::CallBase* allocation = nullptr;
  ar::Function* worker = nullptr;
  std::set< llvm::Instruction* > accesses;
};
} // namespace

class ThreadPrivateHeap::Impl {
  Context& ctx;
  std::map< llvm::Value*, ar::CallBase* > calls;
  std::map< llvm::Value*, ar::Statement* > effects;
  std::map< ar::CallBase*, Certificate > certificates;
  unsigned budget = 131072;

  bool spend() { if (budget == 0) return false; --budget; return true; }
  bool same_bits(ar::Value* value, llvm::Value* source) {
    for (unsigned depth = 0; depth < 32; ++depth) {
      auto* type = dyn_cast< ar::IntegerType >(value->type());
      if (!type || type->bit_width() != 32) return false;
      if (auto* integer = dyn_cast< ar::IntegerConstant >(value)) {
        auto* original = llvm::dyn_cast< llvm::ConstantInt >(source);
        auto bits = integer->value().to_z_number();
        if (bits < 0) bits += core::ZNumber(1) << 32;
        return original && original->getType()->isIntegerTy(32) &&
               bits == core::ZNumber(original->getZExtValue());
      }
      auto* variable = dyn_cast< ar::InternalVariable >(value);
      auto* definition = variable ? unique_def(variable) : nullptr;
      if (auto* cast = dyn_cast_or_null< ar::UnaryOperation >(definition)) {
        if (cast->op() != ar::UnaryOperation::Bitcast) return false;
        value = cast->operand();
      } else {
        auto found = calls.find(source);
        return found != calls.end() && found->second && found->second == definition &&
               found->second->has_result() && found->second->result() == value;
      }
    }
    return false;
  }
  bool entry_uses(llvm::Function* f) {
    std::vector< llvm::Value* > pending{f};
    std::set< llvm::Value* > seen;
    bool created = false;
    while (!pending.empty()) {
      auto* v = pending.back(); pending.pop_back();
      if (!spend()) return false;
      if (!seen.insert(v).second) continue;
      for (auto* u : v->users()) {
        if (llvm::isa< llvm::BitCastOperator >(u)) { pending.push_back(u); continue; }
        auto* call = llvm::dyn_cast< llvm::CallBase >(u);
        if (!call || !native_handle_api(call, true) || call->getArgOperand(2) != v ||
            !calls.count(call) || !calls.at(call)) return false;
        auto* ar_call = calls.at(call);
        auto* ar_target = native_handle_callee(ar_call);
        if (!ar_target || !ar_target->is_declaration() ||
            ar_target->name() != "pthread_create" || ar_call->num_arguments() != 4 ||
            !isa< ar::FunctionPointerConstant >(ar_call->argument(2))) return false;
        auto* child = cast< ar::FunctionPointerConstant >(ar_call->argument(2))->function();
        if (!child->has_frontend() || child->frontend< llvm::Value >() != f) return false;
        created = true;
      }
    }
    return created;
  }
  bool effects_closed(llvm::Function* f, llvm::CallBase* allocation,
                      std::set< llvm::Function* >& active,
                      std::set< llvm::Function* >& done, llvm::CallBase* transfer = nullptr) {
    if (done.count(f)) return true;
    if (!active.insert(f).second) return false;
    for (auto& block : *f) for (auto& instruction : block) {
      if (!spend() || llvm::isa< llvm::IntToPtrInst >(instruction) ||
          llvm::isa< llvm::PtrToIntInst >(instruction) || llvm::isa< llvm::VAArgInst >(instruction)) return false;
      auto* call = llvm::dyn_cast< llvm::CallBase >(&instruction);
      if (!call || llvm::isa< llvm::DbgInfoIntrinsic >(call)) continue;
      auto* target = callee(call);
      if (!target || call->isInlineAsm() || call->hasOperandBundles() ||
          !llvm::isa< llvm::CallInst >(call)) return false;
      if (nondet(call, target)) continue;
      if (!plain_call(call, target)) return false;
      if (!target->isDeclaration()) {
        if (!effects_closed(target, allocation, active, done, transfer)) return false;
      } else if (call == allocation || call == transfer) {
        continue;
      } else if (transfer && ((target->getName() == "malloc" && target->arg_size() == 1 &&
                  target->getReturnType()->isPointerTy() &&
                  target->getFunctionType()->getParamType(0)->isIntegerTy(32)) ||
                  native_handle_api(call, false) || native_handle_api(call, true))) {
        continue;
      } else if (target->getName() == "free" && target->arg_size() == 1 &&
                 target->getReturnType()->isVoidTy() &&
                 target->getFunctionType()->getParamType(0)->isPointerTy()) {
        continue;
      } else if (target->getName() == "abort" && target->arg_size() == 0 && target->getReturnType()->isVoidTy()) {
        continue;
      } else if (target->getName() == "__assert_fail" && target->arg_size() == 4 &&
                 target->getReturnType()->isVoidTy() &&
                 target->getFunctionType()->getParamType(0)->isPointerTy() &&
                 target->getFunctionType()->getParamType(1)->isPointerTy() &&
                 target->getFunctionType()->getParamType(2)->isIntegerTy(32) &&
                 target->getFunctionType()->getParamType(3)->isPointerTy()) {
        continue;
      } else return false;
    }
    active.erase(f); done.insert(f); return true;
  }
  bool counted_index(llvm::Value* index, llvm::Value* count,
                     llvm::Instruction* access, llvm::LoopInfo& loops,
                     llvm::DominatorTree& dom) {
    auto* phi = llvm::dyn_cast< llvm::PHINode >(index);
    auto* loop = phi ? loops.getLoopFor(phi->getParent()) : nullptr;
    if (!phi || !loop || loop->getHeader() != phi->getParent() ||
        phi->getNumIncomingValues() != 2 || phi->getType() != count->getType() ||
        !phi->getType()->isIntegerTy(32) || !loop->contains(access)) return false;
    auto* latch = loop->getLoopLatch(); auto* pre = loop->getLoopPreheader();
    if (!latch || !pre) return false;
    auto* start = llvm::dyn_cast_or_null< llvm::ConstantInt >(phi->getIncomingValueForBlock(pre));
    auto* update = llvm::dyn_cast_or_null< llvm::BinaryOperator >(phi->getIncomingValueForBlock(latch));
    if (!start || !start->isZero() || !update || update->getOpcode() != llvm::Instruction::Add ||
        update->getOperand(0) != phi) return false;
    auto* one = llvm::dyn_cast< llvm::ConstantInt >(update->getOperand(1));
    auto* branch = llvm::dyn_cast< llvm::BranchInst >(loop->getHeader()->getTerminator());
    auto* compare = branch && branch->isConditional()
        ? llvm::dyn_cast< llvm::ICmpInst >(branch->getCondition()) : nullptr;
    if (!one || !one->isOne() || !compare || compare->getPredicate() != llvm::CmpInst::ICMP_SLT ||
        compare->getOperand(0) != phi || compare->getOperand(1) != count ||
        !loop->contains(branch->getSuccessor(0)) || loop->contains(branch->getSuccessor(1))) return false;
    llvm::BasicBlockEdge body(loop->getHeader(), branch->getSuccessor(0));
    // 0 <= i < signed count <= INT_MAX makes i+1 representable; no nsw premise.
    return dom.dominates(body, access->getParent()) && dom.dominates(body, update->getParent()) &&
           dom.dominates(update, latch->getTerminator());
  }
  bool bounded(const Offset& offset, uint64_t width, llvm::CallBase* allocation,
               llvm::Instruction* access, llvm::LoopInfo& loops, llvm::DominatorTree& dom) {
    auto* size = llvm::dyn_cast< llvm::ConstantInt >(allocation->getArgOperand(1));
    if (!size || size->isZero() || size->getValue().getActiveBits() > 32 || width == 0) return false;
    const auto stride = size->getZExtValue();
    auto* index = llvm::dyn_cast_or_null< llvm::ConstantInt >(offset.index);
    if (!offset.index || index) {
      auto* count = llvm::dyn_cast< llvm::ConstantInt >(allocation->getArgOperand(0));
      if (!count || (index && index->isNegative())) return false;
      auto start = index ? core::ZNumber(index->getZExtValue()) * offset.stride : core::ZNumber(0);
      return start + width <= core::ZNumber(count->getZExtValue()) * stride;
    }
    return offset.stride == stride && width <= stride &&
           counted_index(offset.index, allocation->getArgOperand(0), access, loops, dom);
  }
  Certificate certify(ar::CallBase* ar_allocation) {
    Certificate result;
    auto* allocation = ar_allocation->has_frontend()
        ? llvm::dyn_cast< llvm::CallBase >(ar_allocation->frontend< llvm::Value >()) : nullptr;
    auto* api = allocation ? callee(allocation) : nullptr;
    auto* ar_api = native_handle_callee(ar_allocation);
    if (!api || !ar_api || !ar_api->is_declaration() || ar_api->intrinsic_id() != ar::Intrinsic::LibcCalloc ||
        !plain_call(allocation, api) || !api->isDeclaration() || api->getName() != "calloc" ||
        allocation->arg_size() != 2 || ar_allocation->num_arguments() != 2 ||
        !calls.count(allocation) || calls.at(allocation) != ar_allocation ||
        !same_bits(ar_allocation->argument(0), allocation->getArgOperand(0)) ||
        !same_bits(ar_allocation->argument(1), allocation->getArgOperand(1)) ||
        !api->getReturnType()->isPointerTy() ||
        !api->getFunctionType()->getParamType(0)->isIntegerTy(32) ||
        !api->getFunctionType()->getParamType(1)->isIntegerTy(32) ||
        may_follow(allocation, allocation)) return result;
    auto* f = allocation->getFunction();
    auto* count = allocation->getArgOperand(0);
    auto* source = llvm::dyn_cast< llvm::CallBase >(count);
    // In particular, repeated LLVM uses of undef are not a stable size.
    if (!llvm::isa< llvm::ConstantInt >(count) && (!source || !nondet(source, callee(source)))) return result;
    auto* pointer_type = llvm::cast< llvm::PointerType >(allocation->getType());
    const auto& dl = f->getParent()->getDataLayout();
    llvm::Triple target(f->getParent()->getTargetTriple());
    if (pointer_type->getAddressSpace() != 0 || !pointer_type->getPointerElementType()->isIntegerTy(8) ||
        dl.getPointerSizeInBits() != 32 || dl.getIndexSizeInBits(0) != 32 ||
        target.getArch() != llvm::Triple::x86 || !target.isOSLinux() ||
        target.getEnvironment() != llvm::Triple::GNU ||
        !mutex_effect_pointer_representations_closed(*f->getParent()) || !entry_uses(f)) return result;
    std::set< llvm::Function* > active, done;
    if (!effects_closed(f, allocation, active, done)) return result;
    llvm::DominatorTree dom(*f); llvm::LoopInfo loops(dom);
    const auto& layout = f->getParent()->getDataLayout();
    std::map< llvm::Value*, Offset > offsets{{allocation, {}}};
    std::vector< llvm::Value* > pending{allocation};
    std::set< llvm::Value* > seen;
    std::set< llvm::Instruction* > pointer_uses;
    llvm::StoreInst* tls_store = nullptr;
    llvm::CallBase* release = nullptr;
    while (!pending.empty()) {
      auto* pointer = pending.back(); pending.pop_back();
      if (!spend()) return {};
      if (!seen.insert(pointer).second) continue;
      auto offset = offsets.at(pointer);
      for (auto* user : pointer->users()) {
        auto* instruction = llvm::dyn_cast< llvm::Instruction >(user);
        if (!instruction || instruction->getFunction() != f) return {};
        if (llvm::isa< llvm::DbgInfoIntrinsic >(instruction)) continue;
        pointer_uses.insert(instruction);
        if (auto* cast = llvm::dyn_cast< llvm::BitCastInst >(instruction)) {
          if (!cast->getType()->isPointerTy() ||
              llvm::cast< llvm::PointerType >(cast->getType())->getAddressSpace() != 0) return {};
          offsets.emplace(cast, offset); pending.push_back(cast);
        } else if (auto* gep = llvm::dyn_cast< llvm::GetElementPtrInst >(instruction)) {
          auto* element = gep->getSourceElementType();
          if (offset.index || gep->getPointerOperand() != pointer || gep->getNumIndices() != 1 ||
              !element->isIntegerTy() || llvm::cast< llvm::PointerType >(gep->getType())->getAddressSpace() != 0 ||
              !gep->getOperand(1)->getType()->isIntegerTy(32)) return {};
          offsets.emplace(gep, Offset{gep->getOperand(1), layout.getTypeAllocSize(element).getFixedValue()});
          pending.push_back(gep);
        } else if (auto* load = llvm::dyn_cast< llvm::LoadInst >(instruction)) {
          if (load->getPointerOperand() != pointer || !load->getType()->isIntegerTy() ||
              !bounded(offset, layout.getTypeStoreSize(load->getType()).getFixedValue(), allocation, load, loops, dom)) return {};
          result.accesses.insert(load);
        } else if (auto* store = llvm::dyn_cast< llvm::StoreInst >(instruction)) {
          if (store->getPointerOperand() == pointer) {
            if (!store->getValueOperand()->getType()->isIntegerTy() ||
                !bounded(offset, layout.getTypeStoreSize(store->getValueOperand()->getType()).getFixedValue(), allocation, store, loops, dom)) return {};
            result.accesses.insert(store);
          } else {
            auto* tls = llvm::dyn_cast< llvm::GlobalVariable >(store->getPointerOperand());
            if (offset.index || tls_store || !tls || !tls->isThreadLocal() ||
                tls->getAddressSpace() != 0 || !tls->hasInitializer() ||
                !llvm::isa< llvm::ConstantPointerNull >(tls->getInitializer()) ||
                store->isVolatile() || store->isAtomic() || may_follow(store, store)) return {};
            tls_store = store;
            for (auto* use : tls->users()) {
              if (use == store) continue;
              auto* read = llvm::dyn_cast< llvm::LoadInst >(use);
              if (!read || read->getFunction() != f || read->getPointerOperand() != tls ||
                  !read->getType()->isPointerTy() ||
                  llvm::cast< llvm::PointerType >(read->getType())->getAddressSpace() != 0 || read->isVolatile() || read->isAtomic() ||
                  !dom.dominates(store, read)) return {};
              pointer_uses.insert(read);
              offsets.emplace(read, Offset{}); pending.push_back(read);
            }
          }
        } else if (auto* call = llvm::dyn_cast< llvm::CallBase >(instruction)) {
          auto* target = callee(call);
          if (offset.index || release || !target || !target->isDeclaration() ||
              target->getName() != "free" || !plain_call(call, target) || call->arg_size() != 1 ||
              call->getArgOperand(0) != pointer || !target->getReturnType()->isVoidTy() ||
              may_follow(call, call)) return {};
          release = call;
        } else if (auto* compare = llvm::dyn_cast< llvm::ICmpInst >(instruction)) {
          if (!compare->isEquality() ||
              !(llvm::isa< llvm::ConstantPointerNull >(compare->getOperand(0)) ||
                llvm::isa< llvm::ConstantPointerNull >(compare->getOperand(1)))) return {};
        } else return {};
      }
    }
    // No old alias or TLS reload can be used after free, including on a backedge.
    if (release) for (auto* use : pointer_uses)
      if (use != release && may_follow(release, use)) return {};
    result.allocation = ar_allocation;
    return result;
  }
  // One create attempt consumes the current allocation, even on failure.
  // Stop at the allocation event, not its entire block (A and C may share it).
  bool fresh_before_each(llvm::CallBase* allocation, llvm::CallBase* create) {
    llvm::DominatorTree dom(*create->getFunction());
    if (!dom.dominates(allocation, create)) return false;
    std::vector< llvm::Instruction* > pending{create->getNextNode()};
    std::set< llvm::Instruction* > seen;
    while (!pending.empty()) {
      auto* instruction = pending.back(); pending.pop_back();
      if (!instruction || instruction == allocation) continue;
      if (!spend() || instruction == create) return false;
      if (!seen.insert(instruction).second) continue;
      if (instruction->isTerminator()) {
        for (auto* next : llvm::successors(instruction->getParent()))
          pending.push_back(&next->front());
      } else pending.push_back(instruction->getNextNode());
    }
    return true;
  }
  bool ar_pointer_origin(ar::Value* value, ar::Value* allocation) {
    for (unsigned depth = 0; depth < 32; ++depth) {
      if (value == allocation) return true;
      if (!value->type()->is_pointer()) return false;
      auto* variable = dyn_cast< ar::InternalVariable >(value);
      auto* definition = variable ? unique_def(variable) : nullptr;
      auto* cast = dyn_cast_or_null< ar::UnaryOperation >(definition);
      if (!cast || cast->op() != ar::UnaryOperation::Bitcast ||
          !cast->operand()->type()->is_pointer()) return false;
      value = cast->operand();
    }
    return false;
  }
  // This ABI gate is deliberately separate from native_handle_api's NULL
  // argument contract. Only the closed allocation chain may supply arg 4.
  ar::Function* handoff_entry(llvm::CallBase* create, ar::CallBase* allocation) {
    auto* api = callee(create);
    if (!plain_call(create, api) || !api->isDeclaration() || api->getName() != "pthread_create" ||
        create->arg_size() != 4 || !create->use_empty() || !create->getType()->isIntegerTy(32) ||
        !llvm::isa< llvm::ConstantPointerNull >(create->getArgOperand(1)) ||
        !calls.count(create) || !calls.at(create)) return nullptr;
    for (unsigned i = 0; i < 4; ++i) {
      auto* type = llvm::dyn_cast< llvm::PointerType >(create->getArgOperand(i)->getType());
      if (!type || type->getAddressSpace() != 0) return nullptr;
    }
    auto* output = llvm::cast< llvm::PointerType >(create->getArgOperand(0)->getType());
    auto* worker = llvm::dyn_cast< llvm::Function >(create->getArgOperand(2)->stripPointerCasts());
    auto* signature = llvm::cast< llvm::PointerType >(create->getArgOperand(2)->getType());
    auto* byte_pointer = llvm::Type::getInt8PtrTy(create->getContext());
    if (!output->getPointerElementType()->isIntegerTy(32) || !worker || worker->isDeclaration() ||
        worker->isVarArg() || worker->arg_size() != 1 || worker->getReturnType() != byte_pointer ||
        worker->getFunctionType()->getParamType(0) != byte_pointer ||
        create->getArgOperand(3)->getType() != byte_pointer ||
        signature->getPointerElementType() != worker->getFunctionType() ||
        worker->getCallingConv() != llvm::CallingConv::C || worker->getName() == "main") return nullptr;
    auto* ar_create = calls.at(create);
    auto* ar_api = native_handle_callee(ar_create);
    auto* result = ar_api ? dyn_cast< ar::IntegerType >(ar_api->type()->return_type()) : nullptr;
    if (!ar_api || !ar_api->is_declaration() || ar_api->name() != "pthread_create" ||
        !result || result->bit_width() != 32 || ar_api->type()->is_var_arg() ||
        ar_api->num_parameters() != 4 || ar_create->num_arguments() != 4 ||
        ar_create->code() != allocation->code() ||
        !isa< ar::NullConstant >(ar_create->argument(1)) ||
        !isa< ar::FunctionPointerConstant >(ar_create->argument(2)) ||
        !ar_pointer_origin(ar_create->argument(3), allocation->result())) return nullptr;
    for (unsigned i = 0; i < 4; ++i)
      if (ar_api->type()->param_type(i) != ar_create->argument(i)->type()) return nullptr;
    auto* child = cast< ar::FunctionPointerConstant >(ar_create->argument(2))->function();
    if (!child->is_definition() || child->num_parameters() != 1 || !child->param(0)->type()->is_pointer() ||
        !child->type()->return_type()->is_pointer() || !child->has_frontend() ||
        child->frontend< llvm::Value >() != worker) return nullptr;
    // All actual worker activations must use this one certified transfer.
    std::vector< llvm::Value* > pending{worker};
    std::set< llvm::Value* > seen;
    while (!pending.empty()) {
      auto* value = pending.back(); pending.pop_back();
      if (!spend()) return nullptr;
      if (!seen.insert(value).second) continue;
      for (auto* user : value->users()) {
        if (llvm::isa< llvm::BitCastOperator >(user)) pending.push_back(user);
        else if (user != create || create->getArgOperand(2) != value) return nullptr;
      }
    }
    return child;
  }
  bool constant_offset(llvm::GetElementPtrInst* gep, core::ZNumber& offset) {
    auto* type = gep->getSourceElementType();
    const auto& layout = gep->getModule()->getDataLayout();
    bool first = true;
    for (auto it = gep->idx_begin(); it != gep->idx_end(); ++it) {
      auto* index = llvm::dyn_cast< llvm::ConstantInt >(it->get());
      if (!index || !index->getType()->isIntegerTy(32) || !type->isSized()) return false;
      auto value = core::ZNumber(index->getSExtValue());
      if (first) {
        if (layout.getTypeAllocSize(type).isScalable()) return false;
        offset += value * core::ZNumber(layout.getTypeAllocSize(type).getFixedValue());
        first = false;
      } else if (auto* structure = llvm::dyn_cast< llvm::StructType >(type)) {
        if (value < 0 || value >= core::ZNumber(structure->getNumElements())) return false;
        auto field = index->getZExtValue();
        offset += core::ZNumber(layout.getStructLayout(structure)->getElementOffset(field));
        type = structure->getElementType(field);
      } else if (auto* array = llvm::dyn_cast< llvm::ArrayType >(type)) {
        type = array->getElementType();
        if (!type->isSized() || layout.getTypeAllocSize(type).isScalable()) return false;
        offset += value * core::ZNumber(layout.getTypeAllocSize(type).getFixedValue());
      } else return false;
    }
    return true;
  }
  Certificate certify_handoff(ar::CallBase* ar_allocation) {
    Certificate result;
    auto* allocation = ar_allocation->has_frontend()
        ? llvm::dyn_cast< llvm::CallBase >(ar_allocation->frontend< llvm::Value >()) : nullptr;
    auto* api = allocation ? callee(allocation) : nullptr;
    auto* ar_api = native_handle_callee(ar_allocation);
    auto* main = ctx.opts.entry_points.front();
    if (!api || !ar_api || !ar_api->is_declaration() || ar_api->intrinsic_id() != ar::Intrinsic::LibcMalloc ||
        !plain_call(allocation, api) || !api->isDeclaration() || api->getName() != "malloc" ||
        allocation->arg_size() != 1 || ar_allocation->num_arguments() != 1 || !ar_allocation->has_result() ||
        ar_allocation->code() != main->body() || !main->has_frontend() ||
        main->frontend< llvm::Function >() != allocation->getFunction() ||
        !allocation->getFunction()->use_empty() || !calls.count(allocation) || calls.at(allocation) != ar_allocation ||
        !same_bits(ar_allocation->argument(0), allocation->getArgOperand(0)) ||
        allocation->getType() != llvm::Type::getInt8PtrTy(allocation->getContext()) ||
        !api->getFunctionType()->getParamType(0)->isIntegerTy(32)) return result;
    auto* size = llvm::dyn_cast< llvm::ConstantInt >(allocation->getArgOperand(0));
    if (!size || size->isZero()) return result;
    auto* source_main = allocation->getFunction();
    const auto& layout = source_main->getParent()->getDataLayout();
    llvm::Triple target(source_main->getParent()->getTargetTriple());
    if (layout.getPointerSizeInBits() != 32 || layout.getIndexSizeInBits(0) != 32 ||
        target.getArch() != llvm::Triple::x86 || !target.isOSLinux() ||
        target.getEnvironment() != llvm::Triple::GNU ||
        !mutex_effect_pointer_representations_closed(*source_main->getParent())) return result;
    llvm::CallBase* create = nullptr;
    std::vector< llvm::Value* > pending{allocation};
    std::set< llvm::Value* > seen;
    while (!pending.empty()) {
      auto* pointer = pending.back(); pending.pop_back();
      if (!spend()) return {};
      if (!seen.insert(pointer).second) continue;
      for (auto* user : pointer->users()) {
        auto* instruction = llvm::dyn_cast< llvm::Instruction >(user);
        if (!instruction || instruction->getFunction() != source_main) return {};
        if (llvm::isa< llvm::DbgInfoIntrinsic >(instruction)) continue;
        if (auto* cast = llvm::dyn_cast< llvm::BitCastInst >(instruction)) {
          auto* type = llvm::dyn_cast< llvm::PointerType >(cast->getType());
          if (!type || type->getAddressSpace() != 0) return {};
          pending.push_back(cast);
        } else if (auto* comparison = llvm::dyn_cast< llvm::ICmpInst >(instruction)) {
          // This guard observes the fresh base without transferring ownership.
          ar::Comparison* guard = nullptr;
          if (!heap_initialization_successor(ar_allocation->parent(), guard) ||
              !guard || !guard->has_frontend() ||
              guard->frontend< llvm::Value >() != comparison) return {};
        } else if (auto* call = llvm::dyn_cast< llvm::CallBase >(instruction)) {
          if ((create && create != call) || call->arg_size() != 4 || call->getArgOperand(3) != pointer) return {};
          create = call;
          for (unsigned i = 0; i < 3; ++i) if (call->getArgOperand(i) == pointer) return {};
        } else return {};
      }
    }
    if (!create || !fresh_before_each(allocation, create)) return {};
    auto* worker = handoff_entry(create, ar_allocation);
    if (!worker) return {};
    auto* source_worker = worker->frontend< llvm::Function >();
    std::set< llvm::Function* > active, done;
    if (!effects_closed(source_main, allocation, active, done, create)) return {};
    active.clear(); done.clear();
    if (!effects_closed(source_worker, allocation, active, done)) return {};
    const auto extent = core::ZNumber(size->getZExtValue());
    std::map< llvm::Value*, core::ZNumber > offsets{{source_worker->getArg(0), core::ZNumber(0)}};
    pending = {source_worker->getArg(0)}; seen.clear();
    std::set< llvm::Instruction* > uses;
    llvm::CallBase* release = nullptr;
    while (!pending.empty()) {
      auto* pointer = pending.back(); pending.pop_back();
      if (!spend()) return {};
      if (!seen.insert(pointer).second) continue;
      auto offset = offsets.at(pointer);
      for (auto* user : pointer->users()) {
        auto* instruction = llvm::dyn_cast< llvm::Instruction >(user);
        if (!instruction || instruction->getFunction() != source_worker) return {};
        if (llvm::isa< llvm::DbgInfoIntrinsic >(instruction)) continue;
        uses.insert(instruction);
        if (auto* cast = llvm::dyn_cast< llvm::BitCastInst >(instruction)) {
          auto* type = llvm::dyn_cast< llvm::PointerType >(cast->getType());
          if (!type || type->getAddressSpace() != 0) return {};
          offsets.emplace(cast, offset); pending.push_back(cast);
        } else if (auto* gep = llvm::dyn_cast< llvm::GetElementPtrInst >(instruction)) {
          auto derived = offset;
          if (gep->getPointerOperand() != pointer || !constant_offset(gep, derived) || derived < 0 || derived > extent) return {};
          offsets.emplace(gep, derived); pending.push_back(gep);
        } else if (auto* load = llvm::dyn_cast< llvm::LoadInst >(instruction)) {
          if (load->getPointerOperand() != pointer || !load->getType()->isIntegerTy() ||
              offset < 0 || offset + core::ZNumber(layout.getTypeStoreSize(load->getType()).getFixedValue()) > extent) return {};
          result.accesses.insert(load);
        } else if (auto* store = llvm::dyn_cast< llvm::StoreInst >(instruction)) {
          if (store->getPointerOperand() != pointer || !store->getValueOperand()->getType()->isIntegerTy() ||
              offset < 0 || offset + core::ZNumber(layout.getTypeStoreSize(store->getValueOperand()->getType()).getFixedValue()) > extent) return {};
          result.accesses.insert(store);
        } else if (auto* call = llvm::dyn_cast< llvm::CallBase >(instruction)) {
          auto* called = callee(call);
          if (release || offset != 0 || !plain_call(call, called) || !called->isDeclaration() ||
              called->getName() != "free" || call->arg_size() != 1 || call->getArgOperand(0) != pointer ||
              !call->getType()->isVoidTy() || may_follow(call, call)) return {};
          release = call;
        } else if (auto* compare = llvm::dyn_cast< llvm::ICmpInst >(instruction)) {
          if (!compare->isEquality() || !(llvm::isa< llvm::ConstantPointerNull >(compare->getOperand(0)) ||
                                        llvm::isa< llvm::ConstantPointerNull >(compare->getOperand(1)))) return {};
        } else return {};
      }
    }
    if (release) for (auto* use : uses) if (use != release && may_follow(release, use)) return {};
    // Returning the argument is a publication, already rejected by use closure.
    for (auto& block : *source_worker)
      if (auto* ret = llvm::dyn_cast< llvm::ReturnInst >(block.getTerminator()))
        if (!llvm::isa< llvm::ConstantPointerNull >(ret->getReturnValue())) return {};
    result.allocation = ar_allocation; result.worker = worker;
    return result;
  }
  bool access(const AccessRecord& record) const {
    if (!record.pts.is_set() || record.pts.is_empty() || !record.stmt->has_frontend() || record.width == 0) return false;
    auto* source = llvm::dyn_cast< llvm::Instruction >(record.stmt->frontend< llvm::Value >());
    if (!source || !effects.count(source) || effects.at(source) != record.stmt) return false;
    for (auto* object : record.pts) {
      auto* heap = dyn_cast< DynAllocMemoryLocation >(object);
      auto it = heap ? certificates.find(heap->call()) : certificates.end();
      if (!heap || it == certificates.end() || !it->second.allocation ||
          !it->second.accesses.count(source) ||
          record.thread_id != record.stmt->code()->function()->name()) return false;
      if (auto* worker = it->second.worker) {
        if (record.stmt->code() != worker->body() || !record.call_context || !record.call_context->empty() ||
            !heap->context() || !heap->context()->empty()) return false;
      } else if (heap->context() != record.call_context || heap->call()->code() != record.stmt->code()) return false;
      auto* load = llvm::dyn_cast< llvm::LoadInst >(source);
      auto* store = llvm::dyn_cast< llvm::StoreInst >(source);
      if ((!load && !store) || (load && record.kind != AccessKind::Read) ||
          (store && record.kind != AccessKind::Write)) return false;
      auto* type = load ? load->getType() : store->getValueOperand()->getType();
      if (source->getModule()->getDataLayout().getTypeStoreSize(type).getFixedValue() != record.width) return false;
    }
    return true;
  }
public:
  Impl(Context& context, const std::vector< AccessRecord >& accesses) : ctx(context) {
    if (!ctx.opts.enable_thread_modular || ctx.opts.procedural == Procedural::Intraprocedural ||
        ctx.opts.entry_points.size() != 1 || ctx.opts.entry_points.front()->name() != "main" ||
        ctx.opts.entry_points.front()->num_parameters() != 0 ||
        ctx.bundle->data_layout().pointers.bit_width != 32 ||
        !ctx.opts.no_init_globals.empty() || ctx.concurrent_env.is_dirty() ||
        !ctx.concurrent_env.unmodeled_effects().empty() ||
        !ctx.concurrent_env.unmodeled_thread_creations().empty() ||
        ctx.bundle->global_or_null("ar.global_ctors") || ctx.bundle->global_or_null("ar.global_dtors")) return;
    for (auto fi = ctx.bundle->function_begin(); fi != ctx.bundle->function_end(); ++fi)
      if ((*fi)->is_definition()) for (auto* block : *(*fi)->body()) for (auto* stmt : *block) {
        if (!stmt->has_frontend()) continue;
        auto* original = stmt->frontend< llvm::Value >();
        if (auto* call = dyn_cast< ar::CallBase >(stmt)) {
          auto inserted = calls.emplace(original, call);
          if (!inserted.second) inserted.first->second = nullptr;
        }
        if (isa< ar::Load >(stmt) || isa< ar::Store >(stmt)) {
          auto inserted = effects.emplace(original, stmt);
          if (!inserted.second) inserted.first->second = nullptr;
        }
      }
    for (const auto& record : accesses) if (record.pts.is_set()) for (auto* object : record.pts)
      if (auto* heap = dyn_cast< DynAllocMemoryLocation >(object))
        if (!certificates.count(heap->call())) {
          auto* api = native_handle_callee(heap->call());
          auto proof = api && api->intrinsic_id() == ar::Intrinsic::LibcMalloc
              ? certify_handoff(heap->call()) : certify(heap->call());
          certificates.emplace(heap->call(), std::move(proof));
        }
  }
  bool excludes(const AccessRecord& a, const AccessRecord& b) const { return access(a) && access(b); }
  const char* contract(const AccessRecord& record) const {
    for (auto* object : record.pts) {
      auto* heap = cast< DynAllocMemoryLocation >(object);
      if (certificates.at(heap->call()).worker) return "closed-main-child-malloc-handoff-v1";
    }
    return "closed-worker-calloc-v1";
  }
};
ThreadPrivateHeap::ThreadPrivateHeap(Context& ctx, const std::vector< AccessRecord >& accesses)
    : _impl(std::make_unique< Impl >(ctx, accesses)) {}
ThreadPrivateHeap::~ThreadPrivateHeap() = default;
const char* ThreadPrivateHeap::contract(const AccessRecord& a) const { return _impl->contract(a); }
bool ThreadPrivateHeap::excludes(const AccessRecord& a, const AccessRecord& b) const { return _impl->excludes(a, b); }
} // namespace analyzer
} // namespace ikos
