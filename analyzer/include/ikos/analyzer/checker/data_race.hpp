/*******************************************************************************
 *
 * \file
 * \brief Data race checker
 *
 * Author: ikos-race-detection
 *
 * Contact: ikos@lists.nasa.gov
 *
 * Notices:
 *
 * Copyright (c) 2026 United States Government as represented by the
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
 * THE SUBJECT SOFTWARE.  THIS AGREEMENT DOES NOT, IN ANY MANNER, CONSTITUTE AN
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

#include <array>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>

#include <ikos/analyzer/analysis/memory_location.hpp>
#include <ikos/analyzer/checker/checker.hpp>
#include <ikos/analyzer/checker/race_rules.hpp>
#include <ikos/core/domain/concurrent_global_env.hpp>
#include <ikos/core/value/machine_int/congruence.hpp>

namespace ikos {
namespace analyzer {

class RegionOwnership;
class ReadonlyHeapReads;
class BucketListOwnership;

/// \brief Kind of memory access recorded for race detection
enum class AccessKind { Read, Write };

/// \brief Origin of a ⊤ points-to access (phase 2 LIGHT PROTOTYPE).
///
/// When an access's points-to is ⊤, this distinguishes two causes:
/// - UnknownObject: the pointer is (transitively) rooted in a LOAD of a
///   GLOBAL POINTER whose recorded value is ⊤/absent — the heap identity is
///   lost (weaver `A[i]` with `A = create_fresh_uint_array(N)`), so the
///   checker cannot run a reliable may-race analysis.
/// - KnownObject: the pointer's root is a DynAlloc / stack local / global
///   array-struct (identity KNOWN), and the ⊤ is a domain-compression
///   artefact (container_of, variable index) — the race is real and must stay
///   RACE.
enum class TopProvenance { Concrete, UnknownObject, KnownObject };

/// \brief One memory access logged by DataRaceChecker
///
/// Carries enough information to perform a pairwise race check at the end of
/// the analysis: the statement, the call context, the access kind, the
/// points-to set (resolved at log time), the held-lockset digest at this
/// program point, the thread entry-function name (used as a deterministic
/// thread ID), and the set of threads the current thread has joined at this
/// point (join digest). The lockset digest is empty if the underlying
/// lockset is top ("no known lock protection") per the analyzer's race
/// semantics.
struct AccessRecord {
  ar::Statement* stmt;
  CallContext* call_context;
  AccessKind kind;
  core::PointsToSet< MemoryLocation* > pts;
  std::vector< std::uint64_t > locks;
  /// \brief Shared read-lock digest (pthread_rwlock_rdlock): a shared read
  /// lock does NOT mutually exclude another read lock on the same rwlock,
  /// unlike the exclusive locks in `locks`.
  std::vector< std::uint64_t > read_locks;
  /// \brief Byte offset INTO the collapsed base location, captured at log
  /// time. Restores field/element sensitivity on the DATA side: `data.x`
  /// (offset 0) and `data.y` (offset 4) both resolve to `@data`, but their
  /// offsets are disjoint. Top when the offset is imprecise (conservative).
  core::machine_int::Interval offset;
  /// \brief Byte width of the access, from the target data layout. 0 = unknown
  /// (synthesized extern access or unresolvable type) and is treated as "may
  /// overlap any other access" (conservative). Makes `x` (u32 at [0]) and
  /// `((u8*)&x)[1]` (u8 at [1]) overlap as byte RANGES, not disjoint start
  /// offsets (overlapping-widths.c FN).
  std::uint64_t width = 0;
  /// \brief Stable id of the (single) base MemoryLocation this access
  /// resolved to. 0 when the points-to set is not a singleton. Used to
  /// distinguish "lock on the SAME object" from "lock on a different object".
  std::uint64_t base = 0;
  /// \brief Struct-instance base offset of the access pointer (container_of
  /// inverse). A field lock at a DIFFERENT instance does not protect this
  /// access (06-symbeq_14-list_entry_rc.c: `s++` moves the access to A[1]
  /// while the lock stays on A[0].mutex).
  std::uint64_t instance = 0;
  /// \brief Symbolic array-slot region of this access (flat-array
  /// summarization). `has_region` is true when the access pointer provably
  /// derives from a single flat-array slot `region` via a GLOBAL-array load
  /// only (`slot[i]`); a symbolic lock with the SAME index then protects it
  /// (09-regions_11-arraylist_nr.c FP).
  bool has_region = false;
  core::ConcurrentGlobalEnv::SymbolicIndex region;
  /// \brief Name of the function being analyzed (used as the deterministic
  /// thread ID in this static analysis).
  std::string thread_id;
  /// \brief Cached result of `ConcurrentGlobalEnv::is_thread_unique(
  /// thread_id)` at the time this record was created. Pre-computed so the
  /// finalization never re-enters the global blackboard for every race pair
  /// (which used to dominate the O(N^2) race-check loop).
  bool is_unique_thread = true;
  /// \brief Set of thread INSTANCES (create-site ids) whose termination has
  /// been joined by this thread at this program point. Instance-level: joining
  /// `tids[0]` marks only that element, not every instance of the same entry
  /// function (28/29/30-join-array FN). The checker reduces this to a
  /// per-function "fully joined" test via ConcurrentGlobalEnv's instance
  /// registry.
  std::unordered_set< std::uint64_t > joined_threads;
  /// \brief Set of thread entry-function names already spawned by this
  /// thread at this program point (create-happens-before digest).
  std::unordered_set< std::string > spawned_threads;
  /// \brief Set of thread INSTANCE ids (create-site ids) spawned by this
  /// thread at this program point. Instance-aware create/join HB: a parent
  /// access is ordered before an instance it has NOT spawned yet
  /// (10-synch_13-two_threads_nr.c).
  std::unordered_set< std::uint64_t > spawned_instances;
  /// \brief True when the spawned digest was ⊤ at this program point (an
  /// indirect `pthread_create` whose thread function could not be resolved).
  /// "May have spawned ANY thread" — the create-edge HB check MUST NOT skip
  /// when this is set, since the parent may well have spawned the child.
  bool spawned_is_top = false;
  // MAY direct children at this access; TOP cannot establish nonconcurrency.
  std::unordered_set< std::uint64_t > live_children;
  bool live_children_is_top = true;
  /// \brief True when this access is an atomic load/store (C11 `_Atomic` /
  /// LLVM atomic). Two atomic accesses never race per the C11 definition, so
  /// finalization exempts a pair when BOTH sides are atomic.
  bool is_atomic = false;
  /// \brief Origin of a ⊤ points-to access (phase 2 light prototype). Only
  /// meaningful when `pts.is_top()`; Concrete otherwise. An UnknownObject side
  /// marks the pair UNKNOWN (identity lost); a KnownObject side keeps the
  /// pair RACE (identity known, ⊤ is a compression artefact).
  TopProvenance provenance = TopProvenance::Concrete;
  // Stable local may fact used only by the separate SAFE pair-exclusion query.
  // Extern/synthetic accesses deliberately retain no numerical certificate.
  boost::optional< core::machine_int::Congruence > offset_congruence = boost::none;

};

/// \brief Data race checker
///
/// Detects data races between threads using the per-point lockset digest
/// carried by the memory abstract domain. For each Load/Store statement,
/// the checker queries the lockset held at that program point and compares
/// it against the lockset held at every prior access to the same address.
class DataRaceChecker final : public Checker {
private:
  using PointsToSet = core::PointsToSet< MemoryLocation* >;

public:
  /// \brief Constructor
  explicit DataRaceChecker(Context& ctx);

  /// \brief Release resources without running analysis.
  ~DataRaceChecker() override;

  void finalize() override;

  /// \brief Get the checker name
  CheckerName name() const override;

  /// \brief Get the checker description
  const char* description() const override;

  /// \brief Check a statement
  void check(ar::Statement* stmt,
             const value::AbstractDomain& inv,
             CallContext* call_context) override;

private:
  enum class Phase { Collecting, Finalizing, Complete, Failed };
  Phase _phase = Phase::Collecting;

  void finalize_checks();

  struct CoverageObligation {
    std::string source;
    ar::Statement* statement;
    std::string reason;
    std::string contract;
  };
  struct RaceCoverageSummary {
    enum class State { Pending, Blocked, Checked };
    State state = State::Pending;
    std::vector< CoverageObligation > obligations;
    std::size_t unresolved = 0;
    std::size_t safety_only = 0;
  } _coverage;
  void refresh_coverage();
  void add_coverage_obligation(const std::string& source, ar::Statement* statement,
                               const std::string& reason);
  void publish_result(const char* verdict);
  bool _search_performed = false;
  std::size_t _remaining_pairs = 0;
  struct RaceReport {
    ar::Statement* statement;
    CallContext* context;
    bool confirmed;
    JsonDict info;
  };
  std::vector< RaceReport > _pending_reports;

  struct CoverageDecision {
    race_rules::Id rule;
    ar::Statement* effect;
    std::string reason;
    JsonDict report;
  };
  void consume_coverage_proof(const CoverageDecision& proof);
  void consume_library_effect(race_rules::Id rule, ar::CallBase* call,
                              CallContext* context,
                              boost::optional< JsonDict > report = boost::none);

  struct PairDecision {
    race_rules::Id rule = race_rules::Id::None;
    boost::optional< JsonDict > report;

    static PairDecision exclude(race_rules::Id rule) {
      return PairDecision{rule, boost::none};
    }
    static PairDecision exclude(race_rules::Id rule, JsonDict info) {
      return PairDecision{rule, std::move(info)};
    }
    bool excluded() const { return rule != race_rules::Id::None; }
  };
  void consume_pair_proof(std::size_t i, std::size_t j,
                          const PairDecision& decision);

  using JoinedNames = std::vector< std::unordered_set< std::string > >;
  struct PendingRacePair {
    std::size_t i;
    std::size_t j;
    ar::Statement* report_stmt;
    CallContext* report_cc;
    std::string first_conflict_loc;
    JsonList conflicting_pts; // dump() of every cell in a.pts ∧ b.pts
    // Only a surviving mixed atomic/non-atomic pair is a model boundary.
    bool is_unknown = false;
  };

  void check_effect_coverage(ReadonlyHeapReads& readonly_heap_reads,
                             BucketListOwnership& bucket_list_ownership);
  void prepare_access_regions();
  std::vector< PendingRacePair > collect_race_pairs(
      ReadonlyHeapReads& readonly_heap_reads,
      BucketListOwnership& bucket_list_ownership,
      JoinedNames& joined_names);
  void search_race_candidates(const std::vector< PendingRacePair >& all_pairs,
                              const JoinedNames& joined_names);
  void report_race_group(const PendingRacePair& selected, bool group_is_unknown,
                         bool bmc_confirmed, const JsonDict& info);

  /// \brief Log a Load statement into the access digest
  void check_load(ar::Load* load,
                  const value::AbstractDomain& inv,
                  CallContext* call_context);

  /// \brief Log a Store statement into the access digest
  /// \brief Synthesize Write access records for pointer arguments of
  /// calls WITHOUT an analyzable body (extern functions like scanf):
  /// the library may write through the argument even though no explicit
  /// ar::Store exists, so the race checker would otherwise see no
  /// access on the main side (04-mutex_20-stdfun_rc.c FN).
  void check_extern_call_effects(ar::CallBase* call,
                                 const value::AbstractDomain& inv,
                                 CallContext* call_context);

  void check_store(ar::Store* store,
                   const value::AbstractDomain& inv,
                   CallContext* call_context);

  /// \brief Resolve the points-to set of the given pointer operand, or
  /// return an empty PointsToSet on failure.
  PointsToSet resolve_points_to(ar::Value* pointer,
                                const value::AbstractDomain& inv) const;

  /// \brief Resolve the byte offset of the given pointer operand into its
  /// base location. Returns top when the offset is imprecise (conservative).
  core::machine_int::Interval resolve_offset(
      ar::Value* pointer, const value::AbstractDomain& inv) const;

  boost::optional< core::machine_int::Congruence > resolve_offset_congruence(
      ar::Value* pointer, const value::AbstractDomain& inv) const;

  /// \brief Struct-instance base offset of the given pointer operand
  /// (container_of inverse). A PointerShift result's BASE pointer offset is
  /// the containing struct/array element base; whole-object pointers fall
  /// back to their flat offset. See numerical.hpp instance_base_offset.
  std::uint64_t instance_base_offset(
      ar::Value* pointer, const value::AbstractDomain& inv) const;

  /// \brief Extract the held-lockset digest at the current program point.
  /// Returns an empty vector when the lockset is top (unknown / no locks).
  ///
  /// \param access_guard Canonical (affine-peeled) branch guard of the access
  /// being snapshot, or 0 when the access is not directly guarded by an
  /// `if (guard)` branch. Resolves HELD_IF_NONZERO conditional locks by SSA
  /// identity against it.
  std::vector< std::uint64_t > snapshot_locks(
      const value::AbstractDomain& inv, std::uint64_t access_guard = 0) const;

  /// \brief Extract the shared read-lock digest (pthread_rwlock_rdlock).
  std::vector< std::uint64_t > snapshot_read_locks(
      const value::AbstractDomain& inv) const;

  /// \brief Extract the joined-threads digest at the current program point.
  /// Returns an empty set when the digest is top (unknown / no joins
  /// recorded).
  std::unordered_set< std::string > snapshot_spawned_threads(
      const value::AbstractDomain& inv) const;

  /// \brief Snapshot of the spawned thread INSTANCE ids (create-site ids).
  std::unordered_set< std::uint64_t > snapshot_spawned_instances(
      const value::AbstractDomain& inv) const;

  /// \brief True iff the spawned digest is ⊤ at the current program point
  /// (indirect pthread_create — "may have spawned any thread").
  bool snapshot_spawned_is_top(const value::AbstractDomain& inv) const;

  std::unordered_set< std::uint64_t > snapshot_joined_threads(
      const value::AbstractDomain& inv) const;

  /// \brief Return the deterministic thread ID for the function currently
  /// being analyzed: the name of the function that contains `stmt`. This is
  /// the entry function of whichever thread the analyzer is currently
  /// tracing.
  static std::string current_thread_id(ar::Statement* stmt,
                                       CallContext* call_context);

  /// \brief Return true if the points-to set contains at least one global or
  /// dynamic-alloc memory location (i.e., a memory cell that could be shared
  /// across threads). Stack-local addresses are filtered out to avoid
  /// quadratic blow-up on large programs.
  bool touches_shared_memory(const PointsToSet& pts) const;

  /// \brief Soundness gate for flat-array region tainting: true iff no global
  /// pointer slot is ever assigned a LOADED pointer (a `slot[k] = slot[j]`
  /// cross-slot alias), which would make "load from slot[k] ⟹ region k"
  /// unsound. Computed once from a static scan of all stores.
  bool region_tainting_sound();

private:
  /// \brief Access digest collected during the analysis pass
  std::vector< AccessRecord > _accesses;

  std::unique_ptr< RegionOwnership > _ownership;
  std::array< std::uint64_t, static_cast< std::size_t >(race_rules::Id::Count) >
      _pair_rule_counts{};

  // Opt-in research configuration; native mode is the solver-free ablation.
  bool _pair_exclusion_enabled = false;
  bool _pair_exclusion_solver = false;

  // Conjunction across checked contexts; only shared-independent integer
  // computations may discharge a BMC definedness obligation.
  std::map< ar::BinaryOperation*, bool > _bmc_no_wrap;
  std::map< ar::Code*, bool > _bmc_local_control;

  /// \brief Result of the flat-array region-tainting soundness gate, cached
  /// after the first computation.
  bool _region_sound = true;
  bool _region_sound_computed = false;

  /// \brief Forward region label for heap nodes: maps a DynAlloc stable_id to
  /// the flat-array slot it was STORED into (`list_add(new(3), slot[i])`
  /// stores the malloc result into slot[i], so the node's region is {i}). Fills
  /// the region the def-chain based region_collect cannot recover on the WRITE
  /// side (the store lives in the caller after an inlined function returns the
  /// allocation) — 11/13/17/19 race on `new`'s `p->datum = x`.
  std::unordered_map< std::uint64_t,
                      core::ConcurrentGlobalEnv::SymbolicIndex >
      _dyn_alloc_region;
  /// \brief Heap nodes stored into TWO different slots (`list_add(p, slot[j])`
  /// then `list_add(p, slot[k])`): their region is not unique, so no forward
  /// label may be applied (would match the wrong slot's lock — 12-arraycollapse
  /// rc must stay a race).
  std::unordered_set< std::uint64_t > _dyn_alloc_ambiguous;

  /// \brief Thread entry function name -> set of creator thread entry
  /// names, from a static scan of pthread_create call sites. Empty set
  /// for threads nobody creates (the implicit main thread).
  std::unordered_map< std::string, std::unordered_set< std::string > >
      _thread_creators;

  /// \brief Transitive closure of `_thread_creators`: name -> all thread
  /// entry names that DIRECTLY OR TRANSITIVELY create it. Consulted by the
  /// create-chain HB (main → t1_fun → t2_fun: main's pre-create writes
  /// happen-before t2_fun's — 10-synch_15-join_other_nr.c).
  std::unordered_map< std::string, std::unordered_set< std::string > >
      _thread_ancestors;

  bool _creators_built = false;

  /// \brief A pthread_create call site, for the SV-COMP violation witness's
  /// thread-registration (function_enter) waypoints: the call statement plus
  /// the thread entry function it creates.
  struct ThreadCreation {
    ar::Statement* stmt;
    std::string child;
  };

  /// \brief All pthread_create call sites, ordered by source location. Filled
  /// by build_thread_creators(); consumed by finalization to emit
  /// `thread_creations` in the race report JSON.
  std::vector< ThreadCreation > _thread_creations;

  /// \brief One-time static scan of all pthread_create call sites,
  /// filling _thread_creators. Only direct callees with a constant
  /// thread-function argument are recorded (indirect creation simply
  /// stays out of the map — precision loss, never unsoundness).
  void build_thread_creators();

}; // end class DataRaceChecker

} // end namespace analyzer
} // end namespace ikos
