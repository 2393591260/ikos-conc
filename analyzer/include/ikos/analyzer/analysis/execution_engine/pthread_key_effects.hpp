// Copyright and license: see numerical.hpp.
#pragma once

#include <functional>
#include <map>
#include <set>
#include <vector>

#include <llvm/ADT/Triple.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>

#include <ikos/analyzer/analysis/execution_engine/pthread_initialization.hpp>
#include <ikos/analyzer/analysis/execution_engine/heap_pointer_effects.hpp>

namespace ikos {
namespace analyzer {

enum class PthreadKeyEffect { None, Create, Get, Set };

// POSIX.1-2024 pthread_key_create / pthread_getspecific: these contracts do
// not assume successful calls, a particular key number, or pointee privacy.
// ABI shape only; this does not certify a registered callback.
inline PthreadKeyEffect pthread_key_api_shape(llvm::CallBase* call) {
  if (!call || call->hasOperandBundles()) return PthreadKeyEffect::None;
  auto* function = llvm::dyn_cast< llvm::Function >(
      call->getCalledOperand()->stripPointerCasts());
  if (!function || !function->isDeclaration() || function->isVarArg() ||
      call->getFunctionType() != function->getFunctionType() ||
      call->getCallingConv() != llvm::CallingConv::C ||
      function->getCallingConv() != llvm::CallingConv::C)
    return PthreadKeyEffect::None;
  const llvm::Triple target(function->getParent()->getTargetTriple());
  if (!target.isOSLinux() || target.getEnvironment() != llvm::Triple::GNU ||
      target.getArch() != llvm::Triple::x86 ||
      function->getParent()->getDataLayout().getPointerSizeInBits() != 32)
    return PthreadKeyEffect::None;
  PthreadKeyEffect kind = PthreadKeyEffect::None;
  if (function->getName() == "pthread_key_create") kind = PthreadKeyEffect::Create;
  if (function->getName() == "pthread_getspecific") kind = PthreadKeyEffect::Get;
  if (function->getName() == "pthread_setspecific") kind = PthreadKeyEffect::Set;
  if (kind == PthreadKeyEffect::None) return kind;
  const unsigned count = kind == PthreadKeyEffect::Get ? 1 : 2;
  if (function->arg_size() != count || call->arg_size() != count)
    return PthreadKeyEffect::None;
  auto pointer_to = [](llvm::Type* type, llvm::Type* pointee) {
    auto* pointer = llvm::dyn_cast< llvm::PointerType >(type);
    return pointer && !pointer->isOpaque() && pointer->getAddressSpace() == 0 &&
           pointer->getPointerElementType() == pointee;
  };
  auto& context = function->getContext();
  auto* integer = llvm::Type::getInt32Ty(context);
  auto* byte_pointer = llvm::Type::getInt8PtrTy(context);
  auto* signature = function->getFunctionType();
  llvm::Type* returned = kind == PthreadKeyEffect::Get
      ? static_cast< llvm::Type* >(byte_pointer) : integer;
  if (signature->getReturnType() != returned)
    return PthreadKeyEffect::None;
  for (unsigned i = 0; i < count; ++i)
    if (call->getArgOperand(i)->getType() != signature->getParamType(i))
      return PthreadKeyEffect::None;
  if (kind == PthreadKeyEffect::Create) {
    auto* destructor = llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                             {byte_pointer}, false);
    if (!pointer_to(signature->getParamType(0), integer) ||
        !pointer_to(signature->getParamType(1), destructor))
      return PthreadKeyEffect::None;
  } else if (signature->getParamType(0) != integer ||
             (kind == PthreadKeyEffect::Set &&
              signature->getParamType(1) != byte_pointer)) {
    return PthreadKeyEffect::None;
  }
  return kind;
}

// Preserve the NULL-only source contract used by output/selector closures.
inline PthreadKeyEffect pthread_key_effect(llvm::CallBase* call) {
  auto kind = pthread_key_api_shape(call);
  if (kind == PthreadKeyEffect::Create &&
      !llvm::isa< llvm::ConstantPointerNull >(call->getArgOperand(1)))
    return PthreadKeyEffect::None;
  return kind;
}

inline bool pthread_key_program(Context& ctx) {
  return ctx.opts.procedural != Procedural::Intraprocedural &&
      ctx.opts.entry_points.size() == 1 &&
      ctx.opts.entry_points.front()->name() == "main" &&
      ctx.opts.no_init_globals.empty() &&
      !ctx.bundle->global_or_null("ar.global_ctors") &&
      !ctx.bundle->global_or_null("ar.global_dtors");
}

// Memory projection for DataRace only. A callback may abort or diverge;
// this neither executes it nor certifies completion or an assertion verdict.
inline bool pthread_key_memory_free_destructor(ar::CallBase* registration,
                                               Context& ctx) {
  if (ctx.opts.analyses.size() != 1 || ctx.opts.analyses.front() != CheckerName::DataRace ||
      registration->num_arguments() != 2 || !registration->has_frontend()) return false;
  auto* actual = dyn_cast< ar::FunctionPointerConstant >(registration->argument(1));
  auto* site = llvm::dyn_cast< llvm::CallBase >(registration->frontend< llvm::Value >());
  auto* source = site && site->arg_size() == 2 ? llvm::dyn_cast< llvm::Function >(
      site->getArgOperand(1)->stripPointerCasts()) : nullptr;
  auto* root = actual ? actual->function() : nullptr;
  auto* byte_pointer = source ? llvm::Type::getInt8PtrTy(source->getContext()) : nullptr;
  if (!llvm::isa_and_nonnull< llvm::CallInst >(site) || !root || !source || !root->is_definition() || !root->has_frontend() ||
      root->frontend< llvm::Value >() != source || source->isDeclaration() ||
      source->arg_size() != 1 || source->getFunctionType()->getParamType(0) != byte_pointer ||
      !source->getReturnType()->isVoidTy() || root->num_parameters() != 1 ||
      !root->type()->return_type()->is_void()) return false;
  auto* registration_api = llvm::dyn_cast< llvm::Function >(site->getCalledOperand()->stripPointerCasts());
  if (!registration_api) return false;
  for (unsigned i = 0; i < site->arg_size(); ++i)
    for (auto attr : {llvm::Attribute::ByVal, llvm::Attribute::StructRet,
                     llvm::Attribute::InAlloca, llvm::Attribute::Preallocated})
      if (site->paramHasAttr(i, attr) || registration_api->hasParamAttribute(i, attr)) return false;
  auto* parameter = dyn_cast< ar::PointerType >(root->param(0)->type());
  auto* byte = parameter ? dyn_cast< ar::IntegerType >(parameter->pointee()) : nullptr;
  if (!byte || byte->bit_width() != 8) return false;
  unsigned budget = 32768;
  std::set< ar::Function* > active, done;
  std::function< bool(ar::Function*) > visit = [&](ar::Function* function) {
    if (done.count(function)) return true;
    if (active.size() == 32 || !active.insert(function).second ||
        !function->is_definition() || !function->has_frontend()) return false;
    auto* native = llvm::dyn_cast< llvm::Function >(function->frontend< llvm::Value >());
    if (!native || native->isDeclaration() || native->isVarArg() ||
        native->getCallingConv() != llvm::CallingConv::C) return false;
    for (unsigned i = 0; i < native->arg_size(); ++i)
      for (auto attr : {llvm::Attribute::ByVal, llvm::Attribute::StructRet,
                       llvm::Attribute::InAlloca, llvm::Attribute::Preallocated})
        if (native->hasParamAttribute(i, attr)) return false;
    std::map< llvm::CallBase*, ar::CallBase* > calls;
    for (auto* block : *function->body()) for (auto* statement : *block) {
      if (budget == 0) return false;
      --budget;
      if (auto* call = dyn_cast< ar::CallBase >(statement)) {
        auto* original = call->has_frontend()
            ? llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >()) : nullptr;
        if (!isa< ar::Call >(call) || !original || original->getFunction() != native ||
            !calls.emplace(original, call).second) return false;
      } else if (!isa< ar::Assignment >(statement) && !isa< ar::UnaryOperation >(statement) &&
                 !isa< ar::BinaryOperation >(statement) && !isa< ar::Comparison >(statement) &&
                 !isa< ar::PointerShift >(statement) && !isa< ar::ReturnValue >(statement) &&
                 !isa< ar::Unreachable >(statement)) return false;
    }
    for (auto& block : *native) for (auto& instruction : block) {
      if (budget == 0) return false;
      --budget;
      if (llvm::isa< llvm::DbgInfoIntrinsic >(instruction)) continue;
      auto* call = llvm::dyn_cast< llvm::CallBase >(&instruction);
      if (!call) {
        if (!llvm::isa< llvm::BinaryOperator >(instruction) && !llvm::isa< llvm::CastInst >(instruction) &&
            !llvm::isa< llvm::CmpInst >(instruction) && !llvm::isa< llvm::PHINode >(instruction) &&
            !llvm::isa< llvm::SelectInst >(instruction) && !llvm::isa< llvm::GetElementPtrInst >(instruction) &&
            !llvm::isa< llvm::BranchInst >(instruction) && !llvm::isa< llvm::SwitchInst >(instruction) &&
            !llvm::isa< llvm::ReturnInst >(instruction) && !llvm::isa< llvm::UnreachableInst >(instruction)) return false;
        continue;
      }
      auto* callee = llvm::dyn_cast< llvm::Function >(call->getCalledOperand()->stripPointerCasts());
      auto found = calls.find(call);
      auto* ar_call = found == calls.end() ? nullptr : found->second;
      auto* target = ar_call ? pthread_initialization_callee(ar_call) : nullptr;
      if (!llvm::isa< llvm::CallInst >(call) || call->isInlineAsm() || call->hasOperandBundles() ||
          !callee || !target || !target->has_frontend() || target->frontend< llvm::Value >() != callee ||
          callee->isVarArg() || call->getCallingConv() != llvm::CallingConv::C ||
          callee->getCallingConv() != llvm::CallingConv::C ||
          call->getFunctionType() != callee->getFunctionType() || call->arg_size() != callee->arg_size() ||
          ar_call->num_arguments() != call->arg_size()) return false;
      calls.erase(found);
      for (unsigned i = 0; i < call->arg_size(); ++i)
        for (auto attr : {llvm::Attribute::ByVal, llvm::Attribute::StructRet,
                         llvm::Attribute::InAlloca, llvm::Attribute::Preallocated})
          if (call->paramHasAttr(i, attr) || callee->hasParamAttribute(i, attr)) return false;
      if (!callee->isDeclaration()) { if (!visit(target)) return false; continue; }
      if (!target->is_declaration()) return false;
      if (constant_assertion_effect(ar_call)) continue;
      if (callee->getName() == "abort" && call->arg_empty() && call->getType()->isVoidTy() &&
          target->intrinsic_id() == ar::Intrinsic::LibcAbort && target->num_parameters() == 0 &&
          target->type()->return_type()->is_void()) continue;
      auto* type = dyn_cast< ar::IntegerType >(target->type()->return_type());
      if (target->intrinsic_id() == ar::Intrinsic::IkosNonDet && call->arg_empty() && type &&
          call->getMetadata("ikos.freeze_uninit") &&
          ((callee->getName() == "__ikos_nondet_int" && type->bit_width() == 32 && call->getType()->isIntegerTy(32)) ||
           (callee->getName() == "__ikos_nondet_int64" && type->bit_width() == 64 && call->getType()->isIntegerTy(64)))) continue;
      return false;
    }
    if (!calls.empty()) return false;
    active.erase(function); done.insert(function); return true;
  };
  return visit(root);
}

