// Copyright and license: see numerical.hpp.
#pragma once

#include <llvm/ADT/Triple.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>

#include <ikos/analyzer/analysis/context.hpp>
#include <ikos/analyzer/analysis/execution_engine/handle_origin.hpp>
#include <ikos/analyzer/analysis/literal.hpp>
#include <ikos/analyzer/analysis/memory_location.hpp>

namespace ikos {
namespace analyzer {

struct PthreadObjectLayout {
  unsigned word = 0;
  unsigned mutex = 0;
  explicit operator bool() const { return word != 0; }
};

// Sizes describe this verified target ABI, not initialization or closed uses.
inline PthreadObjectLayout pthread_object_layout(unsigned bits,
                                                 const llvm::Module* source) {
  if (!source)
    return {};
  const llvm::Triple target(source->getTargetTriple());
  const auto& layout = source->getDataLayout();
  if (!target.isOSLinux() || target.getEnvironment() != llvm::Triple::GNU ||
      !layout.isLittleEndian() || layout.getPointerSizeInBits(0) != bits ||
      !((target.getArch() == llvm::Triple::x86 && bits == 32) ||
        (target.getArch() == llvm::Triple::x86_64 && bits == 64)))
    return {};
  return {static_cast< unsigned >(bits / 8), bits == 32 ? 24u : 40u};
}

inline PthreadObjectLayout pthread_object_layout(const ar::Bundle* bundle,
                                                 const llvm::Module* source) {
  return pthread_object_layout(bundle->data_layout().pointers.bit_width, source);
}

inline unsigned mutex_array_leaf_width(const GlobalArraySubobject& object) {
  if (!object || !object.type || !object.source_type ||
      !object.global->has_frontend())
    return 0;
  auto* global = object.global;
  auto* source = global->frontend< llvm::GlobalVariable >();
  const auto abi = pthread_object_layout(global->bundle(), source->getParent());
  auto* leaf = llvm::dyn_cast< llvm::StructType >(
      object.source_type->getElementType());
  if (!abi || !leaf || leaf->isOpaque() || !leaf->hasName() ||
      leaf->getName() != "union.pthread_mutex_t")
    return 0;
  const auto& layout = source->getParent()->getDataLayout();
  const auto& ar_layout = global->bundle()->data_layout();
  const auto alignment = source->getAlign()
                             ? source->getAlign()->value()
                             : layout.getABITypeAlign(source->getValueType()).value();
  if (source->getAddressSpace() != 0 || source->isExternallyInitialized() ||
      layout.getABITypeAlign(leaf).value() != abi.word ||
      alignment < abi.word || object.begin % abi.word != 0 ||
      layout.getTypeAllocSize(leaf) != abi.mutex ||
      layout.getTypeStoreSize(leaf) != abi.mutex ||
      ar_layout.alloc_size_in_bytes(object.type->element_type()) != abi.mutex ||
      ar_layout.store_size_in_bytes(object.type->element_type()) != abi.mutex ||
      object.size != abi.mutex * object.type->num_elements())
    return 0;
  return abi.mutex;
}

inline bool mutex_array_storage_scope(ar::GlobalVariable* global,
                                     const core::ZNumber& count) {
  if (!global->has_frontend() || !global->is_definition() ||
      global->is_thread_local())
    return false;
  auto* source = global->frontend< llvm::GlobalVariable >();
  auto* array = llvm::dyn_cast< llvm::ArrayType >(source->getValueType());
  auto* ar_array = dyn_cast< ar::ArrayType >(global->type()->pointee());
  auto* leaf =
      array ? llvm::dyn_cast< llvm::StructType >(array->getElementType())
            : nullptr;
  if (!array || !ar_array || !leaf || leaf->isOpaque() || !leaf->hasName() ||
      leaf->getName() != "union.pthread_mutex_t" ||
      source->getAddressSpace() != 0 || source->isExternallyInitialized() ||
      source->isConstant() || !source->hasInitializer() ||
      !source->getInitializer()->isNullValue() ||
      core::ZNumber(array->getNumElements()) != count ||
      ar_array->num_elements() != count)
    return false;
  const auto& layout = source->getParent()->getDataLayout();
  const GlobalArraySubobject object{
      global, ar_array, array, {}, core::ZNumber(0),
      global->bundle()->data_layout().alloc_size_in_bytes(ar_array)};
  const auto width = mutex_array_leaf_width(object);
  if (!width || layout.getTypeAllocSize(array) != width * array->getNumElements() ||
      object.size != width * count)
    return false;
  std::set< llvm::Value* > visited;
  std::vector< llvm::Value* > pending{source};
  while (!pending.empty()) {
    auto* value = pending.back();
    pending.pop_back();
    if (!visited.insert(value).second)
      continue;
    if (visited.size() > 4096)
      return false;
    for (auto* user : value->users()) {
      if (auto* gep = llvm::dyn_cast< llvm::GEPOperator >(user)) {
        auto* zero =
            gep->getNumIndices() == 2
                ? llvm::dyn_cast< llvm::ConstantInt >(gep->getOperand(1))
                : nullptr;
        if (!gep->isInBounds() || gep->getPointerOperand() != source ||
            gep->getSourceElementType() != array || !zero || !zero->isZero())
          return false;
        pending.push_back(user);
      } else if (llvm::isa< llvm::BitCastOperator >(user)) {
        pending.push_back(user);
      } else if (auto* call = llvm::dyn_cast< llvm::CallBase >(user)) {
        auto* function = llvm::dyn_cast< llvm::Function >(
            call->getCalledOperand()->stripPointerCasts());
        if (!function || !function->isDeclaration() ||
            call->arg_size() == 0 || call->getArgOperand(0) != value ||
            (function->getName() != "pthread_mutex_init" &&
             function->getName() != "pthread_mutex_lock" &&
             function->getName() != "pthread_mutex_unlock"))
          return false;
        auto* actual =
            llvm::dyn_cast< llvm::GEPOperator >(value->stripPointerCasts());
        if (!actual || actual->getPointerOperand() != source ||
            actual->getSourceElementType() != array)
          return false;
      } else
        return false;
    }
  }
  return true;
}

inline bool mutex_array_storage_scope(const GlobalArraySubobject& object,
                                       const core::ZNumber& count) {
  if (!object) return false;
  if (!object.nested()) return mutex_array_storage_scope(object.global, count);
  auto* source = object.global->frontend< llvm::GlobalVariable >();
  auto* leaf = llvm::dyn_cast< llvm::StructType >(object.source_type->getElementType());
  const auto width = mutex_array_leaf_width(object);
  if (!leaf || leaf->isOpaque() || !leaf->hasName() ||
      leaf->getName() != "union.pthread_mutex_t" || !width ||
      source->isConstant() || !source->hasInitializer() || !source->getInitializer()->isNullValue() ||
      object.type->num_elements() != count || object.size != width * count)
    return false;
  const auto arrays = global_array_subobjects(object.global);
  return closed_global_array_uses(object.global, [&](llvm::User* user) {
    if (auto* call = llvm::dyn_cast< llvm::CallBase >(user)) {
      auto* function = llvm::dyn_cast< llvm::Function >(call->getCalledOperand()->stripPointerCasts());
      return function && function->isDeclaration() && call->arg_size() > 0 &&
          (function->getName() == "pthread_mutex_init" || function->getName() == "pthread_mutex_lock" ||
           function->getName() == "pthread_mutex_unlock") &&
          global_array_element(call->getArgOperand(0), object);
    }
    llvm::Value* address = nullptr;
    llvm::Type* type = nullptr;
    if (auto* load = llvm::dyn_cast< llvm::LoadInst >(user)) {
      if (load->isAtomic() || load->isVolatile()) return false;
      address = load->getPointerOperand(); type = load->getType();
    } else if (auto* store = llvm::dyn_cast< llvm::StoreInst >(user)) {
      if (store->isAtomic() || store->isVolatile()) return false;
      address = store->getPointerOperand(); type = store->getValueOperand()->getType();
    } else return false;
    for (const auto& sibling : arrays)
      if (data_pointer_array(sibling) &&
          (sibling.begin + sibling.size <= object.begin || object.begin + object.size <= sibling.begin) &&
          type == sibling.source_type->getElementType() && global_array_element(address, sibling))
        return true;
    return false;
  });
}

// The numerical bounds are collected separately; neither inbounds nor a type
// name establishes the byte range of a dynamic index.
inline GlobalArraySubobject mutex_array_effect_object(ar::CallBase* call) {
  if (!call->has_frontend()) return {};
  auto* source = llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >());
  auto* target = source ? llvm::dyn_cast< llvm::Function >(
      source->getCalledOperand()->stripPointerCasts()) : nullptr;
  if (!target || !target->isDeclaration() || target->isVarArg() ||
      source->getCallingConv() != llvm::CallingConv::C ||
      target->getCallingConv() != llvm::CallingConv::C ||
      !target->getReturnType()->isIntegerTy(32) ||
      !source->getType()->isIntegerTy(32)) return {};
  auto* address = dyn_cast< ar::FunctionPointerConstant >(call->called());
  auto* function = address ? address->function() : nullptr;
  if (!function || !function->is_declaration()) return {};
  const auto id = function->intrinsic_id();
  if ((target->getName() == "pthread_mutex_init" && function->name() != "pthread_mutex_init") ||
      (target->getName() == "pthread_mutex_lock" &&
       function->name() != "pthread_mutex_lock" && id != ar::Intrinsic::PthreadMutexLock) ||
      (target->getName() == "pthread_mutex_unlock" &&
       function->name() != "pthread_mutex_unlock" && id != ar::Intrinsic::PthreadMutexUnlock))
    return {};
  const bool init = target->getName() == "pthread_mutex_init";
  if (!init && target->getName() != "pthread_mutex_lock" &&
      target->getName() != "pthread_mutex_unlock") return {};
  unsigned n = init ? 2 : 1;
  if (call->num_arguments() != n || source->arg_size() != n ||
      target->arg_size() != n) return {};
  for (unsigned i = 0; i < n; ++i)
    if (!target->getFunctionType()->getParamType(i)->isPointerTy() ||
        !source->getArgOperand(i)->getType()->isPointerTy() ||
        !call->argument(i)->type()->is_pointer()) return {};
  if (init && !isa< ar::NullConstant >(call->argument(1))) return {};
  for (auto it = call->bundle()->global_begin(); it != call->bundle()->global_end(); ++it)
    for (const auto& object : global_array_subobjects(*it)) {
      llvm::Value* index = nullptr;
      if (global_array_element(source->getArgOperand(0), object, &index) &&
          pure_local_array_index(index) &&
          mutex_array_storage_scope(object, object.type->num_elements())) return object;
    }
  return {};
}

inline bool constant_assertion_effect(ar::CallBase* call) {
  if (!call->has_frontend() || call->num_arguments() != 4) return false;
  auto* target = dyn_cast< ar::FunctionPointerConstant >(call->called());
  auto* source = llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >());
  auto* function = source ? llvm::dyn_cast< llvm::Function >(source->getCalledOperand()->stripPointerCasts()) : nullptr;
  if (!target || !target->function()->is_declaration() || target->function()->name() != "__assert_fail" ||
      !target->function()->type()->return_type()->is_void() || target->function()->type()->is_var_arg() ||
      target->function()->num_parameters() != 4 || !function || !function->isDeclaration() ||
      function->getName() != "__assert_fail" || !target->function()->has_frontend() ||
      target->function()->frontend< llvm::Value >() != function || !function->getReturnType()->isVoidTy() ||
      function->isVarArg() || function->arg_size() != 4 || source->arg_size() != 4 ||
      !source->getType()->isVoidTy() || source->getCallingConv() != llvm::CallingConv::C ||
      function->getCallingConv() != llvm::CallingConv::C || source->getFunctionType() != function->getFunctionType()) return false;
  for (unsigned i = 0; i < 4; ++i) {
    if (call->argument(i)->type() != target->function()->type()->param_type(i)) return false;
    if (i == 2) {
      auto* integer = dyn_cast< ar::IntegerType >(call->argument(i)->type());
      if (!integer || integer->bit_width() != 32 || !source->getArgOperand(i)->getType()->isIntegerTy(32)) return false;
    } else {
      auto* pointer = dyn_cast< ar::PointerType >(call->argument(i)->type());
      auto* byte = pointer ? dyn_cast< ar::IntegerType >(pointer->pointee()) : nullptr;
      auto* source_pointer = llvm::dyn_cast< llvm::PointerType >(source->getArgOperand(i)->getType());
      auto* text = llvm::dyn_cast< llvm::GlobalVariable >(source->getArgOperand(i)->stripPointerCasts());
      if (!byte || byte->bit_width() != 8 || !source_pointer || source_pointer->getAddressSpace() != 0 ||
          !source_pointer->getPointerElementType()->isIntegerTy(8) || !text || !text->isConstant() ||
          !text->hasInitializer() || text->getAddressSpace() != 0) return false;
      auto* literal = llvm::dyn_cast< llvm::ConstantDataArray >(text->getInitializer());
      if (!literal || !literal->getElementType()->isIntegerTy(8) || !literal->isCString()) return false;
    }
  }
  return true;
}

