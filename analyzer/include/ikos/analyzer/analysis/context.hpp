/*******************************************************************************
 *
 * \file
 * \brief Global analysis context
 *
 * Author: Maxime Arthaud
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
 * RECIPIENT'S USE OF THE SUBJECT SOFTWARE, RECIPIENT SHALL INDEMNIFY AND HOLD
 * HARMLESS THE UNITED STATES GOVERNMENT, ITS CONTRACTORS AND SUBCONTRACTORS,
 * AS WELL AS ANY PRIOR RECIPIENT, TO THE EXTENT PERMITTED BY LAW.
 * RECIPIENT'S SOLE REMEDY FOR ANY SUCH MATTER SHALL BE THE IMMEDIATE,
 * UNILATERAL TERMINATION OF THIS AGREEMENT.
 *
 ******************************************************************************/

#pragma once

#include <map>
#include <memory>
#include <tuple>
#include <set>

#include <boost/filesystem.hpp>

#include <ikos/ar/semantic/bundle.hpp>

#include <ikos/analyzer/analysis/option.hpp>
#include <ikos/analyzer/analysis/all_access_facts.hpp>
#include <ikos/analyzer/checker/race_rules.hpp>
#include <ikos/analyzer/analysis/execution_engine/global_array_subobject.hpp>
#include <ikos/core/domain/concurrent_global_env.hpp>

namespace ikos {
namespace analyzer {

namespace value { class ThreadModularAnalysis; }

// forward declarations
class OutputDatabase;
class MemoryFactory;
class VariableFactory;
class LiteralFactory;
class CallContextFactory;
class LivenessAnalysis;
class FunctionPointerAnalysis;
class PointerAnalysis;
class FixpointParameters;
class RuntimeStdioFrame;
class PthreadInitializationFrame;
class PosixSemaphore;
class PosixBarrier;
class SourceProgramFacts;
class ThreadArgumentWindows;

// Native storage has no numerical encoding of a thread-instance identity.
enum class NativeHandleShape { Array, Scalar };
struct NativeHandleArray {
  ar::LocalVariable* storage;
  ar::IntegerType* type;
  core::ZNumber size;
  NativeHandleShape shape = NativeHandleShape::Array;
};

/// \brief Global analysis context
///
/// Class holding all the global state of the analyses
class Context {
public:
  /// \brief Input program
  ar::Bundle* bundle;

  /// \brief Analysis options
  ///
  /// NOT declared `const` on purpose. The option set is finalised at the
  /// driver entry point (`ikos_analyzer.cpp` near the
  /// `thread_modular_requested` reconciliation block) where derived
  /// requirements (e.g. "race analysis implies the thread-modular
  /// plugin") may upgrade individual flags. Treating opts as immutable
  /// would force every such reconciliation into `const_cast` scattered
  /// across the codebase, which defeats single-source-of-truth for the
  /// plugin lifecycle. All field accesses are still done through the
  /// `_ctx.opts.<field>` read-only pattern in the value-engine code
  /// (no field is ever assigned outside of `ikos_analyzer.cpp`).
  AnalysisOptions opts;

  /// \brief Working directory
  boost::filesystem::path wd;

  /// \brief Output database
  OutputDatabase* output_db;

  /// \brief Memory location factory
  MemoryFactory* mem_factory;

  /// \brief Variable factory
  VariableFactory* var_factory;

  /// \brief Literal factory
  LiteralFactory* lit_factory;

  /// \brief Call context factory
  CallContextFactory* call_context_factory;

  /// \brief Fixpoint parameters
  FixpointParameters* fixpoint_parameters;

  /// \brief Liveness analysis, or null
  LivenessAnalysis* liveness;

  /// \brief Function pointer analysis, or null
  FunctionPointerAnalysis* function_pointer;

  /// \brief Pointer analysis, or null
  PointerAnalysis* pointer;

  /// \brief Concurrent global environment (thread interference blackboard).
  ///
  /// Owned per-analysis by the Context (not a process singleton): one
  /// environment per analysis run, shared by the thread-modular driver,
  /// the transfer engine, and the data-race checker.
  core::ConcurrentGlobalEnv concurrent_env;

  // Prepared by the thread driver; checker consumers never configure native AI.
  std::shared_ptr< ThreadArgumentWindows > thread_argument_windows;

  // Written by the driver only after replay, destructors and shared flush.
  bool race_analysis_complete = false;
  std::vector< std::string > race_entry_obligations;

