/*******************************************************************************
 *
 * \file
 * \brief Implementation of the MarkInternalInlinePass
 *
 * Author: Jorge A. Navas
 *
 * Contact: ikos@lists.nasa.gov
 *
 * Notices:
 *
 * Copyright (c) 2011-2019 United States Government as represented by the
 * Administrator of the National Aeronautics and Space Administration.
 * All Rights Reserved.
 *
 * Disclaimers:
 *
 * No Warranty: THE SUBJECT SOFTWARE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY OF
 * ANY KIND, EITHER EXPRESSED, IMPLIED, OR STATUTORY, INCLUDING, BUT NOT LIMITED
 * TO, ANY WARRANTY THAT THE SUBJECT SOFTWARE WILL CONFORM TO SPECIFICATIONS,
 * ANY IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE,
 * OR FREEDOM FROM INFRINGEMENT, ANY WARRANTY THAT THE SUBJECT SOFTWARE WILL BE
 * ERROR FREE, OR ANY WARRANTY THAT DOCUMENTATION, IF PROVIDED, WILL CONFORM TO
 * THE SUBJECT SOFTWARE. THIS AGREEMENT DOES NOT, IN ANY MANNER, CONSTITUTE AN
 * ENDORSEMENT BY GOVERNMENT AGENCY OR ANY PRIOR RECIPIENT OF ANY RESULTS,
 * RESULTING DESIGNS, HARDWARE, SOFTWARE PRODUCTS OR ANY OTHER APPLICATIONS
 * RESULTING FROM USE OF THE SUBJECT SOFTWARE.  FURTHER, GOVERNMENT AGENCY
 * DISCLAIMS ALL WARRANTIES AND LIABILITIES REGARDING THIRD-PARTY SOFTWARE,
 * IF PRESENT IN THE ORIGINAL SOFTWARE, AND DISTRIBUTES IT "AS IS."
 *
 * Waiver and Indemnity:  RECIPIENT AGREES TO WAIVE ANY AND ALL CLAIMS AGAINST
 * THE UNITED STATES GOVERNMENT, ITS CONTRACTORS AND SUBCONTRACTORS, AS WELL
 * AS ANY PRIOR RECIPIENT.  IF RECIPIENT'S USE OF THE SUBJECT SOFTWARE RESULTS
 * IN ANY LIABILITIES, DEMANDS, DAMAGES, EXPENSES OR LOSSES ARISING FROM SUCH
 * USE, INCLUDING ANY DAMAGES FROM PRODUCTS BASED ON, OR RESULTING FROM,
 * RECIPIENT'S USE OF THE SUBJECT SOFTWARE, RECIPIENT SHALL INDEMNIFY AND HOLD
 * HARMLESS THE UNITED STATES GOVERNMENT, ITS CONTRACTORS AND SUBCONTRACTORS,
 * AS WELL AS ANY PRIOR RECIPIENT, TO THE EXTENT PERMITTED BY LAW.
 * RECIPIENT'S SOLE REMEDY FOR ANY SUCH MATTER SHALL BE THE IMMEDIATE,
 * UNILATERAL TERMINATION OF THIS AGREEMENT.
 *
 ******************************************************************************/

#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/Analysis/CFG.h>
#include <llvm/Analysis/InlineCost.h>
#include <llvm/Pass.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include <map>
#include <set>
#include <vector>

#include <ikos/frontend/llvm/pass.hpp>

// NOLINTNEXTLINE(google-build-using-namespace)
using namespace llvm;