// Native pthread_t values are opaque tokens in the current AI. This first
// preflight subset admits only private outputs which are never read or escaped;
// consequently an artificial token cannot select a range-proof control edge.
inline bool unread_private_thread_output(ar::CallBase* call) {
  auto* target = dyn_cast< ar::FunctionPointerConstant >(call->called());
  if (!target || !target->function()->is_declaration() ||
      (target->function()->name() != "pthread_create" &&
       target->function()->intrinsic_id() != ar::Intrinsic::PthreadCreate) ||
      !call->has_frontend() || call->num_arguments() != 4 ||
      call->code()->function()->name() != "main") return false;
  auto* source = llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >());
  auto* function = source ? llvm::dyn_cast< llvm::Function >(
      source->getCalledOperand()->stripPointerCasts()) : nullptr;
  if (!function || !function->isDeclaration() || function->isVarArg() ||
      function->getName() != "pthread_create" || function->arg_size() != 4 ||
      source->arg_size() != 4 || !source->getType()->isIntegerTy(32) ||
      !function->getReturnType()->isIntegerTy(32) ||
      source->getCallingConv() != llvm::CallingConv::C ||
      function->getCallingConv() != llvm::CallingConv::C ||
      target->function()->num_parameters() != 4 ||
      target->function()->type()->is_var_arg()) return false;
  auto* returned = dyn_cast< ar::IntegerType >(target->function()->type()->return_type());
  if (!returned || returned->bit_width() != 32 || !returned->is_signed()) return false;
  for (unsigned i = 0; i < 4; ++i)
    if (!function->getFunctionType()->getParamType(i)->isPointerTy() ||
        !source->getArgOperand(i)->getType()->isPointerTy() ||
        !call->argument(i)->type()->is_pointer()) return false;
  auto* entry = dyn_cast< ar::FunctionPointerConstant >(call->argument(2));
  if (!entry || !entry->function()->is_definition() || entry->function()->name() == "main")
    return false;
  if (!source->use_empty()) return false;
  auto* storage = llvm::dyn_cast< llvm::AllocaInst >(
      source->getArgOperand(0)->stripPointerCasts());
  auto* count = storage ? llvm::dyn_cast< llvm::ConstantInt >(storage->getArraySize()) : nullptr;
  const auto abi = pthread_object_layout(call->bundle(), source->getModule());
  auto* native_output = llvm::cast< llvm::PointerType >(
      function->getFunctionType()->getParamType(0));
  auto* actual_output = llvm::cast< llvm::PointerType >(
      source->getArgOperand(0)->getType());
  auto* ar_output = dyn_cast< ar::PointerType >(call->argument(0)->type());
  auto* ar_handle = ar_output ? dyn_cast< ar::IntegerType >(ar_output->pointee()) : nullptr;
  if (!storage || storage->getFunction() != source->getFunction() ||
      !abi || !storage->getAllocatedType()->isIntegerTy(abi.word * 8) ||
      native_output->getAddressSpace() != 0 || actual_output->getAddressSpace() != 0 ||
      native_output->isOpaque() || actual_output->isOpaque() ||
      !native_output->getPointerElementType()->isIntegerTy(abi.word * 8) ||
      !actual_output->getPointerElementType()->isIntegerTy(abi.word * 8) ||
      !ar_handle || ar_handle->bit_width() != abi.word * 8 ||
      !count || !count->isOne()) return false;
  std::vector< llvm::Value* > pending{storage};
  std::set< llvm::Value* > seen;
  while (!pending.empty()) {
    auto* value = pending.back(); pending.pop_back();
    if (!seen.insert(value).second) continue;
    if (seen.size() > 512) return false;
    for (auto* user : value->users()) {
      if (llvm::isa< llvm::BitCastOperator >(user)) pending.push_back(user);
      else if (auto* gep = llvm::dyn_cast< llvm::GEPOperator >(user)) {
        if (gep->getPointerOperand() != value || !gep->hasAllZeroIndices()) return false;
        pending.push_back(user);
      } else if (auto* store = llvm::dyn_cast< llvm::StoreInst >(user)) {
        if (store->getPointerOperand() != value || store->isAtomic() ||
            store->isVolatile() || !store->getValueOperand()->getType()->isIntegerTy(abi.word * 8)) return false;
      } else if (llvm::isa< llvm::DbgInfoIntrinsic >(user)) continue;
      else if (auto* invocation = llvm::dyn_cast< llvm::CallBase >(user)) {
        if (invocation != source || invocation->getArgOperand(0) != value) return false;
        for (unsigned i = 1; i < 4; ++i)
          if (invocation->getArgOperand(i) == value) return false;
      } else return false;
    }
  }
  return true;
}

