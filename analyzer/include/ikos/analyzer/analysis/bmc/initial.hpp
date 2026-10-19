#pragma once

#include "unroll.hpp"
#include <llvm/ADT/Triple.h>
#include <llvm/IR/Module.h>

namespace ikos { namespace analyzer { namespace bmc {

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
