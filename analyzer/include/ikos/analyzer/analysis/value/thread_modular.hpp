/*******************************************************************************
 *
 * \file
 * \brief Thread-Modular value analysis driver (plugin-style)
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
 * THE UNITED STATES GOVERNMENT, ITS CONTRACTORS AND SUBCONTRACTORS, AS WELL AS
 * ANY PRIOR RECIPIENT.  IF RECIPIENT'S USE OF THE SUBJECT SOFTWARE RESULTS IN
 * ANY LIABILITIES, DEMANDS, DAMAGES, EXPENSES OR LOSSES ARISING FROM SUCH USE,
 * INCLUDING ANY DAMAGES FROM PRODUCTS BASED ON, OR RESULTING FROM, RECIPIENT'S
 * USE OF THE SUBJECT SOFTWARE, RECIPIENT SHALL INDEMNIFY AND HOLD HARMLESS THE
 * UNITED STATES GOVERNMENT, ITS CONTRACTORS AND SUBCONTRACTORS, AS WELL AS ANY
 * PRIOR RECIPIENT, TO THE EXTENT PERMITTED BY LAW.  RECIPIENT'S SOLE REMEDY
 * FOR ANY SUCH MATTER SHALL BE THE IMMEDIATE, UNILATERAL TERMINATION OF THIS
 * AGREEMENT.
 *
 ******************************************************************************/

#pragma once

#include <ikos/analyzer/analysis/context.hpp>

namespace ikos {
namespace analyzer {
namespace value {

/// \brief Thread-Modular value analysis driver
///
/// This class implements the Goblint-style thread-modular analysis driver
/// as a self-contained plugin. It owns:
///   1. A global fixpoint worklist that iterates over the program entry
///      points and any thread functions discovered during analysis.
///   2. The "blackboard" round: between worklist iterations it consults the
///      `ConcurrentGlobalEnv::is_dirty()` flag to decide whether another
///      round is required.
///   3. The post-loop checker pass that runs the actual property checks
///      (`run_checks`) once the blackboard has stabilized.
///
/// The driver is intentionally decoupled from the sequential interprocedural
/// driver. The original implementation embedded all of this logic into the
/// sequential driver's `run()` method, which made it impossible to run
/// the rest of IKOS's analyses (buffer overflow, division-by-zero, etc.)
/// together with the concurrency-aware value analysis. By extracting the
/// driver into its own class we enable a clean plugin wiring: a user can
/// request the concurrency analysis and the right driver runs.
class ThreadModularAnalysis {
private:
  /// \brief Analysis context
  Context& _ctx;

  /// \brief If true, after each worklist iteration print the per-function
  /// invariants and a snapshot of the global blackboard state.
  bool _emit_concurrency_invariants;

public:
  /// \brief Constructor
  ///
  /// \param ctx The analysis context.
  /// \param emit_concurrency_invariants Whether to print per-iteration
  ///        invariants and a `ConcurrentGlobalEnv::dump()` snapshot.
  explicit ThreadModularAnalysis(Context& ctx,
                                 bool emit_concurrency_invariants = false);

  /// \brief No copy constructor
  ThreadModularAnalysis(const ThreadModularAnalysis&) = delete;

  /// \brief No move constructor
  ThreadModularAnalysis(ThreadModularAnalysis&&) = delete;

  /// \brief No copy assignment operator
  ThreadModularAnalysis& operator=(const ThreadModularAnalysis&) = delete;

  /// \brief No move assignment operator
  ThreadModularAnalysis& operator=(ThreadModularAnalysis&&) = delete;

  /// \brief Destructor
  ~ThreadModularAnalysis();

  /// \brief Run the thread-modular analysis.
  ///
  /// Performs three phases:
  ///   1. Global variable initialization (static + dynamic constructors).
  ///   2. Worklist fixpoint iteration driven by `ConcurrentGlobalEnv`.
  ///   3. Post-loop checks + global destructors.
  void run();

  /// \brief Convenience hook: print the contents of the global blackboard
  /// (`ConcurrentGlobalEnv::dump`). Called after the worklist converges if
  /// `emit_concurrency_invariants` is true.
  void dump_global_blackboard() const;
};

} // end namespace value
} // end namespace analyzer
} // end namespace ikos