// A NULL-root constant layout calculation observes no application object address.
inline bool zero_based_layout_projection(const llvm::Value* value,
                                         const llvm::DataLayout& layout,
                                         std::uint64_t* result = nullptr) {
  auto* cast = llvm::dyn_cast< llvm::Operator >(value);
  if (!cast || cast->getOpcode() != llvm::Instruction::PtrToInt ||
      !cast->getType()->isIntegerTy(32) || layout.getPointerSizeInBits(0) != 32)
    return false;
  value = cast->getOperand(0);
  llvm::APInt offset(32, 0);
  for (unsigned depth = 0; depth < 32; ++depth) {
    if (!value->getType()->isPointerTy() ||
        value->getType()->getPointerAddressSpace() != 0) return false;
    if (llvm::isa< llvm::ConstantPointerNull >(value)) {
      if (result) *result = offset.getZExtValue();
      return true;
    }
    if (auto* gep = llvm::dyn_cast< llvm::GEPOperator >(value)) {
      llvm::APInt delta(32, 0);
      if (!gep->accumulateConstantOffset(layout, delta)) return false;
      offset += delta;
      value = gep->getPointerOperand();
    } else if (auto* bitcast = llvm::dyn_cast< llvm::BitCastOperator >(value))
      value = bitcast->getOperand(0);
    else return false;
  }
  return false;
}

