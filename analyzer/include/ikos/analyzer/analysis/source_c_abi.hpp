// Copyright and license: see source_call_binding.hpp.
#pragma once
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>

namespace ikos {
namespace analyzer {

// Existing conservative value-attribute policy, factored without widening it.
// None of these attributes is consumed as a success, access or effect premise.
inline bool ordinary_source_value_attribute(llvm::Attribute attribute) {
  if (attribute.isStringAttribute()) return false;
  switch (attribute.getKindAsEnum()) {
    case llvm::Attribute::NoUndef: case llvm::Attribute::NonNull:
    case llvm::Attribute::NoCapture: case llvm::Attribute::ReadOnly:
    case llvm::Attribute::WriteOnly: case llvm::Attribute::ReadNone:
    case llvm::Attribute::Dereferenceable:
    case llvm::Attribute::DereferenceableOrNull:
    case llvm::Attribute::Alignment: case llvm::Attribute::NoAlias:
      return true;
    default: return false;
  }
}

// Keep this existing public helper's strict meaning for its current callers.
inline bool ordinary_source_attributes(llvm::AttributeSet attributes) {
  for (auto attribute : attributes)
    if (!ordinary_source_value_attribute(attribute)) return false;
  return true;
}

inline unsigned source_extension_mask(llvm::AttributeSet attributes) {
  return (attributes.hasAttribute(llvm::Attribute::SExt) ? 1u : 0u) |
         (attributes.hasAttribute(llvm::Attribute::ZExt) ? 2u : 0u);
}

// This describes the supported GNU scalar C calling representation only.
// API names, effects, exact known prototypes and target ABI remain separate.
// Narrow source signedness is absent from LLVM's integer type, so neither
// i8 nor i16 alone selects signext versus zeroext.
inline bool ordinary_source_abi_value(llvm::Type* type,
                                      llvm::AttributeSet attributes) {
  const unsigned extension = source_extension_mask(attributes);
  if (extension == 3) return false;
  if (extension) {
    auto* integer = llvm::dyn_cast< llvm::IntegerType >(type);
    if (!integer) return false;
    const unsigned bits = integer->getBitWidth();
    if (bits != 1 && bits != 8 && bits != 16) return false;
    // The supported C bool convention uses zeroext, not signed one-bit ABI.
    if (bits == 1 && extension != 2) return false;
  }
  for (auto attribute : attributes) {
    if (attribute.isEnumAttribute() &&
        (attribute.getKindAsEnum() == llvm::Attribute::SExt ||
         attribute.getKindAsEnum() == llvm::Attribute::ZExt)) continue;
    if (!ordinary_source_value_attribute(attribute)) return false;
  }
  return true;
}

// Check the actual operands even if LLVM leaves the callee indirect. Target
// resolution cannot turn inreg/byval/etc. into an ordinary source invocation.
inline bool ordinary_source_call_site_abi_attributes(llvm::CallBase* call) {
  if (!call ||
      !ordinary_source_abi_value(call->getType(),
                                 call->getAttributes().getRetAttrs()))
    return false;
  for (unsigned n = 0; n < call->arg_size(); ++n) {
    const auto attributes = call->getAttributes().getParamAttrs(n);
    if (!ordinary_source_abi_value(call->getArgOperand(n)->getType(), attributes))
      return false;
    if (n >= call->getFunctionType()->getNumParams() &&
        source_extension_mask(attributes))
      return false;
  }
  return true;
}

// Attribute-only gate: callers still establish C CC, actual callee identity,
// source/AR correspondence, exact ABI/prototype and the API's effect contract.
// Raw AttributeList access is intentional: CallBase hasRetAttr/paramHasAttr
// can fall back to callee attrs and would hide a one-sided extension.
inline bool ordinary_source_call_abi_attributes(llvm::CallBase* call,
                                                llvm::Function* function) {
  if (!ordinary_source_call_site_abi_attributes(call) || !function ||
      call->arg_size() < function->arg_size() ||
      call->getType() != function->getReturnType()) return false;
  const auto actual = call->getAttributes();
  const auto declared = function->getAttributes();
  const auto return_actual = actual.getRetAttrs();
  const auto return_declared = declared.getRetAttrs();
  if (!ordinary_source_abi_value(call->getType(), return_actual) ||
      !ordinary_source_abi_value(function->getReturnType(), return_declared) ||
      source_extension_mask(return_actual) !=
          source_extension_mask(return_declared)) return false;
  for (unsigned n = 0; n < call->arg_size(); ++n) {
    auto* type = call->getArgOperand(n)->getType();
    const auto attributes = actual.getParamAttrs(n);
    if (!ordinary_source_abi_value(type, attributes)) return false;
    if (n < function->arg_size()) {
      auto* formal = function->getFunctionType()->getParamType(n);
      const auto formal_attributes = declared.getParamAttrs(n);
      if (type != formal ||
          !ordinary_source_abi_value(formal, formal_attributes) ||
          source_extension_mask(attributes) !=
              source_extension_mask(formal_attributes)) return false;
    } else {
      // Specialized true-vararg binders retain actual promoted types/formats.
      // Extension-annotated varargs do not describe C's default promotions.
      if (!function->isVarArg() || source_extension_mask(attributes))
        return false;
    }
  }
  return true;
}

// For callbacks entered by the external runtime rather than an inventoried
// LLVM call site. Caller must independently check the exact native API shape.
inline bool ordinary_source_function_abi_attributes(llvm::Function* function) {
  if (!function ||
      !ordinary_source_abi_value(function->getReturnType(),
                                 function->getAttributes().getRetAttrs()))
    return false;
  for (unsigned n = 0; n < function->arg_size(); ++n)
    if (!ordinary_source_abi_value(function->getFunctionType()->getParamType(n),
                                   function->getAttributes().getParamAttrs(n)))
      return false;
  return true;
}

} // namespace analyzer
} // namespace ikos