namespace {

bool unused_frozen_seed(CallInst* call, Function* target) {
  return call->getMetadata("ikos.freeze_uninit") && call->use_empty() &&
         call->arg_empty() &&
         ((target->getName() == "__ikos_nondet_int" && call->getType()->isIntegerTy(32)) ||
          (target->getName() == "__ikos_nondet_int64" && call->getType()->isIntegerTy(64)));
}

bool malloc_helper(Function& f) {
  if (f.isDeclaration() || f.isInterposable() || f.isVarArg() ||
      f.hasAddressTaken() || f.size() > 8 || f.arg_size() != 1 ||
      f.getCallingConv() != CallingConv::C || f.hasGC() || f.hasPersonalityFn() ||
      f.hasFnAttribute(Attribute::OptimizeNone) ||
      f.hasFnAttribute(Attribute::Naked) ||
      f.hasFnAttribute(Attribute::Convergent) ||
      f.hasFnAttribute(Attribute::NoDuplicate) ||
      f.hasFnAttribute(Attribute::ReturnsTwice) ||
      f.hasFnAttribute(Attribute::NullPointerIsValid) ||
      f.getName().startswith("__VERIFIER_atomic_") ||
      f.getName().startswith("__atomic_") || f.getName().startswith("__sync_") ||
      f.getName().startswith("__ikos_"))
    return false;
  const auto bits = f.getParent()->getDataLayout().getPointerSizeInBits();
  auto* returned = dyn_cast< PointerType >(f.getReturnType());
  if ((bits != 32 && bits != 64) || !returned || returned->getAddressSpace() ||
      !f.getArg(0)->getType()->isIntegerTy(bits))
    return false;
  SmallVector< std::pair< const BasicBlock*, const BasicBlock* >, 4 > backedges;
  FindFunctionBackedges(f, backedges);
  if (!backedges.empty() || !isInlineViable(f).isSuccess())
    return false;
  CallInst* allocation = nullptr;
  unsigned budget = 64;
  for (auto& block : f) {
    for (auto& instruction : block) {
      if (!budget--)
        return false;
      if (isa< DbgInfoIntrinsic >(instruction))
        continue;
      if (auto* call = dyn_cast< CallInst >(&instruction)) {
        auto* target = call->getCalledFunction();
        if (!target || !target->isDeclaration() || target->isVarArg() ||
            target->getCallingConv() != CallingConv::C ||
            call->getCallingConv() != CallingConv::C || call->hasOperandBundles() ||
            call->isMustTailCall() || call->getFunctionType() != target->getFunctionType())
          return false;
        if (unused_frozen_seed(call, target))
          continue;
        if (target->getName() == "malloc") {
          auto* type = dyn_cast< PointerType >(call->getType());
          if (allocation || !type || type->getAddressSpace() ||
              call->arg_size() != 1 || !call->getArgOperand(0)->getType()->isIntegerTy(bits))
            return false;
          allocation = call;
        } else if (!(target->getName() == "abort" && call->arg_empty() &&
                     call->getType()->isVoidTy() && call->doesNotReturn() &&
                     isa< UnreachableInst >(block.getTerminator()))) {
          return false;
        }
      } else if (auto* cast = dyn_cast< BitCastInst >(&instruction)) {
        auto* from = dyn_cast< PointerType >(cast->getOperand(0)->getType());
        auto* to = dyn_cast< PointerType >(cast->getType());
        if (!from || !to || from->getAddressSpace() || to->getAddressSpace())
          return false;
      } else if (!isa< ICmpInst >(instruction) && !isa< PHINode >(instruction) &&
                 !isa< BranchInst >(instruction) && !isa< ReturnInst >(instruction) &&
                 !isa< UnreachableInst >(instruction)) {
        return false;
      }
    }
  }
  if (!allocation)
    return false;
  std::vector< Value* > pending;
  std::set< Value* > seen;
  for (auto& block : f)
    if (auto* ret = dyn_cast< ReturnInst >(block.getTerminator()))
      pending.push_back(ret->getReturnValue());
  if (pending.empty())
    return false;
  while (!pending.empty()) {
    auto* value = pending.back(); pending.pop_back();
    if (!seen.insert(value).second || value == allocation || isa< ConstantPointerNull >(value))
      continue;
    if (auto* cast = dyn_cast< BitCastInst >(value))
      pending.push_back(cast->getOperand(0));
    else if (auto* phi = dyn_cast< PHINode >(value)) {
      for (auto& incoming : phi->incoming_values()) pending.push_back(incoming);
    } else
      return false;
  }
  return true;
}

struct InlineMallocHelpersPass final : public ModulePass {
  static char ID;
  InlineMallocHelpersPass() : ModulePass(ID) {}

