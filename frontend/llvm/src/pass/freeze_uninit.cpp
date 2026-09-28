/*******************************************************************************
 *
 * \file
 * \brief Freeze uninitialized integer allocas into a single nondet value
 *
 * Author: IKOS User
 *
 * Copyright (c) 2011-2019 United States Government as represented by the
 * Administrator of the National Aeronautics and Space Administration.
 * All Rights Reserved.
 *
 ******************************************************************************/

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <ikos/frontend/llvm/pass.hpp>

// NOLINTNEXTLINE(google-build-using-namespace)
using namespace llvm;

namespace {

/// \brief Initialize every i32/i64 alloca with a single nondet value.
///
/// mem2reg (the first pass of `-opt=basic`) folds a `load` of a write-before-read
/// alloca into `undef`. An `undef` is then treated by the analyzer as "this path
/// is dead" (BOTTOM), which for RACE DETECTION is an UNDER-approximation: the
/// uninitialized value could be ANY value, and the program still races. Worse,
/// two loads of the SAME uninitialized alloca become two INDEPENDENT `undef`s,
/// losing the "same cell" correlation that path-sensitive programs rely on
/// (04-mutex_52-confid_rc.c FN, 10-synch_19-join_path_nr.c FP).
///
/// Freezing the alloca with a single `store nondet` BEFORE mem2reg gives:
///   1. no `undef` is produced (the alloca is now written before read);
///   2. both loads read the SAME nondet value (correlation preserved).
/// A properly-initialized alloca already has a dominating store, so its freeze
/// store is dead and harmless.
struct FreezeUninitPass final : public FunctionPass {
  static char ID; // Pass identification

  FreezeUninitPass() : FunctionPass(ID) {}

  /// \brief Return the nondet intrinsic name for an integer type, or null.
  static const char* nondet_name(Type* elem) {
    if (!elem->isIntegerTy()) {
      return nullptr;
    }
    switch (elem->getIntegerBitWidth()) {
      case 32:
        return "__ikos_nondet_int";
      case 64:
        return "__ikos_nondet_int64";
      default:
        return nullptr; // other widths: skip for now
    }
  }

  bool runOnFunction(Function& F) override {
    Module* M = F.getParent();
    bool changed = false;

    std::vector< AllocaInst* > allocas;
    for (BasicBlock& BB : F) {
      for (Instruction& I : BB) {
        if (auto* AI = dyn_cast< AllocaInst >(&I)) {
          allocas.push_back(AI);
        }
      }
    }

    for (AllocaInst* AI : allocas) {
      Type* elem = AI->getAllocatedType();
      const char* name = nondet_name(elem);
      if (name == nullptr) {
        continue;
      }
      FunctionCallee nd =
          M->getOrInsertFunction(name, FunctionType::get(elem, false));
      Instruction* insert_pt = AI->getNextNode();
      auto* call = CallInst::Create(nd, {}, "", insert_pt);
      new StoreInst(call, AI, insert_pt);
      changed = true;
    }
    return changed;
  }

}; // end struct FreezeUninitPass

} // end anonymous namespace

char FreezeUninitPass::ID = 0;

INITIALIZE_PASS(FreezeUninitPass,
                "freeze-uninit",
                "Freeze uninitialized integer allocas into a nondet value",
                false,
                false);

FunctionPass* ikos::frontend::pass::create_freeze_uninit_pass() {
  return new FreezeUninitPass();
}