inline bool zero_based_layout_projection(ar::UnaryOperation* cast) {
  if ((cast->op() != ar::UnaryOperation::PtrToUI &&
       cast->op() != ar::UnaryOperation::PtrToSI) || !cast->has_frontend() ||
      !cast->result()->type()->is_integer() ||
      ar::cast< ar::IntegerType >(cast->result()->type())->bit_width() != 32)
    return false;
  auto* source = llvm::dyn_cast< llvm::PtrToIntInst >(cast->frontend< llvm::Value >());
  std::uint64_t expected;
  if (!source || !zero_based_layout_projection(source, source->getModule()->getDataLayout(), &expected)) return false;
  auto* value = cast->operand();
  core::ZNumber offset(0), modulus = core::ZNumber(1) << 32;
  for (unsigned depth = 0; depth < 32; ++depth) {
    if (isa< ar::NullConstant >(value)) {
      offset = ((offset % modulus) + modulus) % modulus;
      return offset == core::ZNumber(expected);
    }
    auto* variable = dyn_cast< ar::InternalVariable >(value);
    auto* definition = variable ? unique_def(variable) : nullptr;
    if (auto* copy = dyn_cast_or_null< ar::Assignment >(definition)) value = copy->operand();
    else if (auto* bitcast = dyn_cast_or_null< ar::UnaryOperation >(definition)) {
      if (bitcast->op() != ar::UnaryOperation::Bitcast) return false;
      value = bitcast->operand();
    } else if (auto* shift = dyn_cast_or_null< ar::PointerShift >(definition)) {
      for (auto it = shift->term_begin(); it != shift->term_end(); ++it) {
        auto term = *it;
        auto* constant = dyn_cast< ar::IntegerConstant >(term.second);
        if (!constant) return false;
        offset += term.first.to_z_number() * constant->value().to_z_number();
      }
      value = shift->pointer();
    } else return false;
  }
  return false;
}

