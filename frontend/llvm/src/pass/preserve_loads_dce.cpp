/*******************************************************************************
 *
 * \file
 * \brief Dead-code elimination that PRESERVES loads
 *
 * Author: IKOS User
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
 * RECIPIENT'S USE OF THE SUBJECT SOFTWARE. RECIPIENT SHALL INDEMNIFY AND HOLD
 * HARMLESS THE UNITED STATES GOVERNMENT, ITS CONTRACTORS AND SUBCONTRACTORS,
 * AS WELL AS ANY PRIOR RECIPIENT, TO THE EXTENT PERMITTED BY LAW.
 * RECIPIENT'S SOLE REMEDY FOR ANY SUCH MATTER SHALL BE THE IMMEDIATE,
 * UNILATERAL TERMINATION OF THIS AGREEMENT.
 *
 ******************************************************************************/

#include <llvm/IR/Instructions.h>

#include <ikos/frontend/llvm/pass.hpp>

// NOLINTNEXTLINE(google-build-using-namespace)
using namespace llvm;

namespace {

/// \brief Dead-code elimination that PRESERVES LoadInst.
///
/// LLVM's `-dce` removes a non-volatile, unused load because a load is
/// "trivially dead" (no observable side effect). For RACE DETECTION the load
/// IS observable — it is a potential data race with a concurrent write. A
/// benchmark whose racy access is a DEAD read (e.g. `int i = *p;` with `i`
/// unused, per-thread-array-init-race) would otherwise lose its race access
/// before the importer ever sees it. This pass removes the same dead code as
/// `-dce` EXCEPT loads, so dead racy reads survive into the AR and the checker
/// pairs them with the concurrent write. Stores already have side effects and
/// were never removed by `-dce`.
struct PreserveLoadsDCEPass final : public FunctionPass {
  static char ID; // Pass identification

  PreserveLoadsDCEPass() : FunctionPass(ID) {}

  static bool isTriviallyDead(const Instruction& I) {
    if (!I.use_empty()) {
      return false;
    }
    if (I.mayHaveSideEffects()) {
      return false;
    }
    if (I.isTerminator() || I.isEHPad()) {
      return false;
    }
    return true;
  }

  bool runOnFunction(Function& F) override {
    bool changed = false;
    bool local_change = true;
    while (local_change) {
      local_change = false;
      for (BasicBlock& BB : F) {
        for (auto it = BB.begin(); it != BB.end();) {
          Instruction* inst = &*it;
          ++it; // advance before a potential erase
          if (isa< LoadInst >(inst)) {
            continue; // preserve loads: they are potential race accesses
          }
          if (isTriviallyDead(*inst)) {
            inst->eraseFromParent();
            changed = true;
            local_change = true;
          }
        }
      }
    }
    return changed;
  }

}; // end struct PreserveLoadsDCEPass

} // end anonymous namespace

char PreserveLoadsDCEPass::ID = 0;

INITIALIZE_PASS(PreserveLoadsDCEPass,
                "preserve-loads-dce",
                "Dead code elimination that preserves loads",
                false,
                false);

FunctionPass* ikos::frontend::pass::create_preserve_loads_dce_pass() {
  return new PreserveLoadsDCEPass();
}
