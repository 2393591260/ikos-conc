// Copyright and license: see numerical.hpp.
#pragma once

#include <set>
#include <vector>

#include <llvm/ADT/Triple.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <ikos/analyzer/analysis/context.hpp>

namespace ikos {
namespace analyzer {

// Native integer bits need no address-alias model when their entire pointer
// lifetime is a pthread argument followed only by native ptrtoint (or discard).
// Check both representations: frontend uses alone do not certify transformed AR.
inline bool numeric_thread_argument(ar::UnaryOperation* cast, Context& ctx) {
  const auto width = ctx.bundle->data_layout().pointers.bit_width;
  auto* integer = dyn_cast< ar::IntegerType >(cast->operand()->type());
  auto* original = cast->has_frontend()
      ? llvm::dyn_cast< llvm::IntToPtrInst >(cast->frontend< llvm::Value >()) : nullptr;
  if (width != 32 || !integer || integer->bit_width() != width || !original ||
      !original->getOperand(0)->getType()->isIntegerTy(width) ||
      original->getType()->getPointerAddressSpace() != 0 ||
      original->getModule()->getDataLayout().getPointerSizeInBits() != width)
    return false;
  const llvm::Triple target(original->getModule()->getTargetTriple());
  if (target.getArch() != llvm::Triple::x86 || !target.isOSLinux() ||
      target.getEnvironment() != llvm::Triple::GNU) return false;

  std::set< llvm::CallBase* > source_creates;
  std::set< llvm::Function* > source_roots;
  unsigned budget = 32768;
  auto source_uses = [&](llvm::Value* initial, bool creator) {
    std::set< llvm::Value* > seen;
    std::vector< llvm::Value* > pending{initial};
    while (!pending.empty()) {
      auto* value = pending.back(); pending.pop_back();
      if (!seen.insert(value).second) continue;
      for (auto* user : value->users()) {
        if (budget == 0) return false;
        --budget;
        if (llvm::isa< llvm::DbgInfoIntrinsic >(user)) continue;
        if (auto* copy = llvm::dyn_cast< llvm::BitCastInst >(user)) {
          if (!copy->getType()->isPointerTy() || copy->getType()->getPointerAddressSpace() != 0)
            return false;
          pending.push_back(copy);
        } else if (auto* to_integer = llvm::dyn_cast< llvm::PtrToIntInst >(user)) {
          if (creator || !to_integer->getType()->isIntegerTy(width)) return false;
        } else if (auto* call = llvm::dyn_cast< llvm::CallInst >(user)) {
          if (!creator || call->arg_size() != 4 || call->getArgOperand(3) != value ||
              call->hasOperandBundles() || call->isInlineAsm()) return false;
          auto* api = llvm::dyn_cast< llvm::Function >(call->getCalledOperand());
          auto* root = llvm::dyn_cast< llvm::Function >(call->getArgOperand(2));
          auto* byte_pointer = llvm::Type::getInt8PtrTy(call->getContext());
          auto* handle_pointer = llvm::Type::getInt32PtrTy(call->getContext());
          if (!api || !api->isDeclaration() || api->getName() != "pthread_create" ||
              api->isVarArg() || api->arg_size() != 4 || !api->getReturnType()->isIntegerTy(32) ||
              api->getCallingConv() != llvm::CallingConv::C ||
              call->getCallingConv() != llvm::CallingConv::C ||
              call->getFunctionType() != api->getFunctionType() ||
              !llvm::isa< llvm::ConstantPointerNull >(call->getArgOperand(1)) ||
              call->getArgOperand(0)->getType() != handle_pointer ||
              call->getArgOperand(3)->getType() != byte_pointer ||
              !root || root->isDeclaration() || root->isVarArg() || root->arg_size() != 1 ||
              root->getReturnType() != byte_pointer || root->getArg(0)->getType() != byte_pointer ||
              root->getCallingConv() != llvm::CallingConv::C) return false;
          for (unsigned i = 0; i < 4; ++i) {
            if (api->getFunctionType()->getParamType(i) != call->getArgOperand(i)->getType())
              return false;
            for (auto attribute : {llvm::Attribute::ByVal, llvm::Attribute::StructRet,
                                   llvm::Attribute::InAlloca, llvm::Attribute::Preallocated})
              if (call->paramHasAttr(i, attribute) || api->hasParamAttribute(i, attribute))
                return false;
          }
          for (auto attribute : {llvm::Attribute::ByVal, llvm::Attribute::StructRet,
                                 llvm::Attribute::InAlloca, llvm::Attribute::Preallocated})
            if (root->hasParamAttribute(0, attribute)) return false;
          source_creates.insert(call);
          source_roots.insert(root);
        } else return false;
      }
    }
    return true;
  };
  if (!source_uses(original, true) || source_creates.empty()) return false;
  for (auto* root : source_roots)
    if (!source_uses(root->getArg(0), false)) return false;

  std::set< llvm::CallBase* > matched_creates;
  std::set< ar::Function* > roots;
  auto ar_uses = [&](ar::Value* initial, ar::Code* code, bool creator) {
    std::set< ar::Value* > seen;
    std::vector< ar::Value* > pending{initial};
    while (!pending.empty()) {
      auto* value = pending.back(); pending.pop_back();
      if (!seen.insert(value).second) continue;
      for (auto* block : *code) for (auto* statement : *block) {
        if (budget == 0) return false;
        --budget;
        bool used = false;
        for (auto i = statement->op_begin(); i != statement->op_end(); ++i)
          used |= *i == value;
        if (!used) continue;
        if (auto* copy = dyn_cast< ar::Assignment >(statement)) {
          if (!copy->result()->type()->is_pointer()) return false;
          pending.push_back(copy->result());
        } else if (auto* conversion = dyn_cast< ar::UnaryOperation >(statement)) {
          if (conversion->op() == ar::UnaryOperation::Bitcast &&
              conversion->result()->type()->is_pointer()) {
            pending.push_back(conversion->result());
          } else {
            auto* result = dyn_cast< ar::IntegerType >(conversion->result()->type());
            if (creator || !result || result->bit_width() != width ||
                (conversion->op() != ar::UnaryOperation::PtrToUI &&
                 conversion->op() != ar::UnaryOperation::PtrToSI)) return false;
          }
        } else if (auto* call = dyn_cast< ar::Call >(statement)) {
          auto* api = dyn_cast< ar::FunctionPointerConstant >(call->called());
          auto* source = call->has_frontend()
              ? llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >()) : nullptr;
          if (!creator || !api || !api->function()->is_declaration() ||
              (api->function()->intrinsic_id() != ar::Intrinsic::PthreadCreate &&
               api->function()->name() != "pthread_create") ||
              call->num_arguments() != 4 || call->argument(3) != value ||
              !source || !source_creates.count(source)) return false;
          auto* entry = dyn_cast< ar::FunctionPointerConstant >(call->argument(2));
          auto* root = entry ? entry->function() : nullptr;
          if (!root || !root->is_definition() || root->num_parameters() != 1 ||
              !root->param(0)->type()->is_pointer() || !root->has_frontend() ||
              root->frontend< llvm::Value >() != source->getArgOperand(2)) return false;
          matched_creates.insert(source);
          roots.insert(root);
        } else return false;
      }
    }
    return true;
  };
  if (!cast->code()->is_function_body() ||
      !ar_uses(cast->result(), cast->code(), true) || matched_creates != source_creates)
    return false;
  for (auto* root : roots)
    if (!ar_uses(root->param(0), root->body(), false)) return false;
  return true;
}

} // namespace analyzer
} // namespace ikos