inline PthreadKeyEffect pthread_key_effect(ar::CallBase* call, Context& ctx) {
  // The optional intraprocedural pointer prepass does not propagate opaque
  // extern returns through every copy. Do not certify effects in that mode.
  if (!pthread_key_program(ctx)) return PthreadKeyEffect::None;
  auto* source = call->has_frontend()
      ? llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >()) : nullptr;
  auto kind = pthread_key_api_shape(source);
  if (kind == PthreadKeyEffect::None) return kind;
  auto* function = pthread_initialization_callee(call);
  auto* original = llvm::cast< llvm::Function >(
      source->getCalledOperand()->stripPointerCasts());
  if (!function || !function->is_declaration() || !function->has_frontend() ||
      function->frontend< llvm::Value >() != original || function->name() != original->getName() ||
      function->type()->is_var_arg() ||
      function->num_parameters() != source->arg_size() ||
      call->num_arguments() != source->arg_size())
    return PthreadKeyEffect::None;
  auto integer = [](ar::Type* type) {
    auto* value = dyn_cast< ar::IntegerType >(type);
    return value && value->bit_width() == 32;
  };
  auto bytes = [](ar::Type* type) {
    auto* pointer = dyn_cast< ar::PointerType >(type);
    auto* value = pointer ? dyn_cast< ar::IntegerType >(pointer->pointee()) : nullptr;
    return value && value->bit_width() == 8;
  };
  if (kind == PthreadKeyEffect::Get) {
    if (!bytes(function->type()->return_type()) || !integer(call->argument(0)->type()))
      return PthreadKeyEffect::None;
  } else {
    auto* returned = dyn_cast< ar::IntegerType >(function->type()->return_type());
    if (!returned || returned->bit_width() != 32 || !returned->is_signed())
      return PthreadKeyEffect::None;
    if (kind == PthreadKeyEffect::Create) {
      auto* output = dyn_cast< ar::PointerType >(call->argument(0)->type());
      if (!output || !integer(output->pointee())) return PthreadKeyEffect::None;
      const bool null_callback = isa< ar::NullConstant >(call->argument(1)) &&
          llvm::isa< llvm::ConstantPointerNull >(source->getArgOperand(1));
      if (!null_callback && !pthread_key_memory_free_destructor(call, ctx)) return PthreadKeyEffect::None;
    } else if (!integer(call->argument(0)->type()) || !bytes(call->argument(1)->type())) {
      return PthreadKeyEffect::None;
    }
  }
  for (unsigned i = 0; i < call->num_arguments(); ++i)
    if (call->argument(i)->type() != function->type()->param_type(i))
      return PthreadKeyEffect::None;
  return kind;
}

