#pragma once

#include "unroll.hpp"
#include <llvm/ADT/Triple.h>
#include <llvm/IR/Module.h>

namespace ikos { namespace analyzer { namespace bmc {

// Reuse AR initializers and the abstract interpreter's target layout. Resolve an
// exactly typed integer field; undef, padding and reinterpretation are unsupported.
// No abstract memory value is promoted to a concrete value.
inline bool initial_integer(ar::GlobalVariable* global, core::ZNumber offset,
                            unsigned bits, core::ZNumber& value) {
  if (!global->is_definition() || offset < 0) return false;
  ar::Value* constant = nullptr;
  for (ar::BasicBlock* bb : *global->initializer()) {
    for (ar::Statement* stmt : *bb) {
      if (auto* store = core::dyn_cast< ar::Store >(stmt)) {
        if (constant != nullptr || store->pointer() != global) return false;
        constant = store->value();
      }
    }
  }
  if (constant == nullptr) return false;
  auto* type = constant->type();
  const auto& layout = global->bundle()->data_layout();
  for (unsigned depth = 0; depth < 32; ++depth) {
    if (core::isa< ar::UndefinedConstant >(constant)) return false;
    bool zero = core::isa< ar::AggregateZeroConstant >(constant);
    if (auto* integer = core::dyn_cast< ar::IntegerType >(type)) {
      if (offset != 0 || integer->bit_width() != bits) return false;
      if (zero) {value = 0; return true;}
      auto* c = core::dyn_cast< ar::IntegerConstant >(constant);
      if (c == nullptr) return false;
      value = c->value().to_z_number();
      return true;
    }
    if (auto* structure = core::dyn_cast< ar::StructType >(type)) {
      bool found = false;
      if (zero) {
        for (const auto& field : structure->layout()) {
          if (field.type->is_opaque()) return false;
          auto width = layout.alloc_size_in_bytes(field.type);
          if (offset >= field.offset && offset < field.offset + width) {
            offset -= field.offset; type = field.type; found = true; break;
          }
        }
      } else if (auto* c = core::dyn_cast< ar::StructConstant >(constant)) {
        for (const auto& field : c->values()) {
          if (field.value->type()->is_opaque()) return false;
          auto width = layout.alloc_size_in_bytes(field.value->type());
          if (offset >= field.offset && offset < field.offset + width) {
            offset -= field.offset; constant = field.value; type = constant->type(); found = true; break;
          }
        }
      }
      if (!found) return false;
    } else if (auto* array = core::dyn_cast< ar::ArrayType >(type)) {
      if (array->element_type()->is_opaque()) return false;
      auto stride = layout.alloc_size_in_bytes(array->element_type());
      if (stride == 0 || offset / stride >= array->num_elements()) return false;
      if (!zero) {
        auto* c = core::dyn_cast< ar::ArrayConstant >(constant);
        if (c == nullptr) return false;
        constant = c->values()[(offset / stride).to< std::size_t >()];
      }
      offset %= stride;
      type = array->element_type();
    } else {
      return false;
    }
  }
  return false;
}

// Recognize the glibc x86 default initializer, not arbitrary zero storage or
// another platform's condition ABI. Dynamic init remains a separate proof.
inline bool static_condition(const BmcEvent& address) {
  if (address.base == 0 || address.const_off != 0 || address.var_id != 0 || address.width != 48) return false;
  auto* global = reinterpret_cast< ar::GlobalVariable* >(address.base);
  if (!global->has_frontend()) return false;
  auto* source = global->frontend< llvm::GlobalVariable >();
  const llvm::Triple target(source->getParent()->getTargetTriple());
  if (!target.isOSLinux() || target.getEnvironment() != llvm::Triple::GNU ||
      (target.getArch() != llvm::Triple::x86 && target.getArch() != llvm::Triple::x86_64) ||
      !source->hasInitializer() || !source->getInitializer()->isNullValue()) return false;
  auto* outer = llvm::dyn_cast< llvm::StructType >(source->getValueType());
  if (outer == nullptr || !outer->hasName() || outer->getName() != "union.pthread_cond_t" ||
      outer->isOpaque() || outer->getNumElements() != 1) return false;
  auto* control = llvm::dyn_cast< llvm::StructType >(outer->getElementType(0));
  if (control == nullptr || control->isOpaque() || control->getNumElements() != 7) return false;
  const auto& layout = source->getParent()->getDataLayout();
  const auto* fields = layout.getStructLayout(control);
  const unsigned offsets[] = {0, 8, 16, 24, 32, 36, 40};
  const unsigned sizes[] = {8, 8, 8, 8, 4, 4, 8};
  if (layout.getTypeAllocSize(outer) != 48 || fields->getSizeInBytes() != 48) return false;
  for (unsigned i = 0; i < 7; ++i)
    if (fields->getElementOffset(i) != offsets[i] ||
        layout.getTypeAllocSize(control->getElementType(i)) != sizes[i]) return false;
  core::ZNumber value;
  for (unsigned offset = 0; offset < 48; offset += offset < 16 ? 8 : 4)
    if (!initial_integer(global, core::ZNumber(offset), offset < 16 ? 64 : 32, value) || value != 0) return false;
  return true;
}

} } }