  // Range observations are MUST predicates owned by this Context/run.
  bool memory_independent_ranges = false;
  std::map< ar::CallBase*, GlobalArraySubobject > mutex_effect_queries;
  AllAccessFacts< ar::CallBase* > mutex_effect_ranges;
  std::map< ar::Statement*, GlobalArraySubobject > subarray_bounds_queries;
  AllAccessFacts< ar::Statement* > subarray_ranges;
  std::map< std::tuple< ar::GlobalVariable*, ar::ArrayType*, core::ZNumber, core::ZNumber >, bool > readonly_subarray_certificates;
  bool native_handle_inventory_checked = false;
  bool native_handles_only = false;
  std::map< ar::Statement*, NativeHandleArray > native_handle_accesses;
  std::set< ar::CallBase* > native_handle_joins;
  AllAccessFacts< ar::Statement* > native_handle_ranges;

private:
  // Only the driver publishes validated preflight outputs into the fresh run.
  friend class value::ThreadModularAnalysis;
  template < typename Key >
  static bool range_premise(race_rules::Id id,
                            const AllAccessFacts< Key >& facts, Key key) {
    return race_rules::permits_range_premise(id) && facts.proves(key);
  }
  std::set< ar::CallBase* > _mutex_effect_certificates;
  std::map< ar::Statement*, GlobalArraySubobject > _subarray_bounds_certificates;
  std::set< ar::CallBase* > _native_handle_effect_certificates;

  // Whole-source certificates are fixed before ordinary transfer begins.
  // The existing configuration functions are their only writers.
  friend void configure_runtime_stdio_frame(Context&);
  friend void configure_pthread_initialization_frame(Context&);
  friend void configure_posix_semaphore(Context&);
  friend void configure_posix_barrier(Context&);
  friend SourceProgramFacts& source_program_facts(Context&);
  std::shared_ptr< SourceProgramFacts > _source_program_facts;
  std::shared_ptr< const RuntimeStdioFrame > _runtime_stdio_frame_certificate;
  std::shared_ptr< const PthreadInitializationFrame > _pthread_initialization_frame;
  std::shared_ptr< const PosixSemaphore > _posix_semaphore;
  std::shared_ptr< const PosixBarrier > _posix_barrier;

public:
  const std::set< ar::CallBase* >& mutex_effect_certificates() const {
    return _mutex_effect_certificates;
  }
  const std::map< ar::Statement*, GlobalArraySubobject >& subarray_bounds_certificates() const {
    return _subarray_bounds_certificates;
  }
  const std::set< ar::CallBase* >& native_handle_effect_certificates() const {
    return _native_handle_effect_certificates;
  }
  const std::shared_ptr< const RuntimeStdioFrame >& runtime_stdio_frame_certificate() const {
    return _runtime_stdio_frame_certificate;
  }
  const std::shared_ptr< const PthreadInitializationFrame >& pthread_initialization_frame() const {
    return _pthread_initialization_frame;
  }
  const std::shared_ptr< const PosixBarrier >& posix_barrier() const {
    return _posix_barrier;
  }
  const std::shared_ptr< const PosixSemaphore >& posix_semaphore() const {
    return _posix_semaphore;
  }



public:
  /// \brief Constructor
  Context(ar::Bundle* bundle_,
          AnalysisOptions opts_,
          boost::filesystem::path wd_,
          OutputDatabase& output_db_,
          MemoryFactory& mem_factory_,
          VariableFactory& var_factory_,
          LiteralFactory& lit_factory_,
          CallContextFactory& call_context_factory_,
          FixpointParameters& fixpoint_parameters_)
      : bundle(bundle_),
        opts(std::move(opts_)),
        wd(std::move(wd_)),
        output_db(&output_db_),
        mem_factory(&mem_factory_),
        var_factory(&var_factory_),
        lit_factory(&lit_factory_),
        call_context_factory(&call_context_factory_),
        fixpoint_parameters(&fixpoint_parameters_),
        liveness(nullptr),
        function_pointer(nullptr),
        pointer(nullptr) {}

  /// \brief No copy constructor
  Context(const Context&) = delete;

  /// \brief No move constructor
  Context(Context&&) = delete;

  /// \brief No copy assignment operator
  Context& operator=(const Context&) = delete;

  /// \brief No move assignment operator
  Context& operator=(Context&&) = delete;

  /// \brief Destructor
  ~Context() = default;

}; // end class Context

} // end namespace analyzer
} // end namespace ikos