// A standalone key used only as an opaque API selector cannot expose pointer
// bytes, including via integer loads/stores. Other uses keep byte havoc.
inline llvm::GlobalVariable* pthread_key_scalar_output(ar::CallBase* call) {
  auto* source = llvm::cast< llvm::CallBase >(call->frontend< llvm::Value >());
  auto* global = llvm::dyn_cast< llvm::GlobalVariable >(
      source->getArgOperand(0)->stripPointerCasts());
  if (!global || !global->hasInitializer() || global->isConstant() ||
      global->getAddressSpace() != 0 || !global->getValueType()->isIntegerTy(32))
    return nullptr;
  std::set< llvm::Value* > seen;
  std::vector< llvm::Value* > pending{global};
  while (!pending.empty()) {
    auto* value = pending.back(); pending.pop_back();
    if (!seen.insert(value).second) continue;
    if (seen.size() > 512) return nullptr;
    for (auto* user : value->users()) {
      if (llvm::isa< llvm::BitCastOperator >(user)) pending.push_back(user);
      else if (auto* gep = llvm::dyn_cast< llvm::GEPOperator >(user)) {
        if (gep->getPointerOperand() != value || !gep->hasAllZeroIndices()) return nullptr;
        pending.push_back(user);
      } else if (auto* load = llvm::dyn_cast< llvm::LoadInst >(user)) {
        if (load->getPointerOperand() != value || !load->getType()->isIntegerTy(32) ||
            load->isVolatile() || load->isAtomic()) return nullptr;
        for (auto* use : load->users()) {
          auto* consumer = llvm::dyn_cast< llvm::CallBase >(use);
          auto kind = pthread_key_effect(consumer);
          if ((kind != PthreadKeyEffect::Get && kind != PthreadKeyEffect::Set) ||
              consumer->getArgOperand(0) != load) return nullptr;
        }
      } else if (auto* invocation = llvm::dyn_cast< llvm::CallBase >(user)) {
        if (pthread_key_effect(invocation) != PthreadKeyEffect::Create ||
            invocation->getArgOperand(0) != value || !invocation->use_empty()) return nullptr;
      } else return nullptr;
    }
  }
  return global;
}