  bool runOnModule(Module& m) override {
    std::vector< CallInst* > calls;
    for (Function& f : m) {
      if (!malloc_helper(f)) continue;
      for (auto* use : f.users()) {
        auto* call = dyn_cast< CallInst >(use);
        if (call && call->getCalledFunction() == &f && !call->hasOperandBundles() &&
            !call->isMustTailCall() && call->getCallingConv() == CallingConv::C &&
            call->getFunctionType() == f.getFunctionType() &&
            !call->getAttributes().hasFnAttr(Attribute::NoInline) && calls.size() < 256)
          calls.push_back(call);
      }
    }
    bool changed = false;
    for (auto* call : calls) {
      // -O0 emits function noinline; this bounded normalization retains its attrs.
      InlineFunctionInfo info;
      changed |= InlineFunction(*call, info).isSuccess();
    }
    return changed;
  }
};

// Each actual remains observed by a non-overlapping store after inlining.
bool allocator_wrapper(Function& f) {
  if (f.isDeclaration() || f.size() != 1 || f.isVarArg() ||
      f.getCallingConv() != CallingConv::C ||
      !f.getReturnType()->isPointerTy() || f.hasAddressTaken())
    return false;
  const auto& layout = f.getParent()->getDataLayout();
  const auto bits = layout.getPointerSizeInBits();
  if (bits != 32 && bits != 64)
    return false;
  for (auto& argument : f.args())
    if (!argument.getType()->isIntegerTy(32) &&
        !argument.getType()->isIntegerTy(64))
      return false;
  std::map< Value*, uint64_t > addresses;
  std::vector< std::pair< uint64_t, uint64_t > > stores;
  std::set< Value* > observed;
  CallInst* allocation = nullptr;
  uint64_t extent = 0;
  unsigned budget = 128;
  for (auto& instruction : f.front()) {
    if (!budget--)
      return false;
    if (isa< DbgInfoIntrinsic >(instruction))
      continue;
    if (auto* call = dyn_cast< CallInst >(&instruction)) {
      auto* target = call->getCalledFunction();
      if (!target || !target->isDeclaration() || target->isVarArg() ||
          call->getCallingConv() != CallingConv::C ||
          target->getCallingConv() != CallingConv::C ||
          call->hasOperandBundles() ||
          call->getFunctionType() != target->getFunctionType())
        return false;
      if (unused_frozen_seed(call, target))
        continue;
      auto* size = call->arg_size() == 1
                       ? dyn_cast< ConstantInt >(call->getArgOperand(0))
                       : nullptr;
      if (allocation || target->getName() != "malloc" ||
          target->arg_size() != 1 || !call->getType()->isPointerTy() ||
          cast< PointerType >(call->getType())->getAddressSpace() != 0 ||
          !size || !size->getType()->isIntegerTy(bits) || size->isZero() ||
          size->getValue().ugt(1048576))
        return false;
      extent = size->getZExtValue();
      allocation = call;
      addresses.emplace(call, 0);
    } else if (auto* bitcast = dyn_cast< BitCastInst >(&instruction)) {
      auto source = addresses.find(bitcast->getOperand(0));
      auto* type = dyn_cast< PointerType >(bitcast->getType());
      if (source == addresses.end() || !type || type->getAddressSpace())
        return false;
      addresses.emplace(bitcast, source->second);
    } else if (auto* gep = dyn_cast< GetElementPtrInst >(&instruction)) {
      auto source = addresses.find(gep->getPointerOperand());
      auto* type = dyn_cast< StructType >(gep->getSourceElementType());
      auto* zero = gep->getNumIndices() == 2
                       ? dyn_cast< ConstantInt >(gep->getOperand(1))
                       : nullptr;
      auto* field = gep->getNumIndices() == 2
                        ? dyn_cast< ConstantInt >(gep->getOperand(2))
                        : nullptr;
      if (source == addresses.end() || !gep->isInBounds() || !type ||
          !type->isSized() || !zero || !zero->isZero() || !field ||
          field->getValue().uge(type->getNumElements()))
        return false;
      const auto offset =
          layout.getStructLayout(type)->getElementOffset(field->getZExtValue());
      if (offset > extent || source->second > extent - offset)
        return false;
      addresses.emplace(gep, source->second + offset);
    } else if (auto* store = dyn_cast< StoreInst >(&instruction)) {
      auto address = addresses.find(store->getPointerOperand());
      auto* value = store->getValueOperand();
      const bool formal =
          isa< Argument >(value) && cast< Argument >(value)->getParent() == &f;
      if (store->isAtomic() || store->isVolatile() ||
          address == addresses.end() ||
          (!formal && !isa< ConstantInt >(value) &&
           !isa< ConstantPointerNull >(value)))
        return false;
      if (auto* integer = dyn_cast< IntegerType >(value->getType()))
        if (integer->getBitWidth() > 64)
          return false;
      auto size = layout.getTypeStoreSize(value->getType());
      if (size.isScalable() || size.getFixedValue() == 0 ||
          address->second > extent ||
          size.getFixedValue() > extent - address->second ||
          store->getAlign() > layout.getABITypeAlign(value->getType()) ||
          address->second % store->getAlign().value())
        return false;
      const auto end = address->second + size.getFixedValue();
      for (const auto& old : stores)
        if (address->second < old.second && old.first < end)
          return false;
      stores.emplace_back(address->second, end);
      if (formal)
        observed.insert(value);
    } else if (auto* ret = dyn_cast< ReturnInst >(&instruction)) {
      auto pointer = addresses.find(ret->getReturnValue());
      if (!allocation || pointer == addresses.end() || pointer->second != 0)
        return false;
    } else {
      return false;
    }
  }
  return allocation && observed.size() == f.arg_size();
}

std::vector< CallInst* > allocator_calls(Module& m, bool local_only) {
  std::vector< CallInst* > calls;
  for (Function& f : m) {
    if ((local_only && !f.hasLocalLinkage()) || !allocator_wrapper(f))
      continue;
    for (auto* use : f.users()) {
      auto* call = dyn_cast< CallInst >(use);
      if (call && call->getCalledFunction() == &f &&
          !call->hasOperandBundles() &&
          call->getCallingConv() == CallingConv::C &&
          call->getFunctionType() == f.getFunctionType())
        calls.push_back(call);
    }
  }
  return calls;
}

struct AllocatorInventoryPass final : public ModulePass {
  static char ID;
  raw_ostream& Output;

