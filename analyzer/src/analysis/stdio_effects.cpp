// Copyright and license: see execution_engine/numerical.hpp.
#include <llvm/IR/Operator.h>
#include <ikos/analyzer/analysis/execution_engine/native_thread_handles.hpp>
#include <ikos/analyzer/analysis/execution_engine/stdio_effects.hpp>
#include <ikos/analyzer/analysis/source_call_abi.hpp>
#include <set>
#include <string>
#include <vector>

namespace ikos {
namespace analyzer {
namespace {

ar::Variable* zero_object(ar::Value* value) {
  for (unsigned depth = 0; depth < 32; ++depth) {
    if (isa< ar::GlobalVariable >(value) || isa< ar::LocalVariable >(value))
      return cast< ar::Variable >(value);
    auto* variable = dyn_cast< ar::InternalVariable >(value);
    auto* definition = variable ? unique_def(variable) : nullptr;
    if (auto* copy = dyn_cast_or_null< ar::Assignment >(definition)) {
      value = copy->operand();
    } else if (auto* conversion =
                   dyn_cast_or_null< ar::UnaryOperation >(definition)) {
      if (conversion->op() != ar::UnaryOperation::Bitcast)
        return nullptr;
      value = conversion->operand();
    } else if (auto* shift = dyn_cast_or_null< ar::PointerShift >(definition)) {
      for (auto term = shift->term_begin(); term != shift->term_end(); ++term) {
        auto* index = dyn_cast< ar::IntegerConstant >((*term).second);
        if (!index || !index->value().is_zero())
          return nullptr;
      }
      value = shift->pointer();
    } else {
      return nullptr;
    }
  }
  return nullptr;
}

bool byte_pointer(llvm::Type* native, ar::Type* imported) {
  auto* p = llvm::dyn_cast< llvm::PointerType >(native);
  auto* q = dyn_cast< ar::PointerType >(imported);
  auto* byte = q ? dyn_cast< ar::IntegerType >(q->pointee()) : nullptr;
  return p && !p->isOpaque() && p->getAddressSpace() == 0 &&
         p->getPointerElementType()->isIntegerTy(8) && byte &&
         byte->bit_width() == 8;
}

bool exact_percent_d(ar::Value* imported, llvm::Value* native) {
  auto* global =
      llvm::dyn_cast< llvm::GlobalVariable >(native->stripPointerCasts());
  auto* object = dyn_cast_or_null< ar::GlobalVariable >(zero_object(imported));
  if (!global || !object || !object->is_definition() ||
      !object->has_frontend() || object->frontend< llvm::Value >() != global ||
      !global->isConstant() || global->getAddressSpace() != 0 ||
      global->isExternallyInitialized() || !global->hasInitializer())
    return false;
  auto* bytes =
      llvm::dyn_cast< llvm::ConstantDataArray >(global->getInitializer());
  if (!bytes || !bytes->getElementType()->isIntegerTy(8) ||
      !bytes->isCString() || bytes->getAsCString() != "%d" ||
      bytes->getNumElements() != 3)
    return false;
  ar::ArrayConstant* array = nullptr;
  for (auto* block : *object->initializer())
    for (auto* statement : *block) {
      auto* store = dyn_cast< ar::Store >(statement);
      if (!store || store->pointer() != object || array)
        return false;
      array = dyn_cast< ar::ArrayConstant >(store->value());
      if (!array)
        return false;
    }
  if (!array || array->values().size() != 3)
    return false;
  for (unsigned i = 0; i < 3; ++i) {
    auto* value = dyn_cast< ar::IntegerConstant >(array->values()[i]);
    if (!value || value->value().bit_width() != 8 ||
        value->value().to_z_number() !=
            core::ZNumber(bytes->getElementAsInteger(i)))
      return false;
  }
  return true;
}

llvm::CallInst* scalar_scan_declaration(ar::CallBase* call) {
  auto* site =
      call && call->has_frontend()
          ? llvm::dyn_cast< llvm::CallInst >(call->frontend< llvm::Value >())
          : nullptr;
  auto* source = site ? llvm::dyn_cast< llvm::Function >(
                            site->getCalledOperand()->stripPointerCasts())
                      : nullptr;
  if (!source ||
      (source->getName() != "sscanf" && source->getName() != "__isoc99_sscanf"))
    return nullptr;
  auto* imported = native_handle_callee(call);
  if (!imported || !isa< ar::Call >(call) || !source->isDeclaration() ||
      !imported->is_declaration() || site->isInlineAsm() ||
      site->hasOperandBundles() ||
      site->getCallingConv() != llvm::CallingConv::C ||
      source->getCallingConv() != llvm::CallingConv::C ||
      !source->getReturnType()->isIntegerTy(32) ||
      !site->getType()->isIntegerTy(32) ||
      site->getFunctionType() != source->getFunctionType() ||
      !source->isVarArg() || source->arg_size() != 2 || site->arg_size() != 3 ||
      !imported->type()->is_var_arg() || imported->num_parameters() != 2 ||
      call->num_arguments() != 3)
    return nullptr;
  if (unsupported_source_call_abi(call)) return nullptr;
  // Mapped libc intrinsics intentionally may have no source frontend identity.
  // This exception is only the actual sscanf API + exact LibcSscanf intrinsic.
  if (imported->has_frontend()) {
    if (imported->frontend< llvm::Value >() != source)
      return nullptr;
  } else if (imported->intrinsic_id() != ar::Intrinsic::LibcSscanf) {
    return nullptr;
  }
  if (imported->intrinsic_id() != ar::Intrinsic::LibcSscanf &&
      imported->name() != source->getName().str())
    return nullptr;
  auto* result = dyn_cast< ar::IntegerType >(imported->type()->return_type());
  if (!result || result->bit_width() != 32 || !result->is_signed())
    return nullptr;
  if (call->has_result() &&
      call->result()->type() != imported->type()->return_type())
    return nullptr;
  // Format/output/rc/errno stay i32; only pointer storage follows the
  // source/AR-checked GNU little-endian x86 or x86-64 layout.
  const auto abi = pthread_object_layout(call->bundle(), source->getParent());
  if (!abi || !call->bundle()->data_layout().is_little_endian())
    return nullptr;
  for (unsigned i = 0; i < 3; ++i) {
    if (i < 2 &&
        (site->getArgOperand(i)->getType() !=
             source->getFunctionType()->getParamType(i) ||
         call->argument(i)->type() != imported->type()->param_type(i) ||
         !byte_pointer(site->getArgOperand(i)->getType(),
                       call->argument(i)->type())))
      return nullptr;
    for (auto attribute : {llvm::Attribute::ByVal,
                           llvm::Attribute::StructRet,
                           llvm::Attribute::InAlloca,
                           llvm::Attribute::Preallocated})
      if (site->paramHasAttr(i, attribute) ||
          (i < 2 && source->hasParamAttribute(i, attribute)))
        return nullptr;
  }
  return site;
}

bool verified_stderr_declaration(ar::Bundle* bundle, llvm::GlobalVariable& g) {
  const auto abi = pthread_object_layout(bundle, g.getParent());
  if (!abi || !bundle->data_layout().is_little_endian())
    return false;
  auto* type = llvm::dyn_cast< llvm::PointerType >(g.getValueType());
  auto* file =
      type && !type->isOpaque()
          ? llvm::dyn_cast< llvm::StructType >(type->getPointerElementType())
          : nullptr;
  if (g.getName() != "stderr" || !g.isDeclaration() || g.isConstant() ||
      g.isThreadLocal() || g.isExternallyInitialized() ||
      g.getAddressSpace() != 0 || !type || type->getAddressSpace() != 0 ||
      !file || !file->hasName() || file->getName() != "struct._IO_FILE")
    return false;
  unsigned matches = 0;
  for (auto it = bundle->global_begin(); it != bundle->global_end(); ++it) {
    auto* object = *it;
    if (!object->has_frontend() || object->frontend< llvm::Value >() != &g)
      continue;
    if (object->is_definition() || object->is_thread_local() ||
        !object->type()->pointee()->is_pointer() ||
        bundle->data_layout().store_size_in_bytes(object->type()->pointee()) !=
            abi.word)
      return false;
    ++matches;
  }
  return matches == 1;
}

bool errno_result_uses_closed(llvm::CallInst* call, unsigned& budget) {
  std::vector< llvm::Value* > pending{call};
  std::set< llvm::Value* > seen;
  while (!pending.empty()) {
    if (!budget)
      return false;
    --budget;
    auto* value = pending.back();
    pending.pop_back();
    if (!seen.insert(value).second)
      continue;
    for (auto* use : value->users()) {
      auto* instruction = llvm::dyn_cast< llvm::Instruction >(use);
      if (instruction && instruction->getFunction() != call->getFunction())
        return false;
      if (llvm::isa< llvm::BitCastOperator >(use)) {
        pending.push_back(use);
      } else if (auto* gep = llvm::dyn_cast< llvm::GEPOperator >(use)) {
        if (gep->getPointerOperand() != value || !gep->hasAllZeroIndices())
          return false;
        pending.push_back(use);
      } else if (auto* load = llvm::dyn_cast< llvm::LoadInst >(use)) {
        if (load->getPointerOperand() != value ||
            !load->getType()->isIntegerTy(32) || load->isAtomic() ||
            load->isVolatile())
          return false;
      } else if (auto* store = llvm::dyn_cast< llvm::StoreInst >(use)) {
        if (store->getPointerOperand() != value ||
            !store->getValueOperand()->getType()->isIntegerTy(32) ||
            store->isAtomic() || store->isVolatile())
          return false;
      } else if (!llvm::isa< llvm::DbgInfoIntrinsic >(use)) {
        return false;
      }
    }
  }
  return true;
}

bool errno_call_binding(ar::Bundle* bundle, llvm::CallInst* call) {
  auto* source = llvm::dyn_cast< llvm::Function >(
      call->getCalledOperand()->stripPointerCasts());
  auto* pointer = llvm::dyn_cast< llvm::PointerType >(call->getType());
  if (!source || !source->isDeclaration() || source->isVarArg() ||
      !source->arg_empty() || !call->arg_empty() ||
      source->getCallingConv() != llvm::CallingConv::C ||
      call->getCallingConv() != llvm::CallingConv::C ||
      call->getFunctionType() != source->getFunctionType() ||
      call->isInlineAsm() || call->hasOperandBundles() || !pointer ||
      pointer->isOpaque() || pointer->getAddressSpace() != 0 ||
      !pointer->getPointerElementType()->isIntegerTy(32) ||
      !ordinary_source_call_abi_attributes(call, source))
    return false;
  unsigned matches = 0;
  for (auto fit = bundle->function_begin(); fit != bundle->function_end();
       ++fit) {
    if (!(*fit)->is_definition())
      continue;
    for (auto* block : *(*fit)->body())
      for (auto* statement : *block) {
        if (!statement->has_frontend() ||
            statement->frontend< llvm::Value >() != call)
          continue;
        auto* imported = dyn_cast< ar::Call >(statement);
        auto* callee = imported ? native_handle_callee(imported) : nullptr;
        auto* result =
            imported && imported->has_result()
                ? dyn_cast< ar::PointerType >(imported->result()->type())
                : nullptr;
        auto* integer =
            result ? dyn_cast< ar::IntegerType >(result->pointee()) : nullptr;
        if (!callee || !callee->is_declaration() ||
            imported->num_arguments() != 0 || callee->num_parameters() != 0 ||
            callee->type()->is_var_arg() || !integer ||
            integer->bit_width() != 32 ||
            (callee->has_frontend() &&
             callee->frontend< llvm::Value >() != source) ||
            (!callee->has_frontend() &&
             callee->intrinsic_id() != ar::Intrinsic::LibcErrnoLocation) ||
            (callee->intrinsic_id() != ar::Intrinsic::LibcErrnoLocation &&
             callee->name() != source->getName().str()))
          return false;
        ++matches;
      }
  }
  return matches == 1;
}

bool implicit_state_closed(ar::Bundle* bundle, llvm::Module& module) {
  unsigned budget = 8192;
  // A used foreign data symbol may expose locale/ctype/TLS implementation
  // state. The only current exception is the actual ABI-matched stderr
  // declaration. This exception does NOT establish stream isolation or remove
  // fprintf effects.
  for (auto& global : module.globals()) {
    if (!budget)
      return false;
    --budget;
    if (global.isDeclaration() && !global.use_empty() &&
        !verified_stderr_declaration(bundle, global))
      return false;
  }
  for (auto& function : module) {
    const auto name = function.getName();
    if (name != "__errno_location" && name != "__errno" && name != "_errno" &&
        name != "__error")
      continue;
    if (function.use_empty())
      continue;
    if (!function.isDeclaration())
      return false;
    std::vector< llvm::Value* > pending{&function};
    std::set< llvm::Value* > seen;
    while (!pending.empty()) {
      if (!budget)
        return false;
      --budget;
      auto* value = pending.back();
      pending.pop_back();
      if (!seen.insert(value).second)
        continue;
      for (auto* use : value->users()) {
        if (llvm::isa< llvm::BitCastOperator >(use)) {
          pending.push_back(use);
          continue;
        }
        auto* call = llvm::dyn_cast< llvm::CallInst >(use);
        if (!call || call->getCalledOperand() != value ||
            !errno_call_binding(bundle, call) ||
            !errno_result_uses_closed(call, budget))
          return false;
      }
    }
  }
  return true;
}
} // namespace

boost::optional< ScalarScanEffect > scalar_scan_effect(ar::CallBase* call) {
  auto* site = scalar_scan_declaration(call);
  if (!site || !exact_percent_d(call->argument(1), site->getArgOperand(1)))
    return boost::none;
  auto* p =
      llvm::dyn_cast< llvm::PointerType >(site->getArgOperand(2)->getType());
  auto* q = dyn_cast< ar::PointerType >(call->argument(2)->type());
  auto* integer = q ? dyn_cast< ar::IntegerType >(q->pointee()) : nullptr;
  auto* object = zero_object(call->argument(2));
  auto* native = site->getArgOperand(2)->stripPointerCasts();
  if (!p || p->isOpaque() || p->getAddressSpace() != 0 ||
      !p->getPointerElementType()->isIntegerTy(32) || !integer ||
      integer->bit_width() != 32 || !integer->is_signed() || !object ||
      !object->has_frontend() || object->frontend< llvm::Value >() != native ||
      cast< ar::PointerType >(object->type())->pointee() != integer ||
      call->bundle()->data_layout().store_size_in_bytes(integer) != 4)
    return boost::none;
  if (auto* global = llvm::dyn_cast< llvm::GlobalVariable >(native)) {
    auto* imported = dyn_cast< ar::GlobalVariable >(object);
    if (!imported || !imported->is_definition() ||
        imported->is_thread_local() || !global->hasInitializer() ||
        global->isConstant() || global->isThreadLocal() ||
        global->isExternallyInitialized() || global->getAddressSpace() != 0 ||
        !global->getValueType()->isIntegerTy(32))
      return boost::none;
  } else {
    auto* local = llvm::dyn_cast< llvm::AllocaInst >(native);
    auto* count =
        local ? llvm::dyn_cast< llvm::ConstantInt >(local->getArraySize())
              : nullptr;
    auto* imported = dyn_cast< ar::LocalVariable >(object);
    if (!local || !imported || !count || !count->isOne() ||
        local->getFunction() != site->getFunction() ||
        !local->getAllocatedType()->isIntegerTy(32) ||
        local->getAddressSpace() != 0 ||
        imported->function()->body() != call->code())
      return boost::none;
    unsigned matches = 0;
    for (auto* block : *imported->function()->body())
      for (auto* statement : *block) {
        auto* allocation = dyn_cast< ar::Allocate >(statement);
        if (!allocation || allocation->result() != imported)
          continue;
        auto* n = dyn_cast< ar::IntegerConstant >(allocation->array_size());
        if (!allocation->has_frontend() ||
            allocation->frontend< llvm::Value >() != local ||
            allocation->allocated_type() != integer || !n ||
            n->value().to_z_number() != 1)
          return boost::none;
        ++matches;
      }
    if (matches != 1)
      return boost::none;
  }
  return ScalarScanEffect{call,
                          object,
                          implicit_state_closed(call->bundle(),
                                                *site->getModule())};
}
} // namespace analyzer
} // namespace ikos