// An exit status cannot feed an address, branch, memory effect or callback.
inline bool main_exit_pointer_projection(const llvm::Value* value) {
  auto* projection = llvm::dyn_cast< llvm::PtrToIntInst >(value);
  auto* main = projection ? projection->getFunction() : nullptr;
  if (!main || main->getName() != "main" || !main->use_empty() ||
      !main->arg_empty() || !main->getReturnType()->isIntegerTy(32) ||
      projection->getOperand(0)->getType()->getPointerAddressSpace() != 0 ||
      !projection->getType()->isIntegerTy() ||
      projection->getType()->getIntegerBitWidth() > 64)
    return false;
  std::vector< const llvm::Value* > pending{projection};
  std::set< const llvm::Value* > seen;
  unsigned budget = 2048;
  while (!pending.empty()) {
    auto* current = pending.back();
    pending.pop_back();
    if (!seen.insert(current).second) continue;
    for (auto* user : current->users()) {
      if (budget-- == 0) return false;
      auto* ret = llvm::dyn_cast< llvm::ReturnInst >(user);
      if (ret && ret->getFunction() == main && ret->getReturnValue() == current)
        continue;
      auto* cast = llvm::dyn_cast< llvm::CastInst >(user);
      if (!cast || cast->getFunction() != main ||
          !cast->getType()->isIntegerTy() ||
          cast->getType()->getIntegerBitWidth() > 64 ||
          (cast->getOpcode() != llvm::Instruction::SExt &&
           cast->getOpcode() != llvm::Instruction::ZExt &&
           cast->getOpcode() != llvm::Instruction::Trunc &&
           cast->getOpcode() != llvm::Instruction::BitCast))
        return false;
      pending.push_back(cast);
    }
  }
  return true;
}