// A literal token passed only to setspecific cannot address application memory.
// get remains TOP: a later retrieval/dereference still needs ordinary coverage.
inline bool pthread_key_opaque_token(ar::UnaryOperation* statement, Context& ctx) {
  if (!pthread_key_program(ctx)) return false;
  auto* source = statement->has_frontend()
      ? llvm::dyn_cast< llvm::IntToPtrInst >(statement->frontend< llvm::Value >()) : nullptr;
  if (!source || !llvm::isa< llvm::ConstantInt >(source->getOperand(0))) return false;
  std::set< llvm::Value* > seen;
  std::vector< llvm::Value* > pending{source};
  while (!pending.empty()) {
    auto* value = pending.back(); pending.pop_back();
    if (!seen.insert(value).second) continue;
    if (seen.size() > 256) return false;
    for (auto* user : value->users()) {
      if (auto* cast = llvm::dyn_cast< llvm::BitCastInst >(user)) {
        pending.push_back(cast);
      } else if (auto* call = llvm::dyn_cast< llvm::CallBase >(user)) {
        if (pthread_key_effect(call) != PthreadKeyEffect::Set ||
            call->getArgOperand(1) != value) return false;
      } else return false;
    }
  }
  return true;
}

} // namespace analyzer
} // namespace ikos
