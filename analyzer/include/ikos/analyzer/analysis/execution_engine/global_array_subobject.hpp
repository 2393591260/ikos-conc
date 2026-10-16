// Copyright and license: see numerical.hpp.
#pragma once

#include <functional>
#include <set>
#include <vector>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <ikos/ar/semantic/bundle.hpp>
#include <ikos/ar/semantic/statement.hpp>
#include <ikos/analyzer/support/cast.hpp>

namespace ikos {
namespace analyzer {

// An array is identified by its allocation, typed field path and byte range.
// It never becomes a separate memory allocation or a source of uniqueness.
struct GlobalArraySubobject {
  ar::GlobalVariable* global = nullptr;
  ar::ArrayType* type = nullptr;
  llvm::ArrayType* source_type = nullptr;
  std::vector< unsigned > fields;
  core::ZNumber begin{0}, size{0};
  explicit operator bool() const { return global != nullptr; }
  bool nested() const { return !fields.empty(); }
  bool operator==(const GlobalArraySubobject& other) const {
    return global == other.global && type == other.type && source_type == other.source_type &&
           fields == other.fields && begin == other.begin && size == other.size;
  }
};

inline std::vector< GlobalArraySubobject > global_array_subobjects(ar::GlobalVariable* global) {
  std::vector< GlobalArraySubobject > result;
  if (!global || !global->is_definition() || global->is_thread_local() ||
      !global->has_frontend()) return result;
  auto* source = global->frontend< llvm::GlobalVariable >();
  if (source->getAddressSpace() != 0 || source->isExternallyInitialized()) return result;
  const auto& layout = source->getParent()->getDataLayout();
  const auto& ar_layout = global->bundle()->data_layout();
  unsigned budget = 128;
  std::function< void(ar::Type*, llvm::Type*, core::ZNumber, std::vector< unsigned >) > visit;
  visit = [&](ar::Type* type, llvm::Type* original, core::ZNumber begin,
              std::vector< unsigned > fields) {
    if (budget == 0 || fields.size() > 8) return;
    --budget;
    if (!original->isSized() ||
        ar_layout.alloc_size_in_bytes(type) != core::ZNumber(layout.getTypeAllocSize(original).getFixedValue())) return;
    if (auto* array = dyn_cast< ar::ArrayType >(type)) {
      auto* actual = llvm::dyn_cast< llvm::ArrayType >(original);
      if (!actual || array->num_elements() <= 0 ||
          array->num_elements() != core::ZNumber(actual->getNumElements()) ||
          ar_layout.alloc_size_in_bytes(array->element_type()) !=
              core::ZNumber(layout.getTypeAllocSize(actual->getElementType()).getFixedValue())) return;
      result.push_back({global, array, actual, fields, begin, ar_layout.alloc_size_in_bytes(type)});
      return;
    }
    auto* structure = dyn_cast< ar::StructType >(type);
    auto* actual = llvm::dyn_cast< llvm::StructType >(original);
    if (!structure || !actual || actual->isOpaque() || actual->isPacked() ||
        (actual->hasName() && actual->getName().startswith("union.")) ||
        structure->layout().size() != actual->getNumElements()) return;
    const auto* offsets = layout.getStructLayout(actual);
    for (unsigned i = 0; i < actual->getNumElements(); ++i) {
      const auto& field = structure->layout()[i];
      if (field.offset != core::ZNumber(offsets->getElementOffset(i))) return;
    }
    for (unsigned i = 0; i < actual->getNumElements(); ++i) {
      auto path = fields; path.push_back(i);
      visit(structure->layout()[i].type, actual->getElementType(i),
            begin + structure->layout()[i].offset, std::move(path));
    }
  };
  visit(global->type()->pointee(), source->getValueType(), core::ZNumber(0), {});
  if (budget == 0) result.clear();
  return result;
}

// Match a complete typed leaf address. The index still needs an independent
// range proof for a nested array: LLVM inbounds only bounds the whole global.
inline bool global_array_element(llvm::Value* value,
                                  const GlobalArraySubobject& array,
                                  llvm::Value** index = nullptr) {
  if (!array) return false;
  auto* root = array.global->frontend< llvm::GlobalVariable >();
  llvm::Type* current = root->getValueType();
  std::vector< unsigned > fields;
  llvm::Value* selected = nullptr;
  unsigned remaining = 32;
  std::function< bool(llvm::Value*) > walk = [&](llvm::Value* address) {
    if (remaining-- == 0) return false;
    if (address == root) return true;
    if (auto* cast = llvm::dyn_cast< llvm::BitCastOperator >(address))
      return walk(cast->getOperand(0));
    auto* gep = llvm::dyn_cast< llvm::GEPOperator >(address);
    if (!gep || !gep->isInBounds() || !walk(gep->getPointerOperand()) ||
        gep->getSourceElementType() != current || gep->getNumIndices() == 0) return false;
    auto it = gep->idx_begin();
    auto* zero = llvm::dyn_cast< llvm::ConstantInt >(it->get());
    if (!zero || !zero->isZero()) return false;
    ++it;
    for (; it != gep->idx_end(); ++it) {
      if (selected) return false;
      if (auto* structure = llvm::dyn_cast< llvm::StructType >(current)) {
        auto* field = llvm::dyn_cast< llvm::ConstantInt >(it->get());
        if (!field || field->getValue().getActiveBits() > 32 ||
            field->getZExtValue() >= structure->getNumElements()) return false;
        unsigned n = field->getZExtValue(); fields.push_back(n);
        current = structure->getElementType(n);
      } else if (current == array.source_type) {
        selected = it->get(); current = array.source_type->getElementType();
      } else return false;
    }
    return true;
  };
  if (!walk(value) || !selected || fields != array.fields ||
      current != array.source_type->getElementType()) return false;
  if (index) *index = selected;
  return true;
}

inline bool data_pointer_array(const GlobalArraySubobject& array) {
  auto* leaf = array ? llvm::dyn_cast< llvm::PointerType >(array.source_type->getElementType()) : nullptr;
  return leaf && array.type->element_type()->is_pointer() && leaf->getAddressSpace() == 0 &&
         !leaf->getPointerElementType()->isFunctionTy();
}

inline bool pointer_array_access(ar::Statement* statement,
                                  const GlobalArraySubobject& array,
                                  llvm::Value** index = nullptr) {
  if (!data_pointer_array(array) || !statement->has_frontend()) return false;
  auto* source = statement->frontend< llvm::Value >();
  llvm::Value* pointer = nullptr;
  llvm::Type* leaf = nullptr;
  if (auto* load = llvm::dyn_cast< llvm::LoadInst >(source)) {
    if (load->isVolatile() || load->isAtomic()) return false;
    pointer = load->getPointerOperand(); leaf = load->getType();
  } else if (auto* store = llvm::dyn_cast< llvm::StoreInst >(source)) {
    if (store->isVolatile() || store->isAtomic()) return false;
    pointer = store->getPointerOperand(); leaf = store->getValueOperand()->getType();
  } else return false;
  return leaf == array.source_type->getElementType() &&
         global_array_element(pointer, array, index);
}

inline bool closed_global_array_uses(ar::GlobalVariable* global,
                                     const std::function< bool(llvm::User*) >& terminal) {
  if (!global->has_frontend()) return false;
  std::vector< llvm::Value* > pending{global->frontend< llvm::GlobalVariable >()};
  std::set< llvm::Value* > seen;
  while (!pending.empty()) {
    auto* value = pending.back(); pending.pop_back();
    if (!seen.insert(value).second) continue;
    if (seen.size() > 8192) return false;
    for (auto* user : value->users()) {
      if (auto* gep = llvm::dyn_cast< llvm::GEPOperator >(user)) {
        if (gep->getPointerOperand() != value) return false;
        pending.push_back(user);
      } else if (llvm::isa< llvm::BitCastOperator >(user)) pending.push_back(user);
      else {
        // Destination shape alone must never admit an address escaping as data.
        if (auto* load = llvm::dyn_cast< llvm::LoadInst >(user)) {
          if (load->getPointerOperand() != value) return false;
        } else if (auto* store = llvm::dyn_cast< llvm::StoreInst >(user)) {
          if (store->getPointerOperand() != value || store->getValueOperand() == value) return false;
        } else if (auto* call = llvm::dyn_cast< llvm::CallBase >(user)) {
          if (call->arg_size() == 0 || call->getArgOperand(0) != value ||
              call->getCalledOperand() == value) return false;
          for (unsigned i = 1; i < call->arg_size(); ++i)
            if (call->getArgOperand(i) == value) return false;
        } else return false;
        if (!terminal(user)) return false;
      }
    }
  }
  return true;
}

inline bool pure_local_array_index(llvm::Value* index) {
  std::vector< llvm::Value* > pending{index};
  std::set< llvm::Value* > visited;
  while (!pending.empty()) {
    auto* value = pending.back(); pending.pop_back();
    if (!visited.insert(value).second) continue;
    if (visited.size() > 1024 || !value->getType()->isIntegerTy()) return false;
    if (llvm::isa< llvm::ConstantInt >(value)) continue;
    if (auto* input = llvm::dyn_cast< llvm::CallBase >(value)) {
      auto* function = llvm::dyn_cast< llvm::Function >(
          input->getCalledOperand()->stripPointerCasts());
      if (!function || !function->isDeclaration() ||
          function->getName() != "__VERIFIER_nondet_int" ||
          function->arg_size() != 0 || input->arg_size() != 0 ||
          !function->getReturnType()->isIntegerTy(32) || !input->getType()->isIntegerTy(32) ||
          input->getCallingConv() != llvm::CallingConv::C ||
          function->getCallingConv() != llvm::CallingConv::C) return false;
      continue;
    }
    auto* instruction = llvm::dyn_cast< llvm::Instruction >(value);
    if (!instruction || !(llvm::isa< llvm::PHINode >(instruction) ||
        llvm::isa< llvm::BinaryOperator >(instruction) ||
        llvm::isa< llvm::ICmpInst >(instruction) || llvm::isa< llvm::SelectInst >(instruction) ||
        (llvm::isa< llvm::CastInst >(instruction) &&
         instruction->getOperand(0)->getType()->isIntegerTy()))) return false;
    for (auto& operand : instruction->operands()) pending.push_back(operand.get());
  }
  return true;
}

} // namespace analyzer
} // namespace ikos