inline bool mutex_effect_pointer_representations_closed(const llvm::Module& module) {
  std::vector< const llvm::Constant* > pending;
  unsigned budget = 16384;
  for (const auto& global : module.globals())
    if (global.hasInitializer()) pending.push_back(global.getInitializer());
  for (const auto& function : module) for (const auto& block : function)
    for (const auto& instruction : block) {
      if (budget-- == 0 || (llvm::isa< llvm::PtrToIntInst >(instruction) &&
          !zero_based_layout_projection(&instruction, module.getDataLayout()) &&
          !main_exit_pointer_projection(&instruction)) ||
          llvm::isa< llvm::IntToPtrInst >(instruction)) return false;
      for (const auto& operand : instruction.operands())
        if (auto* constant = llvm::dyn_cast< llvm::Constant >(operand.get()))
          pending.push_back(constant);
    }
  std::set< const llvm::Constant* > seen;
  while (!pending.empty()) {
    auto* value = pending.back(); pending.pop_back();
    if (!seen.insert(value).second) continue;
    if (budget-- == 0) return false;
    if (auto* expression = llvm::dyn_cast< llvm::ConstantExpr >(value))
      if ((expression->getOpcode() == llvm::Instruction::PtrToInt &&
           !zero_based_layout_projection(expression, module.getDataLayout())) ||
          expression->getOpcode() == llvm::Instruction::IntToPtr) return false;
    if (llvm::isa< llvm::GlobalValue >(value)) continue;
    for (const auto& operand : value->operands())
      if (auto* constant = llvm::dyn_cast< llvm::Constant >(operand.get()))
        pending.push_back(constant);
  }
  return true;
}