  explicit AllocatorInventoryPass(raw_ostream& output)
      : ModulePass(ID), Output(output) {}

  void getAnalysisUsage(AnalysisUsage& usage) const override {
    usage.setPreservesAll();
  }

  bool runOnModule(Module& m) override {
    // External definitions may become local in the optional second analysis.
    Output << "{\"schema\":1,\"eligible_calls\":"
           << allocator_calls(m, false).size() << "}\n";
    return false;
  }
};

struct MarkInternalInlinePass final : public ModulePass {
  static char ID; // Pass identification
  bool AllocatorsOnly;

  explicit MarkInternalInlinePass(bool allocators_only = false)
      : ModulePass(ID), AllocatorsOnly(allocators_only) {}

  void getAnalysisUsage(AnalysisUsage& AU) const override {
    if (!AllocatorsOnly)
      AU.setPreservesAll();
  }

  bool runOnModule(Module& m) override {
    bool change = false;
    if (AllocatorsOnly) {
      auto calls = allocator_calls(m, true);
      for (auto* call : calls) {
        InlineFunctionInfo info;
        change |= InlineFunction(*call, info).isSuccess();
      }
      return change;
    }
    for (Function& f : m) {
      if (!f.isDeclaration() && f.hasLocalLinkage()) {
        f.removeFnAttr(Attribute::NoInline);
        f.addFnAttr(Attribute::AlwaysInline);
        change = true;
      }
    }
    return change;
  }

}; // end struct MarkInternalInlinePass

} // end anonymous namespace

char MarkInternalInlinePass::ID = 0;
char AllocatorInventoryPass::ID = 0;
char InlineMallocHelpersPass::ID = 0;

INITIALIZE_PASS(MarkInternalInlinePass,
                "mark-internal-inline",
                "Internalize all functions for enabling inlining",
                false,
                false);

ModulePass* ikos::frontend::pass::create_mark_internal_inline_pass() {
  return new MarkInternalInlinePass();
}

ModulePass* ikos::frontend::pass::create_inline_allocator_wrappers_pass() {
  return new MarkInternalInlinePass(true);
}

ModulePass* ikos::frontend::pass::create_inline_malloc_helpers_pass() {
  return new InlineMallocHelpersPass();
}

ModulePass* ikos::frontend::pass::create_allocator_inventory_pass(
    llvm::raw_ostream& output) {
  return new AllocatorInventoryPass(output);
}