template < typename E >
bool subarray_address_within(E& eng, ar::Value* operand,
                             const GlobalArraySubobject& object,
                             const core::ZNumber& width) {
  const auto& literal = eng.ctx().lit_factory->get_scalar(operand);
  if (!literal.is_pointer_var() || eng.data().is_normal_flow_bottom()) return false;
  const auto address = eng.data().normal().pointer_to_pointer(literal.var());
  const auto& points = address.points_to();
  const auto& offset = address.offset();
  return eng.data().normal().nullity_is_non_null(literal.var()) &&
      points.is_set() && points.size() == 1 &&
      *points.begin() == eng.ctx().mem_factory->get_global(object.global) &&
      !offset.is_bottom() && !offset.is_top() &&
      offset.lb().to_z_number() >= object.begin &&
      offset.ub().to_z_number() + width <= object.begin + object.size;
}

template < typename E >
void observe_subarray_bounds(E& eng, ar::Statement* statement, ar::Value* operand,
                             ar::Type* type) {
  auto& ctx = eng.ctx();
  auto query = ctx.subarray_bounds_queries.find(statement);
  if (query == ctx.subarray_bounds_queries.end() || eng.data().is_normal_flow_bottom()) return;
  bool valid = subarray_address_within(eng, operand, query->second,
      ctx.bundle->data_layout().store_size_in_bytes(type));
  ctx.subarray_ranges.observe(statement, valid);
}

template < typename E >
void observe_mutex_array_effect(E& eng, ar::CallBase* call) {
  auto& ctx = eng.ctx();
  auto query = ctx.mutex_effect_queries.find(call);
  if (query == ctx.mutex_effect_queries.end() ||
      eng.data().is_normal_flow_bottom()) return;
  const auto& literal = ctx.lit_factory->get_scalar(call->argument(0));
  const auto width = mutex_array_leaf_width(query->second);
  bool valid = false;
  if (width && literal.is_pointer_var()) {
    auto address = eng.data().normal().pointer_to_pointer(literal.var());
    auto& points = address.points_to();
    const auto& offset = address.offset();
    valid = eng.data().normal().nullity_is_non_null(literal.var()) &&
        points.is_set() && points.size() == 1 &&
        *points.begin() == ctx.mem_factory->get_global(query->second.global) &&
        !offset.is_bottom() && !offset.is_top() && offset.lb().to_z_number() >= query->second.begin &&
        offset.ub().to_z_number() + width <=
            query->second.begin + query->second.size;
  }
  ctx.mutex_effect_ranges.observe(call, valid);
}

} // namespace analyzer
} // namespace ikos
