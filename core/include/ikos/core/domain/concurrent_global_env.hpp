/*******************************************************************************
 *
 * \file
 * \brief Concurrent global environment (global blackboard) for privatization
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
 * RECIPIENT'S USE OF THE SUBJECT SOFTWARE, RECIPIENT SHALL INDEMNIFY AND HOLD
 * HARMLESS THE UNITED STATES GOVERNMENT, ITS CONTRACTORS AND SUBCONTRACTORS,
 * AS WELL AS ANY PRIOR RECIPIENT, TO THE EXTENT PERMITTED BY LAW. RECIPIENT'S
 * SOLE REMEDY FOR ANY SUCH MATTER SHALL BE THE IMMEDIATE, UNILATERAL
 * TERMINATION OF THIS AGREEMENT.
 *
 ******************************************************************************/

#pragma once

#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <atomic>
#include <iostream>

#include <ikos/ar/semantic/function.hpp>
#include <ikos/core/domain/machine_int/interval.hpp>
#include <ikos/core/domain/numeric/dbm.hpp>
#include <ikos/core/linear_expression.hpp>
#include <ikos/core/number/machine_int.hpp>
#include <ikos/core/number/z_number.hpp>
#include <ikos/core/value/pointer/points_to_set.hpp>
#include <ikos/analyzer/analysis/memory_location.hpp>
#include <ikos/analyzer/analysis/variable.hpp>

namespace ikos {
namespace core {

/// \brief Public record type for the PBR (Protection-Based Reading)
/// deferral queue: one deferred write of a GLOBAL variable — its address
/// and the interval value written under the held lock(s).
///
/// Declared at namespace scope (not nested inside `ConcurrentGlobalEnv`)
/// because `_privatized_writes` (a private member of the env) needs the
/// complete type at its declaration site, and the env declares this
/// member before any `public:` block — keeping the type outside the class
/// body lets the member declaration see the complete type regardless of
/// access-specifier order.
struct PrivatizedWrite {
  std::uint64_t addr;
  machine_int::Interval val;
};

/// \brief One lock's partition of the interference environment: the
/// interval value per global address written under that lock. Read by
/// `read_global_with_lockset` pass (b); widened by the widening
/// discipline (`join_into_cell_nolock`) at the iteration boundary.
struct LockPartition {
  std::unordered_map< std::uint64_t /*glob_addr*/, machine_int::Interval >
      intervals;
};

/// \brief Reserved thread-instance id for the implicit MAIN thread.
///
/// `pthread_self()` in main returns this, and `pthread_join(mainid)` joins it,
/// so a thread that joins the main thread happens-after main's writes
/// (51-threadjoins/09-join-main.c). Distinct from real create-site ids (small
/// monotonic counters starting at 1).
constexpr std::uint64_t kMainThreadSid = 0xA7E0AD1DULL;

/// \brief Concurrent global environment (global blackboard) for privatization
///
/// This singleton class provides a thread-safe global blackboard for
/// tracking global-variable interference intervals and discovered thread
/// functions. It is the foundation for data-race detection: the value
/// analysis reads back the joined interference via `read_global`, and the
/// DataRaceChecker queries `is_thread_unique` (ESOP'23 thread-uniqueness
/// abstraction) to short-circuit same-thread race pairs.
class ConcurrentGlobalEnv {
public:
  /// \brief Symbolic array slot identity: `coeff * idx_var + const_term`,
  /// where the index variable is identified by its AR InternalVariable
  /// pointer (stable within a run — the same SSA value maps to the same
  /// pointer on both the lock and data side).
  struct SymbolicIndex {
    std::uint64_t var_id = 0;    ///< index-variable identity
    std::int64_t coeff = 1;      ///< coefficient of the index variable
    std::int64_t const_term = 0; ///< additive constant (j vs j+1)

    bool operator==(const SymbolicIndex& o) const {
      return var_id == o.var_id && coeff == o.coeff && const_term == o.const_term;
    }
    bool operator!=(const SymbolicIndex& o) const { return !(*this == o); }
  };

private:
  /// \brief Global blackboard: address -> machine integer interval
  std::map< std::uint64_t, machine_int::Interval > _global_board;

  /// \brief Protection-Based Reading (Goblint SAS'21) privatized write log.
  ///
  /// Keyed by mutex address; each entry is a list of (global_addr, value)
  /// writes that were DEFERRED (skipped join_global) because the lock was
  /// held at the write site. The values are captured at write time from
  /// the local RHS interval, NOT extracted from the memory cell at unlock
  /// time — this is the soundness-bias choice in the SAS'21 paper
  /// (over-approximation is preferred for race detection).
  ///
  /// Lifecycle:
  ///   - exec_store:    env.record_privatized_write(lock_addr, addr, val)
  ///   - exec_unlock:   env.flush_privatized(lock_addr) → joins per-addr,
  ///                     then join_global(addr, joined) for each; clears.
  ///
  /// The registry is keyed only by lock_addr (no per-function state), so
  /// it does NOT need to be reset at function entry — a function called
  /// inside a held critical section should still propagate its writes
  /// into the lock's privatized set (see CI-5 in the design review).
  std::unordered_map< std::uint64_t /*lock_addr*/,
                      std::vector< PrivatizedWrite > >
      _privatized_writes;

  /// \brief Lock-partitioned blackboard (AstréeA-style interference,
  /// interval-only now that the relational DBM backend was removed).
  ///
  /// `_lock_partition[L].intervals` maps each global address written under
  /// lock `L` to its interval value. Populated exclusively by
  /// `flush_privatized(L)` (the sentinel value 0 is intercepted to
  /// `flush_unlocked_partition`); read by `read_global_with_lockset` pass
  /// (b). The flat `_global_board` is preserved for unlocked interference.
  /// The widening discipline (count-then-force-TOP) operates on
  /// `LockPartition::intervals`.
  std::unordered_map< std::uint64_t /*lock_addr*/, LockPartition >
      _lock_partition;

  /// \brief Set of discovered thread functions (as pointers for direct use)
  std::vector< ar::Function* > _discovered_thread_functions;

  /// \brief Global fixpoint iteration counter (Cousot-Cousot delayed
  /// widening).
  ///
  /// Written once per global iteration by the outer iterator via
  /// `set_global_iteration`; read by `join_into_cell_nolock` to switch
  /// from JOIN (≤ `kWideningDelay`) to WIDENING (> `kWideningDelay`).
  /// This is the ORDER-INSENSITIVE widening schedule: the blackboard
  /// accumulates the precise join of all interferences during the delay,
  /// and only then widens the still-growing bounds — the result no longer
  /// depends on the order in which threads deposit interference. Replaces
  /// the old per-cell consecutive-expansion counters (a late interference
  /// could spuriously trigger TOP and erase the race — arrayloop2_rc /
  /// evilcollapse_rc FN).
  std::atomic< std::uint64_t > _global_iteration{0};

  /// \brief Thread creation count keyed by entry-function NAME.
  ///
  /// Used by the "Thread Uniqueness" abstraction (ESOP'23): if a thread
  /// function is spawned exactly ONCE across the whole program, all of its
  /// instantiations are sequential with respect to each other (no two
  /// concurrently running copies exist), so the same-thread HB short-circuit
  /// in DataRaceChecker can safely drop races among its accesses.
  ///
  /// This is a sound static approximation: a function spawned >1 times
  /// conservatively models concurrent instances. A function never spawned
  /// is treated as unique (cannot race against itself).
  std::unordered_map< std::string, unsigned > _spawn_counts_by_name;

  /// \brief Distinct pthread_create call SITES per thread-function name.
  /// The spawn count is the STATIC number of call sites, not the execution
  /// count — a fixpoint replay re-executes the same call site (bumping an
  /// execution counter to 3 for a single `pthread_create`) without adding a
  /// second concurrent instance, which would poison Thread Uniqueness and
  /// fabricate self-race pairs (09-regions_02-list_nr.c).
  std::unordered_map< std::string, std::unordered_set< ar::CallBase* > >
      _spawn_sites_by_name;

  /// \brief Cache of `spawns_are_sequential_locked` (name → sequential?).
  /// Static: the create-site CFG shape does not change across iterations.
  mutable std::unordered_map< std::string, bool > _sequential_cache;

  /// \brief Pending conditional-lock fact for an `if (i) lock(m)` branch,
  /// keyed by the FALSE-branch block. The lock detection (running in the
  /// TRUE branch) records the correlation here; the Comparison in the false
  /// branch replays it onto its own state so the must-lockset intersection at
  /// the join PRESERVES the correlation instead of dropping it
  /// (04-mutex_07-ps_nr / 17-ps_add1_nr.c). Idempotent: replayed fixpoint
  /// visits insert the same entry.
  struct PendingCondLock {
    std::uint64_t guard;      ///< canonical base AR var pointer (affine-peeled)
    std::uint64_t lock_addr;  ///< lock key (as admitted by the lock detection)
    bool is_nonzero;          ///< true ⇒ held iff guard != 0
  };
  std::unordered_map< ar::BasicBlock*, std::vector< PendingCondLock > >
      _pending_cond_locks;

  /// \brief Stable id registry for pthread_create call sites (thread
  /// instances). Ids are monotonic starting at 1; 0 is reserved for "no site"
  /// (the implicit main instance / an unresolved create). Deterministic within
  /// a run AND across runs: assignment follows the (sorted) analysis order.
  std::unordered_map< ar::CallBase*, std::uint64_t > _site_ids;
  std::uint64_t _next_site_id = 1;

  /// \brief Thread instances per entry-function name: name → set of create-site
  /// ids. A STATIC program property — NOT reset at iteration boundaries (unlike
  /// `_spawn_sites_by_name`). Consulted by the race checker to decide "all
  /// instances of F have been joined" (instance-aware join HB).
  std::unordered_map< std::string, std::unordered_set< std::uint64_t > >
      _func_instances;

  /// \brief Reverse site-id registry: create-site id → entry function. Filled
  /// alongside `_func_instances` in `register_thread_func`; consulted by
  /// pthread_join to pull the joined thread's MUST-join summary (transitive
  /// join HB).
  std::unordered_map< std::uint64_t, ar::Function* > _site_func;

  /// \brief pthread_once callback instance ids, keyed by a stable identity of
  /// the once-CONTROL object (arg0). A once-callback runs once PER DISTINCT
  /// control: two once-calls on the same control must share ONE instance id
  /// (else "all instances joined" can never hold), two on different controls
  /// get two (they run twice and race — 87-once/07-different-onces.c). The
  /// ambiguous-control case keys by the call site instead (never joinable).
  std::unordered_map< std::uint64_t, std::uint64_t > _once_sids;

  /// \brief Per-function MUST-join summary: entry function → the set of thread
  /// instances it definitely joined by the time it exits (its exit invariant's
  /// joined digest, transitively closed). Written by the thread-modular driver
  /// after each function fixpoint; read by pthread_join to implement transitive
  /// join HB (main joins t_benign, t_benign joins t_fun ⇒ main transitively
  /// joins t_fun — 51-threadjoins/01-trivial.c).
  std::unordered_map< ar::Function*, std::unordered_set< std::uint64_t > >
      _join_summary;

  /// \brief Condition-variable wait queue: cond address → count of threads
  /// currently blocked on it. Populated by `cond_wait_enter`, drained by
  /// `cond_signal` (one) / broadcast (all). Used for broadcast-vs-signal
  /// modeling; NOT consumed by the race checker (the signal→wait edge is MAY —
  /// spurious wakeup — so it cannot soundly suppress a race). Reset at each
  /// global-iteration boundary (see reset_spawn_counts).
  std::unordered_map< std::uint64_t, std::size_t > _cond_waiters;

  /// \brief Condition-variable signal epoch: cond address → monotonic count of
  /// signal/broadcast events. A wait-return with a lower epoch than a later
  /// signal MAY have been woken by it (conservative MAY state for future value
  /// analysis; never used to suppress a race).
  std::unordered_map< std::uint64_t, std::uint64_t > _cond_signal_epoch;

  /// \brief Map from a thread entry function to the points-to set of the
  /// 4th pthread_create argument (`arg`) captured at the spawn site.
  ///
  /// IPA context binding: without this, the child thread's formal `void*`
  /// parameter stays TOP, so any dereference through it (`(int*)arg`)
  /// loses its target and the race is missed (04-mutex_45-escape_rc.c).
  /// Multiple spawns of the same function with different args JOIN their
  /// points-to sets (defensive: the child may receive any of them).
  /// \brief IPA spawn-arg record: thread entry function → the points-to set
  /// AND byte offset of the `arg` actual.
  ///
  /// The OFFSET is essential for `container_of`-style field recovery: `&s->f`
  /// (base `s`, byte offset `off`) passed as `arg` must reach the child WITH
  /// the offset — otherwise `container_of` subtracts `off` from a base that
  /// already lost it and the deref lands at the wrong byte
  /// (race-2_3b-container_of.c FN).
  struct SpawnArg {
    PointsToSet< analyzer::MemoryLocation* > pts;
    machine_int::Interval offset;
  };
  std::unordered_map< ar::Function*, SpawnArg > _spawn_args;

  /// \brief MUST joined digest at a pthread_create point, keyed by the child
  /// thread function AND the distinguishing create site.
  ///
  /// "join-then-create" HB: when the parent joins T and THEN creates U, T's
  /// accesses happen-before U's (T terminated before U started). The child U's
  /// entry `joined` digest must therefore inherit the threads the parent had
  /// DEFINITELY joined at the create point (bigshot_s/singleton_with-uninit:
  /// `create(t1); join(t1); create(t2);` — t2's `if(v)` must not race t1's
  /// `v=malloc`). Per (function, site) the value is OVERWRITTEN (the parent's
  /// fixpoint is monotone in the MUST lattice, so the last visit is the
  /// converged value); `get_spawn_joined` intersects over sites (a thread is
  /// "definitely joined before the child" only if joined at EVERY spawn site).
  struct SpawnJoined {
    bool top = true;                          // ⊤ = no definite join
    std::unordered_set< std::uint64_t > set;  // site ids definitely joined
  };
  std::unordered_map< ar::Function*,
                      std::unordered_map< ar::CallBase*, SpawnJoined > >
      _spawn_joined;

  /// \brief Set of LOCATIONS whose address escaped into a thread `arg`.
  ///
  /// A stack local is normally thread-private, but passing `&i` as the
  /// pthread_create arg escapes it — the child dereferences the same cell
  /// (`04-mutex_45-escape_rc.c`). `DataRaceChecker::touches_shared_memory`
  /// consults `is_escaped` to treat these locals as cross-thread visible.
  std::unordered_set< analyzer::MemoryLocation* > _escaped_locs;

  /// \brief Pointer blackboard cell: the points-to set AND the byte offset of
  /// a global pointer field's value.
  ///
  /// The offset must ride along with the points-to set — the old
  /// PointsToSet-only board dropped it, so a reader's `container_of`
  /// subtracted `offsetof` from a base that had already lost its field
  /// offset and the deref landed at the wrong byte (09-regions_20
  /// -arrayloop2_rc.c FN). Mirrors `SpawnArg`'s offset-transport pattern.
  struct GlobalPointer {
    PointsToSet< analyzer::MemoryLocation* > pts;
    machine_int::Interval offset;
  };

  /// \brief Pointer-domain blackboard channel: global pointer variable →
  /// the points-to set (and its byte offset) it currently holds.
  ///
  /// The flat `_global_board` is interval-only, so a global POINTER's
  /// DynAlloc points-to is lost at the thread handoff (`02-base_24
  /// -malloc_races.c`: `x = malloc()` in main never reaches t_fun, so
  /// `*y` dereferences to an empty set). This channel propagates it.
  /// Written by `join_global_pointer` (Store side) and consumed at thread
  /// entry (thread_modular.cpp injects it into the pointer domain).
  std::unordered_map< analyzer::MemoryLocation*, GlobalPointer >
      _global_pointer_board;

  /// \brief Reverse index of the pointer blackboard: the set of heap DynAlloc
  /// locations (and any other targets) that some global pointer may point to.
  /// Consulted by `is_global_pointer_target` to classify a deref target as
  /// cross-thread SHARED (vs a thread-private malloc). Kept in sync with
  /// `_global_pointer_board` in `join_global_pointer` and `clear`.
  std::unordered_set< analyzer::MemoryLocation* > _global_pointer_targets;

  /// \brief Heap-field pointer blackboard channel: (alloc-site stable_id << 32
  /// | field byte offset) → the points-to set (+ offset) that heap field
  /// currently holds. The global `_global_pointer_board` only carries GLOBAL
  /// pointer variables; a DynAlloc field (`A->next = p`) is invisible there,
  /// so a cross-thread deref through it (`A->next->datum++`) loses the datum
  /// access (09-regions_03-list2_rc.c FN). Keyed by allocation site × field
  /// offset (reversible pack) so distinct list nodes / fields never collide,
  /// and the per-key set stays bounded by `kPtrBoardWidenThreshold`.
  std::unordered_map< std::uint64_t, GlobalPointer > _heap_pointer_board;

  /// \brief Dirty flag for the global-fixpoint driver
  /// (`analysis/value/interprocedural/sequential/analysis.cpp`).
  /// Set by `join_global` whenever the blackboard state actually expands;
  /// the driver reads it after each worklist pass to decide whether another
  /// iteration is required.
  bool _is_dirty = false;

  /// \brief Lock key → struct-instance base offset (container_of inverse).
  ///
  /// The lock KEY is the flat byte offset (identity: distinguishes m[0]/m[1],
  /// m.x/m.y). This side table carries the lock's PROTECTION REGION — the
  /// struct instance base — so the race checker can refuse to let A[0].mutex
  /// protect A[1].datum (06-symbeq_14-list_entry_rc.c). Populated by
  /// `record_lock_instance` at lock admission, read by the checker.
  std::unordered_map< std::uint64_t, std::uint64_t > _lock_instance;

  /// \brief Symbolic-index side table for variable-offset locks.
  ///
  /// A `pthread_mutex_lock(&mutex[i])` with a VARIABLE offset `i` cannot be
  /// keyed by flat offset (that would collapse mutex[j]/mutex[k] to one lock).
  /// Instead the lock is admitted under a DETERMINISTIC key (a mix of base +
  /// SymbolicIndex, so the same symbolic slot always maps to the same key and
  /// the lockset is stable across fixpoint iterations), and this table records
  /// `key → (base stable_id, SymbolicIndex)` so the checker can decide
  /// "lock slot == access slot" via `SymbolicIndex` equality (09-regions_11
  /// -arraylist_nr.c FP).
  std::unordered_map< std::uint64_t, std::pair< std::uint64_t, SymbolicIndex > >
      _lock_symbolic_index;

  /// \brief Mutex for thread safety
  mutable std::mutex _mutex;

  /// \brief Deleted copy constructor (std::mutex is not copyable/movable)
  ConcurrentGlobalEnv(const ConcurrentGlobalEnv&) = delete;

  /// \brief Deleted move constructor
  ConcurrentGlobalEnv(ConcurrentGlobalEnv&&) = delete;

  /// \brief Deleted copy assignment
  ConcurrentGlobalEnv& operator=(const ConcurrentGlobalEnv&) = delete;

  /// \brief Deleted move assignment
  ConcurrentGlobalEnv& operator=(ConcurrentGlobalEnv&&) = delete;

public:
  /// \brief Default constructor.
  ///
  /// De-singletonized: the environment is a per-analysis object owned by
  /// the analyzer `Context` (the `Context::concurrent_env` member), not a
  /// process-wide singleton.
  ConcurrentGlobalEnv() = default;

public:
  /// \brief Sentinel lock address for the unlocked-write partition.
  ///
  /// Used by `NumericalExecutionEngine::exec(Store*)` (the `!has_locks`
  /// branch) to route unlocked writes through `record_privatized_write`,
  /// and by `ThreadModularAnalysis::run()` to drain the sentinel at
  /// iteration boundaries.
  ///
  /// \warning This must NOT reach `flush_privatized(0)` from a real
  /// `pthread_mutex_unlock` whose points-to resolution failed — see
  /// the `unlock_addr_known` guard at `numerical.hpp:4318`. The guard
  /// ensures the sentinel value 0 is used only as a key for the
  /// sentinel partition, never as the address of a real mutex whose
  /// points-to went wrong.
  static constexpr std::uint64_t kNoLockPartitionAddr = 0;

  /// \brief Cousot-Cousot delayed-widening delay: number of global
  /// fixpoint iterations that use pure JOIN (gathering the precise,
  /// order-independent union of all interferences) before widening is
  /// enabled.
  static constexpr std::uint64_t kWideningDelay = 2;

  /// \brief Record a lock's protection region (struct-instance base offset),
  /// keyed by the lock key. See `_lock_instance`.
  void record_lock_instance(std::uint64_t key, std::uint64_t instance) {
    std::lock_guard< std::mutex > lock(this->_mutex);
    this->_lock_instance[key] = instance;
  }

  /// \brief Look up a lock's protection region (struct-instance base offset).
  /// Returns 0 when unknown (whole-object lock).
  std::uint64_t lock_instance(std::uint64_t key) const {
    std::lock_guard< std::mutex > lock(this->_mutex);
    auto it = this->_lock_instance.find(key);
    return (it == this->_lock_instance.end()) ? 0 : it->second;
  }

  /// \brief Admit a variable-offset lock under a deterministic symbolic key
  /// and bind its (base, SymbolicIndex). Returns the key to push into the
  /// lockset. The key is a pure function of (base, sym) so the same symbolic
  /// slot always yields the same key (fixpoint-stable); a 2^-64 collision
  /// would only OVER-approximate the lock (sound direction).
  std::uint64_t record_lock_symbolic_index(std::uint64_t base,
                                           const SymbolicIndex& sym) {
    std::lock_guard< std::mutex > lock(this->_mutex);
    std::uint64_t key = sym.var_id;
    key ^= base + 0x9e3779b97f4a7c15ULL + (key << 6) + (key >> 2);
    key ^= static_cast< std::uint64_t >(sym.coeff) + 0x9e3779b97f4a7c15ULL +
           (key << 6) + (key >> 2);
    key ^= static_cast< std::uint64_t >(sym.const_term) + 0x9e3779b97f4a7c15ULL +
           (key << 6) + (key >> 2);
    this->_lock_symbolic_index[key] = {base, sym};
    return key;
  }

  /// \brief Look up a symbolic lock's (base, SymbolicIndex). Returns false
  /// when `key` is not a symbolic lock (i.e. a constant-offset lock).
  bool lock_symbolic_index(std::uint64_t key, std::uint64_t& base,
                           SymbolicIndex& sym) const {
    std::lock_guard< std::mutex > lock(this->_mutex);
    auto it = this->_lock_symbolic_index.find(key);
    if (it == this->_lock_symbolic_index.end()) {
      return false;
    }
    base = it->second.first;
    sym = it->second.second;
    return true;
  }

  /// \brief Set the global fixpoint iteration counter.
  ///
  /// Called by the outer concurrent iterator (thread_modular.cpp) at the
  /// start of every global iteration, BEFORE any thread is analyzed.
  /// `join_into_cell_nolock` reads it to decide join-vs-widen. This is
  /// the ORDER-INSENSITIVE widening schedule that replaces the old
  /// per-cell consecutive-expansion counters (whose result depended on
  /// the arrival order of interferences).
  void set_global_iteration(std::uint64_t n) {
    _global_iteration.store(n, std::memory_order_relaxed);
  }

  /// \brief Recover the global mutex state after a hardware-signal
  /// longjmp (Bug 5 fix, Semantic Purification Stage 3).
  ///
  /// **What this is for.** When a SIGSEGV/SIGBUS/SIGILL/SIGFPE fires
  /// inside a critical section protected by `std::lock_guard`, the
  /// `siglongjmp` jumps back across the lock_guard destructor — the
  /// mutex is left **permanently locked**. Any subsequent acquisition
  /// by the same thread will deadlock.
  ///
  /// **How it works.** IKOS's value analysis is single-threaded: only
  /// the current thread can have held `_mutex` at signal delivery. We
  /// therefore try to grab the mutex non-blocking; if we get it, we WERE
  /// the stuck holder and `unlock()` is safe. If `try_lock` fails (a
  /// different thread genuinely owns it — unreachable in IKOS in
  /// practice), we do nothing and log a diagnostic; the analyzer will
  /// deadlock on the next acquisition, surfacing the underlying bug
  /// instead of silently corrupting state.
  ///
  /// **Callers.** Only invoke this from the signal-recovery branch in
  /// `ikos_analyzer.cpp` (after `sigsetjmp` returns non-zero). Calling
  /// it from normal flow would unlock another critical section's mutex
  /// and corrupt invariants.
  void force_unlock_after_signal() noexcept {
    if (this->_mutex.try_lock()) {
      // We just acquired the mutex that was left in a locked state by
      // a siglongjmp over a lock_guard destructor. Release it.
      this->_mutex.unlock();
    }
    // else: the mutex is genuinely held by another thread. In IKOS this
    // branch is unreachable (single-threaded value analysis); if hit,
    // subsequent acquisition will deadlock and surface the bug.
  }

  /// \brief Read a global variable
  ///
  /// \param addr The address of the global variable
  /// \return The interval value, or top if not found
  machine_int::Interval read_global(std::uint64_t addr) const {
    std::lock_guard< std::mutex > lock(_mutex);
    auto it = _global_board.find(addr);
    if (it != _global_board.end()) {
      return it->second;
    }
    return machine_int::Interval::top(64, Signed);
  }

  /// \brief Snapshot of the flat `_global_board` (addr -> interval), for
  /// thread-local-semantics entry materialization: the thread-modular driver
  /// joins each entry into the thread's LOCAL invariant at function entry, so
  /// cross-thread integer interference is visible WITHOUT a per-read override.
  std::unordered_map< std::uint64_t, machine_int::Interval >
  snapshot_global_board() const {
    std::lock_guard< std::mutex > lock(_mutex);
    return {_global_board.begin(), _global_board.end()};
  }

  /// \brief AstréeA lock-aware read with sound signedness.
  ///
  /// For each lock L in `held_locks`, consult both:
  ///   (a) the unflushed deferral queue `_privatized_writes[L]` (the
  ///       current thread's pending writes under L — sound to consult
  ///       because we are the writer), and
  ///   (b) the aggregated partition `_lock_partition[L]` (the value
  ///       of the global at the last unlock of L).
  /// Return the join of all hits. If no held lock has state for `addr`,
  /// fall back to `_global_board[addr]` only if the entry exists;
  /// otherwise return **Bottom**.
  ///
  /// \warning The "absence → Bottom" contract is REQUIRED for
  /// soundness at the call site: `exec_load` uses
  /// `if (!interference.is_bottom()) { ... inject ... }` to gate
  /// unconditional injection. Returning Top here would silently widen
  /// every local global read that has no concurrent writer, which is
  /// unsound. Top returned from this method is reserved for the case
  /// where real concurrent contention has forced a partition cell to
  /// widen — that Top MUST be absorbed by the local value.
  ///
  /// \warning Sign and bit-width contract. Each candidate interval
  /// (from the PBR deferral queue, a lock partition, or the flat board)
  /// is normalized to the caller's `(bit_width, sign)` via
  /// `Interval::cast` BEFORE joining. Without this normalization, two
  /// candidates with mismatched signs would trip
  /// `assert_compatible<Interval,Interval>` in `join_with` (see
  /// compatibility.hpp:62), and a candidate with the wrong sign would
  /// silently widen a `m : unsigned int` to Signed 64-bit, breaking
  /// every later `m == 0` / `value < n` comparison. The fix here is
  /// not a guard at the injection site (numerical.hpp) — it is a
  /// type-aware result construction INSIDE this method, so that ALL
  /// callers (now and future) get a correctly-signed interval.
  ///
  /// \param addr        The global address being read.
  /// \param bit_width   Bit width of the destination variable (e.g. 32
  ///                    for `unsigned int`, 64 for `unsigned long long`).
  ///                    Must match the LLVM type's bit width so that the
  ///                    returned interval can be soundly assigned back.
  /// \param sign        Signedness of the destination variable.
  /// \param held_locks  Locks currently held by the reading thread
  ///                    (typically `inv.normal().lockset_held_mutexes()`).
  /// \return The joined interference interval, expressed in
  ///         `(bit_width, sign)`. Bottom if no held lock and no
  ///         flat-board entry; possibly Top if a partition cell was
  ///         widened by real concurrent contention.
  machine_int::Interval read_global_with_lockset(
      std::uint64_t addr,
      std::uint64_t bit_width,
      Signedness sign,
      const std::unordered_set< std::uint64_t >& held_locks) const {
    std::lock_guard< std::mutex > lock(_mutex);
    // Result is constructed in the destination's (bit_width, sign) so
    // that any subsequent `join_with` (between this and any candidate)
    // is assertion-safe.
    machine_int::Interval result =
        machine_int::Interval::bottom(bit_width, sign);
    bool has_hit = false;

    // Normalize any candidate to the caller's type. This is the key
    // step: a `_global_board[m_addr] = [0, 0]_(32, Unsigned)` entry
    // injected into a Signed variable would otherwise pollute the
    // destination with the wrong sign. `Interval::cast` is a sound
    // trunc/ext+sign_cast composition that widens to Top on overflow,
    // so a mismatch cannot silently narrow the analysis.
    auto normalize =
        [&](const machine_int::Interval& src) -> machine_int::Interval {
      if (src.is_bottom()) {
        return src;
      }
      if (src.bit_width() == bit_width && src.sign() == sign) {
        return src;
      }
      return src.cast(bit_width, sign);
    };

    // (a) Deferral queue: current thread's pending writes under L.
    for (std::uint64_t lk : held_locks) {
      auto pit = _privatized_writes.find(lk);
      if (pit == _privatized_writes.end()) {
        continue;
      }
      for (const PrivatizedWrite& pw : pit->second) {
        if (pw.addr == addr) {
          machine_int::Interval v = normalize(pw.val);
          if (std::getenv("IKOS_RW_TRACE") != nullptr) {
            std::cerr << "[RW-A] addr=" << (const void*)addr << " from queue: "
                      << v << " (queue size " << pit->second.size() << ")\n";
          }
          if (!has_hit) {
            result = v;
            has_hit = true;
          } else {
            result.join_with(v);
          }
        }
      }
    }
    // (b) Aggregated partition: last-known value at unlock.
    for (std::uint64_t lk : held_locks) {
      auto ppit = _lock_partition.find(lk);
      if (ppit == _lock_partition.end()) {
        continue;
      }
      auto cit = ppit->second.intervals.find(addr);
      if (cit == ppit->second.intervals.end()) {
        continue;
      }
      machine_int::Interval v = normalize(cit->second);
      if (std::getenv("IKOS_RW_TRACE") != nullptr) {
        std::cerr << "[RW-B] addr=" << (const void*)addr << " from partition: "
                  << v << "\n";
      }
      if (!has_hit) {
        result = v;
        has_hit = true;
      } else {
        result.join_with(v);
      }
    }
    // (c) Phase 5 Sentinel Peeking.
    //
    // Pending unlocked writes in `_privatized_writes[0]` (the sentinel
    // partition keyed by `kNoLockPartitionAddr`) are guaranteed to flush
    // into `_global_board` at the next iteration boundary (see
    // `flush_unlocked_partition` below). Peeking them into this read
    // makes them visible one iteration earlier — recovering the false
    // positives that strict unlock-flushing lost.
    //
    // Read-only: no mutation of `_privatized_writes`, `_lock_partition`,
    // or `_global_board`. `join_with` only widens (`a ⊔ b`), so this is
    // a sound over-approximation.
    //
    // Note: we use the literal `0` instead of `kNoLockPartitionAddr` to
    // avoid ODR-using the static constexpr member.
    auto pit = _privatized_writes.find(0);
    if (pit != _privatized_writes.end()) {
      for (const PrivatizedWrite& pw : pit->second) {
        if (pw.addr != addr) {
          continue;
        }
        machine_int::Interval v = normalize(pw.val);
        if (!has_hit) {
          result = v;
          has_hit = true;
        } else {
          result.join_with(v);
        }
      }
    }
    if (has_hit) {
      return result;
    }
    // Fallback to flat board. Return Bottom if absent — NOT Top.
    auto git = _global_board.find(addr);
    if (git != _global_board.end()) {
      return normalize(git->second);
    }
    return result; // bottom(bit_width, sign)
  }

  /// \brief Join (merge) a value into a global variable
  ///
  /// \param addr The address of the global variable
  /// \param value the interval value to join
  /// \return true if the blackboard state changed (new or larger interval)
  bool join_global(std::uint64_t addr, const machine_int::Interval& value) {
    std::lock_guard< std::mutex > lock(_mutex);
    return this->join_global_nolock(addr, value);
  }

  /// \brief Outcome of a widening-channel join, consumed by
  /// `join_partition_nolock` to couple the interval circuit breaker.
  enum class JoinResult {
    Unchanged,  ///< join was absorbed (dest ⊒ value already)
    Expanded,   ///< a real expansion (or a new cell) happened
    ForcedTop,  ///< the widening discipline forced the cell to TOP
  };

  /// \brief Internal helper: shared widening core for both the flat
  /// blackboard (`join_global_nolock` → `_global_board`) and per-lock
  /// partitions (`join_partition_nolock` → `_lock_partition[L].intervals`).
  ///
  /// \param dest  Destination cell map (keyed by `addr`, value type
  ///              `machine_int::Interval`).
  /// \param addr  The global variable's address.
  /// \param value The interval to join into the cell.
  /// \return the JoinResult (see enum).
  ///
  /// Caller MUST already hold `_mutex`.
  ///
  /// Widening discipline (Cousot-Cousot DELAYED widening, ORDER-INSENSITIVE):
  /// for the first `kWideningDelay` global iterations we perform a pure JOIN,
  /// accumulating the exact order-independent union of all interferences.
  /// After the delay, a still-growing bound is widened to TOP. The switch is
  /// keyed by the GLOBAL iteration counter (`_global_iteration`), NOT by a
  /// per-cell arrival counter — so the outcome depends only on the SET of
  /// interferences, never on the order in which threads deposited them.
  template < typename CellMap >
  JoinResult join_into_cell_nolock(CellMap& dest,
                                   std::uint64_t addr,
                                   const machine_int::Interval& value) {
    auto it = dest.find(addr);
    if (it == dest.end()) {
      dest.emplace(addr, value);
      this->_is_dirty = true;
      return JoinResult::Expanded;
    }
    machine_int::Interval old_val = it->second;
    // Bit-width/sign normalization: two intervals of different width cannot
    // be joined (`assert_compatible` aborts). The cell's width was set by its
    // first writer; readers normalize to their own width via
    // `read_global_with_lockset`, so the cell width is an internal choice.
    // Up-cast the narrower to the wider and prefer Signed on sign mismatch —
    // `Interval::cast` extends (a sound over-approximation), it never
    // truncates. `old_val` must be normalized too, otherwise the
    // `it->second.leq(old_val)` comparison below re-triggers the assertion.
    if (old_val.bit_width() != value.bit_width() ||
        old_val.sign() != value.sign()) {
      std::uint64_t w = old_val.bit_width() > value.bit_width()
                            ? old_val.bit_width()
                            : value.bit_width();
      Signedness s = (old_val.sign() == value.sign()) ? old_val.sign() : Signed;
      old_val = old_val.cast(w, s);
      it->second = old_val;
    }
    machine_int::Interval v =
        (value.bit_width() == it->second.bit_width() &&
         value.sign() == it->second.sign())
            ? value
            : value.cast(it->second.bit_width(), it->second.sign());
    it->second.join_with(v);
    if (!it->second.leq(old_val)) {
      // State actually expanded after join (old ⊏ joined)
      this->_is_dirty = true;
      // Bidirectional widening: check if lb shrunk or ub grew
      bool lb_shrank = it->second.lb() < old_val.lb();
      bool ub_grew = it->second.ub() > old_val.ub();
      if (this->_global_iteration.load(std::memory_order_relaxed) >
              kWideningDelay &&
          (lb_shrank || ub_grew)) {
        // Delayed widening: after the delay, widen a still-unstable bound
        // to TOP. Preserve the existing interval's bit_width and sign:
        // collapsing to a 64-bit TOP would create a bit_width mismatch with
        // future callers (e.g. PBR flush_privatized publishes intervals
        // whose bit_width matches the LLVM type of the written global —
        // often 32 bits for `int`, 8 for `char`, etc.).
        it->second = machine_int::Interval::top(old_val.bit_width(),
                                                old_val.sign());
        return JoinResult::ForcedTop;
      }
      return JoinResult::Expanded;
    }
    return JoinResult::Unchanged;
  }

  /// \brief Internal helper: same semantics as `join_global` but assumes the
  /// caller already holds `_mutex`. Used by `flush_privatized` to avoid
  /// re-locking (and self-deadlocking on a non-recursive std::mutex) when
  /// it joins per-address values into the blackboard.
  ///
  /// MUST NOT be called from outside without holding `_mutex`.
  bool join_global_nolock(std::uint64_t addr,
                          const machine_int::Interval& value) {
    return this->join_into_cell_nolock(this->_global_board,
                                       addr,
                                       value) == JoinResult::Expanded;
  }

  /// \brief AstréeA: insert/join into a specific lock's partition
  /// (interval channel).
  ///
  /// Caller MUST already hold `_mutex`. Used by `flush_privatized` to
  /// drain per-lock PBR queues into the partition (the destination
  /// for "value at unlock time"). The widening discipline matches
  /// `join_global_nolock` via `join_into_cell_nolock`.
  bool join_partition_nolock(std::uint64_t lock_addr,
                             std::uint64_t addr,
                             const machine_int::Interval& value) {
    JoinResult res =
        this->join_into_cell_nolock(this->_lock_partition[lock_addr].intervals,
                                    addr,
                                    value);
    return res == JoinResult::Expanded;
  }

  // ---------------------------------------------------------------------
  // Protection-Based Reading (Goblint SAS'21) — privatized write registry
  // ---------------------------------------------------------------------

  /// \brief Record a deferred (lock-protected) global write.
  ///
  /// Called from NumericalExecutionEngine::exec_store when the write site
  /// holds at least one mutex. The mutex address is the KEY of the
  /// registry entry; the (addr, val) pair will be flushed on unlock.
  ///
  /// \param lock_addr  Address of the mutex held at the write site.
  /// \param addr       Address of the global being written.
  /// \param val        Local RHS interval (sound to over-approximate).
  void record_privatized_write(
      std::uint64_t lock_addr,
      std::uint64_t addr,
      const machine_int::Interval& val) {
    std::lock_guard< std::mutex > lock(_mutex);
    _privatized_writes[lock_addr].push_back(PrivatizedWrite{addr, val});
  }

  /// \brief Flush all privatized writes recorded against `lock_addr`.
  ///
  /// Atomically: groups writes by `addr`, joins their `val` intervals,
  /// and either:
  ///   (a) deposits into the lock's partition `_lock_partition[lock_addr]`
  ///       when `lock_addr != kNoLockPartitionAddr`, or
  ///   (b) publishes into the flat `_global_board` via
  ///       `join_global_nolock` when `lock_addr == kNoLockPartitionAddr`.
  /// In both cases the registry entry is cleared after aggregation.
  /// Returns true if any join actually expanded state (i.e., the
  /// publish exposed new interference to other threads).
  ///
  /// \param lock_addr  The mutex being released, or the sentinel
  ///                   `kNoLockPartitionAddr` for the unlocked-write
  ///                   partition (drained at worklist iteration boundaries).
  /// \return true iff the global fixpoint should re-iterate.
  ///
  /// \note `pthread_mutex_unlock` paths whose points-to resolution
  /// failed are guarded at the call site (`numerical.hpp:4318`) and
  /// do NOT call this method with `lock_addr == 0`; that guard is the
  /// load-bearing safety property that prevents the strict-flushing
  /// invariant from being violated by misresolved unlocks.
  /// \brief Over-approximation flush (soundness guard for TOP locksets).
  ///
  /// Drain EVERY non-empty `_privatized_writes` bucket as if the
  /// releasing thread held all of those locks. Sound exactly when the
  /// releasing thread's abstract lockset is TOP: the abstract set of
  /// held locks is then "any subset of the universe", so each
  /// non-sentinel bucket MAY be a real release point — publishing all
  /// of them is a strict over-approximation (a wrong bucket only
  /// widens its partition; the interval widening discipline bounds the
  /// growth). Never unsound-narrowing.
  ///
  /// Trigger (numerical.hpp unlock path): the lockset is TOP, under
  /// which the abstract `lockset_remove_lock` is a no-op and the
  /// single-lock flush cannot identify the released bucket — see the
  /// `04-mutex_23-sound_unlock.c` trace (unlock EXIT printed
  /// `lockset_is_top=YES` with a real mutex1 bucket left undrained:
  /// the FN root cause). Non-TOP locksets keep the precise single-lock
  /// flush and never enter this path.
  ///
  /// Implementation note: the per-bucket work is `flush_nolock` (the
  /// locked core of `flush_privatized`); re-entering the locking
  /// `flush_privatized` here would deadlock on the non-recursive
  /// `_mutex`.
  void flush_all_privatized_locks() {
    std::lock_guard< std::mutex > lock(_mutex);
    // Snapshot keys under the mutex: flushing mutates the map.
    std::vector< std::uint64_t > addrs;
    addrs.reserve(_privatized_writes.size());
    for (const auto& kv : _privatized_writes) {
      addrs.push_back(kv.first);
    }
    for (std::uint64_t lock_addr : addrs) {
      if (lock_addr == kNoLockPartitionAddr) {
        continue; // sentinel drains via flush_unlocked_partition only.
      }
      this->flush_nolock(lock_addr);
    }
  }

  bool flush_privatized(std::uint64_t lock_addr) {
    std::lock_guard< std::mutex > lock(_mutex);
    return this->flush_nolock(lock_addr);
  }

private:
  /// \brief Locked core of `flush_privatized`. Caller MUST hold
  /// `_mutex` (same discipline as join_*_nolock).
  ///
  /// State-Overwriting semantics: within ONE critical section the
  /// writes to the same global are strictly ordered by program order —
  /// the later write OVERWRITES the earlier one. The queue is appended
  /// in statement order, so folding it into a "latest-state map"
  /// (addr -> last write) and publishing ONLY the surviving entries is
  /// the flow-sensitive projection of the global at the release
  /// instant. The previous join_all aggregation was flow-INsensitive:
  /// it published the join of the whole history (`x=0 ⊔ x=1 = [0,1]`
  /// instead of the true `[1,1]`), letting ghost values pollute the
  /// lock partition and produce false races
  /// (13-privatized_03-priv_inv.c).
  bool flush_nolock(std::uint64_t lock_addr) {
    auto it = _privatized_writes.find(lock_addr);
    if (it == _privatized_writes.end() || it->second.empty()) {
      return false;
    }
    // Overwrite map: iterate in queue (statement) order; a later write
    // to the same address replaces the earlier record entirely.
    // (`operator[]` needs a default-constructible mapped type —
    // `machine_int::Interval` is not — hence find/assign.)
    std::unordered_map< std::uint64_t, machine_int::Interval > latest;
    for (const PrivatizedWrite& pw : it->second) {
      auto lit = latest.find(pw.addr);
      if (lit == latest.end()) {
        latest.emplace(pw.addr, pw.val);
      } else {
        lit->second = pw.val; // overwrite, never join
      }
    }
    // Clear the entry BEFORE publishing: avoids infinite recursion if a
    // partition join triggers a side-effect that re-enters this method
    // through the same mutex (defensive — should not happen but cheap).
    it->second.clear();

    bool changed = false;
    if (lock_addr == kNoLockPartitionAddr) {
      // Strict unlock-flushing path: drain the sentinel "no_lock"
      // bucket into the FLAT blackboard. join_global_nolock sets
      // `_is_dirty` if any cell expands.
      for (const auto& kv : latest) {
        bool c = this->join_global_nolock(kv.first, kv.second);
        changed = changed || c;
      }
    } else {
      // AstréeA-style: deposit into the lock's partition.
      // join_partition_nolock reuses the widening discipline of
      // join_global_nolock via join_into_cell_nolock. We already
      // hold `_mutex`, so the nolock variant is required —
      // re-acquiring a non-recursive std::mutex would deadlock.
      for (const auto& kv : latest) {
        bool c =
            this->join_partition_nolock(lock_addr, kv.first, kv.second);
        changed = changed || c;
      }
    }
    return changed;
  }

public:
  /// \brief Drain the sentinel "no_lock" partition (lock_addr == 0) into
  /// the flat `_global_board`. Strict unlock-flushing uses this to
  /// publish unlocked writes only at worklist iteration boundaries,
  /// not on every store (which would force per-store TOP widening).
  ///
  /// Called from the worklist loop boundary in
  /// `analysis/value/thread_modular.cpp`. Delegates to
  /// `flush_privatized(kNoLockPartitionAddr)`, which dispatches on
  /// the sentinel value to write into the flat board rather than a
  /// per-lock partition.
  ///
  /// \return true iff the global board expanded (sets `_is_dirty`).
  bool flush_unlocked_partition() {
    return this->flush_privatized(kNoLockPartitionAddr);
  }

  /// \brief Is `glob_addr` currently in the privatized cache for
  /// `lock_addr`? Used by exec_load to short-circuit read_global.
  ///
  /// \param lock_addr  The mutex currently held by the reading thread.
  /// \param glob_addr  The global being read.
  /// \return true iff some prior write to `glob_addr` under `lock_addr`
  ///         has not yet been flushed.
  bool is_privatized(std::uint64_t lock_addr, std::uint64_t glob_addr) const {
    std::lock_guard< std::mutex > lock(_mutex);
    auto it = _privatized_writes.find(lock_addr);
    if (it == _privatized_writes.end()) {
      return false;
    }
    for (const PrivatizedWrite& pw : it->second) {
      if (pw.addr == glob_addr) {
        return true;
      }
    }
    return false;
  }
  /// ```
  ///
  /// \param lock_addr  The lock whose registry to snapshot.
  /// \return A copy of the queue (empty if the lock has no pending
  ///         writes).
  std::vector< PrivatizedWrite > peek_privatized(std::uint64_t lock_addr) const {
    std::lock_guard< std::mutex > lock(_mutex);
    auto it = _privatized_writes.find(lock_addr);
    if (it == _privatized_writes.end()) {
      return {};
    }
    return it->second;
  }

  /// \brief Was any global write ever recorded against `lock_addr`?
  /// Useful for diagnostics; not on the hot path.
  bool has_privatized_writes(std::uint64_t lock_addr) const {
    std::lock_guard< std::mutex > lock(_mutex);
    auto it = _privatized_writes.find(lock_addr);
    return it != _privatized_writes.end() && !it->second.empty();
  }

  /// \brief Register a discovered thread function
  ///
  /// \param func Pointer to the thread function
  /// \param call_site The pthread_create statement (nullptr when unknown)
  void register_thread_func(ar::Function* func, ar::CallBase* call_site = nullptr) {
    std::lock_guard< std::mutex > lock(_mutex);
    bool is_new = true;
    for (ar::Function* existing : _discovered_thread_functions) {
      if (existing == func) {
        is_new = false;
        break;
      }
    }
    if (is_new) {
      _discovered_thread_functions.push_back(func);
      _is_dirty = true;
    }
    // Track the by-name spawn count: this feeds the Thread Uniqueness
    // abstraction (ESOP'23). The count is the number of DISTINCT static
    // pthread_create call sites, NOT the execution count — a fixpoint replay
    // re-executes the same call site without adding a second concurrent
    // instance, so an execution counter would read 3 for a single spawn and
    // fabricate self-race pairs (09-regions_02-list_nr.c). A call site whose
    // block can reach itself (a loop) may execute more than once, so it
    // counts as 2 — conservatively modeling concurrent instances
    // (46_monabsex2 / 48_ticket_lock: `while (...) pthread_create(...)`).
    if (func != nullptr && call_site != nullptr) {
      auto& sites = _spawn_sites_by_name[func->name()];
      if (sites.insert(call_site).second) {
        bool in_loop = false;
        if (ar::BasicBlock* bb = call_site->parent()) {
          std::unordered_set< ar::BasicBlock* > visited;
          std::vector< ar::BasicBlock* > stack;
          for (auto it = bb->successor_begin(), et = bb->successor_end();
               it != et; ++it) {
            stack.push_back(*it);
          }
          while (!stack.empty()) {
            ar::BasicBlock* cur = stack.back();
            stack.pop_back();
            if (cur == bb) {
              in_loop = true;
              break;
            }
            if (!visited.insert(cur).second) {
              continue;
            }
            for (auto it = cur->successor_begin(), et = cur->successor_end();
                 it != et; ++it) {
              stack.push_back(*it);
            }
          }
        }
        _spawn_counts_by_name[func->name()] += (in_loop ? 2u : 1u);
      }
    }
    // Thread-instance registry: assign this create site a stable id (once) and
    // record it as an instance of the entry function. Idempotent across
    // fixpoint replays (same site → same id; set insertion). NOT reset at
    // iteration boundaries — a static program property the checker reads after
    // convergence to decide "all instances of F joined" (28/29/30-join-array).
    if (func != nullptr && call_site != nullptr) {
      std::uint64_t sid = 0;
      auto sit = _site_ids.find(call_site);
      if (sit != _site_ids.end()) {
        sid = sit->second;
      } else {
        sid = _next_site_id++;
        _site_ids[call_site] = sid;
      }
      _func_instances[func->name()].insert(sid);
      _site_func[sid] = func;
    }
  }

  /// \brief Register a pthread_once callback and return its instance id.
  ///
  /// Unlike pthread_create (one instance per create site), a once-callback
  /// runs once PER DISTINCT once-control object. `key` is a stable identity of
  /// that control (definite) or of the call site (ambiguous control); two
  /// calls sharing a key share an instance, distinct keys are distinct
  /// instances (and make the callback non-unique, so its two runs race). When
  /// `joinable` is false (ambiguous control) the returned id is 0 and the
  /// caller must NOT join — the callback may run under a different control and
  /// so is not proven to happen-before this call's return.
  std::uint64_t register_once_thread(ar::Function* func, std::uint64_t key,
                                     bool joinable) {
    std::lock_guard< std::mutex > lock(_mutex);
    bool is_new = true;
    for (ar::Function* existing : _discovered_thread_functions) {
      if (existing == func) {
        is_new = false;
        break;
      }
    }
    if (is_new) {
      _discovered_thread_functions.push_back(func);
      _is_dirty = true;
    }
    std::uint64_t sid = 0;
    auto it = _once_sids.find(key);
    if (it != _once_sids.end()) {
      sid = it->second;
    } else {
      sid = _next_site_id++;
      _once_sids[key] = sid;
      _func_instances[func->name()].insert(sid);
      _site_func[sid] = func;
    }
    return joinable ? sid : 0;
  }

  /// \brief Record a pending conditional-lock fact for the FALSE branch of an
  /// `if (i) lock(m)` (idempotent — replays insert the same entry).
  void record_pending_cond_lock(ar::BasicBlock* false_branch,
                                std::uint64_t guard,
                                std::uint64_t lock_addr,
                                bool is_nonzero) {
    std::lock_guard< std::mutex > lock(_mutex);
    auto& v = _pending_cond_locks[false_branch];
    for (const auto& p : v) {
      if (p.guard == guard && p.lock_addr == lock_addr &&
          p.is_nonzero == is_nonzero) {
        return;
      }
    }
    v.push_back({guard, lock_addr, is_nonzero});
  }

  /// \brief The pending conditional-lock facts for a false-branch block, or
  /// nullptr when none.
  const std::vector< PendingCondLock >* pending_cond_locks(
      ar::BasicBlock* false_branch) const {
    std::lock_guard< std::mutex > lock(_mutex);
    auto it = _pending_cond_locks.find(false_branch);
    return (it == _pending_cond_locks.end()) ? nullptr : &it->second;
  }

  /// \brief Thread Uniqueness abstraction (ESOP'23): is the thread function
  /// `thread_id` statically known to be spawned at most once across the
  /// program?
  ///
  /// Returns `true` iff every reachable execution has exactly one instance of
  /// this thread function alive at any time. A function never spawned is
  /// conservatively considered unique (no races to model against itself).
  /// The entry function `"main"` is always unique (its lifetime is sequential
  /// from program start to end).
  ///
  /// \param thread_id The thread entry-function name (e.g., "thr1").
  /// \return true if unique, false if multiple concurrent instances may exist.
  bool is_thread_unique(const std::string& thread_id) const {
    if (thread_id.empty() || thread_id == "main") {
      // main() has exactly one instance per program; empty string means the
      // statement's containing function is unknown — conservative-no-race.
      return true;
    }
    std::lock_guard< std::mutex > lock(_mutex);
    auto it = _spawn_counts_by_name.find(thread_id);
    if (it == _spawn_counts_by_name.end()) {
      // Never spawned via pthread_create. It may still be a pthread_once
      // callback registered under 2+ DISTINCT controls, which runs once per
      // control and therefore races itself (87-once/07-different-onces.c).
      // _func_instances is a STATIC property (unlike _spawn_counts_by_name,
      // reset each iteration), so its size is the true distinct-instance count.
      auto fi = _func_instances.find(thread_id);
      if (fi != _func_instances.end()) {
        return fi->second.size() <= 1;
      }
      // Otherwise (e.g. a helper called only from main) there is no second
      // instance to race against → unique.
      return true;
    }
    // Count > 1 means multiple pthread_create call sites in the program text
    // — those could in principle execute concurrently (loop / recursion).
    // UNLESS they are SEQUENTIAL: each create site is joined before the next
    // create site, so at most one instance runs at a time
    // (10-synch_13-two_threads_nr.c: create t1; join t1; ...; create t2; join t2).
    if (it->second <= 1) {
      return true;
    }
    return this->spawns_are_sequential_locked(thread_id);
  }

  /// \brief True iff the thread function's create sites are all SEQUENTIAL —
  /// each create is joined before the next create. A single straight-line
  /// basic block whose creates are balance-interleaved with joins (at every
  /// create, the number of preceding joins ≥ the number of preceding creates).
  /// Caller must hold `_mutex`.
  bool spawns_are_sequential_locked(const std::string& name) const {
    auto ci = _sequential_cache.find(name);
    if (ci != _sequential_cache.end()) {
      return ci->second;
    }
    bool seq = false; // size<=1 (a loop'd create site) is concurrent, not unique
    auto si = _spawn_sites_by_name.find(name);
    if (si != _spawn_sites_by_name.end() && si->second.size() > 1) {
      seq = true; // multiple sites: assume sequential, verify below
      ar::BasicBlock* bb = nullptr;
      for (ar::CallBase* site : si->second) {
        if (site->parent() == nullptr) {
          seq = false;
          break;
        }
        if (bb == nullptr) {
          bb = site->parent();
        } else if (site->parent() != bb) {
          seq = false; // different blocks: conservatively concurrent
          break;
        }
      }
      if (seq) {
        unsigned creates = 0;
        unsigned joins = 0;
        for (ar::Statement* st : *bb) {
          auto* c = dyn_cast< ar::CallBase >(st);
          if (c == nullptr) {
            continue;
          }
          // Identify the create sites by the SITE SET (exec_pthread_create has
          // already resolved each site's thread function through bitcasts /
          // wrapper formals); re-resolving arg2 here would miss bitcast-wrapped
          // thread functions.
          if (si->second.count(c) != 0) {
            if (creates > joins) {
              seq = false; // previous create not joined → concurrent
              break;
            }
            ++creates;
            continue;
          }
          auto* fpc =
              dyn_cast_or_null< ar::FunctionPointerConstant >(c->called());
          if (fpc != nullptr && fpc->function() != nullptr &&
              fpc->function()->name() == "pthread_join") {
            ++joins;
          }
        }
      }
    }
    _sequential_cache[name] = seq;
    return seq;
  }

  /// \brief Get all discovered thread functions
  ///
  /// \return Vector of thread function pointers
  std::vector< ar::Function* > get_all_thread_functions() const {
    std::lock_guard< std::mutex > lock(_mutex);
    return _discovered_thread_functions;
  }

  /// \brief Reset the per-name spawn counters.
  ///
  /// The worklist driver re-analyzes the entry function every global
  /// iteration, and each replay re-executes pthread_create — with a
  /// naive `+1` the counter accumulates across iterations (t_fun
  /// spawned once reads as 3 after three iterations), poisoning the
  /// Thread Uniqueness abstraction and triggering spurious
  /// instance-duplication in DataRaceChecker. Reset at each iteration
  /// boundary: within ONE iteration the counter is the true static
  /// spawn count.
  void reset_spawn_counts() {
    std::lock_guard< std::mutex > lock(_mutex);
    for (auto& kv : _spawn_counts_by_name) {
      kv.second = 0;
    }
    _spawn_sites_by_name.clear();
    _sequential_cache.clear();
    // Cond-var global state is re-populated each iteration (wait/signal calls
    // replay); clear it here so it does not accumulate across the worklist
    // fixpoint and destabilise convergence.
    _cond_waiters.clear();
    _cond_signal_epoch.clear();
    _spawn_joined.clear();
  }

  /// \brief Is `fun` a thread entry function (a function that has been
  /// passed to pthread_create at some analysis point)?
  ///
  /// Used by the indirect-call resolver's FS+FI hybrid: thread entries
  /// are reachable concurrently with OTHER threads' stores, so their
  /// flow-sensitive view of global function pointers may miss stores
  /// performed by other threads (04-mutex_27-base_rc.c). Local helpers
  /// invoked from a single thread keep the pure flow-sensitive set.
  /// \brief Registry of pointer-typed variables whose value was LOADED
  /// from global memory. The indirect-call resolver's FS+FI hybrid
  /// only joins the FPA set for these: a helper's lock parameter passed
  /// by argument (thread-local precision) must NOT be widened, while a
  /// `g = load @f` may reflect stores performed by another thread.
  std::unordered_set< analyzer::Variable* > _glob_flown_vars;

  /// \brief Mark `v` as loaded from global memory (pointer-typed).
  void mark_glob_flown(analyzer::Variable* v) {
    if (v == nullptr) {
      return;
    }
    std::lock_guard< std::mutex > lock(_mutex);
    _glob_flown_vars.insert(v);
  }

  /// \brief Was `v` loaded from global memory? (see mark_glob_flown)
  bool is_glob_flown(analyzer::Variable* v) const {
    if (v == nullptr) {
      return false;
    }
    std::lock_guard< std::mutex > lock(_mutex);
    return _glob_flown_vars.count(v) != 0;
  }

  bool is_thread_entry(ar::Function* fun) const {
    if (fun == nullptr) {
      return false;
    }
    std::lock_guard< std::mutex > lock(_mutex);
    return std::find(_discovered_thread_functions.begin(),
                     _discovered_thread_functions.end(),
                     fun) != _discovered_thread_functions.end();
  }

  /// \brief Return the stable id of a pthread_create call site (thread
  /// instance). Ids are assigned lazily and cached; 0 means "no site" (the
  /// implicit main instance / an unresolved create). Caller must NOT hold
  /// `_mutex` (this method acquires it).
  std::uint64_t site_id(ar::CallBase* call_site) {
    if (call_site == nullptr) {
      return 0;
    }
    std::lock_guard< std::mutex > lock(_mutex);
    auto it = _site_ids.find(call_site);
    if (it != _site_ids.end()) {
      return it->second;
    }
    std::uint64_t sid = _next_site_id++;
    _site_ids[call_site] = sid;
    return sid;
  }

  /// \brief The set of create-site ids (instances) recorded for a thread
  /// entry function. Empty when the function was never spawned via a constant
  /// pthread_create (or is "main"). Consulted by the checker to decide "all
  /// instances of F joined".
  std::unordered_set< std::uint64_t > get_func_instances(
      const std::string& func_name) const {
    std::lock_guard< std::mutex > lock(_mutex);
    auto it = _func_instances.find(func_name);
    if (it == _func_instances.end()) {
      return {};
    }
    return it->second;
  }

  /// \brief The entry function of a create-site id (reverse of `site_id`).
  /// Returns nullptr when the id is unknown (e.g. 0 = main/unknown).
  ar::Function* site_func(std::uint64_t sid) const {
    std::lock_guard< std::mutex > lock(_mutex);
    auto it = _site_func.find(sid);
    if (it == _site_func.end()) {
      return nullptr;
    }
    return it->second;
  }

  /// \brief Store a thread function's MUST-join summary (the instances it
  /// definitely joined by exit). Written by the thread-modular driver after
  /// each function fixpoint; overwritten each iteration (converges).
  void set_join_summary(ar::Function* func,
                        const std::unordered_set< std::uint64_t >& joined) {
    if (func == nullptr) {
      return;
    }
    std::lock_guard< std::mutex > lock(_mutex);
    _join_summary[func] = joined;
  }

  /// \brief A thread function's MUST-join summary (transitively closed set of
  /// instances it definitely joined by exit). Empty when unknown / no joins.
  std::unordered_set< std::uint64_t > get_join_summary(
      ar::Function* func) const {
    if (func == nullptr) {
      return {};
    }
    std::lock_guard< std::mutex > lock(_mutex);
    auto it = _join_summary.find(func);
    if (it == _join_summary.end()) {
      return {};
    }
    return it->second;
  }

  /// \brief Register that a thread blocked on condition variable `cond`
  /// (pthread_cond_wait entry). Idempotent across replayed fixpoint visits is
  /// NOT required — the state is cleared each iteration boundary.
  void cond_wait_enter(std::uint64_t cond) {
    std::lock_guard< std::mutex > lock(_mutex);
    _cond_waiters[cond] += 1;
  }

  /// \brief Record a signal (wake ONE arbitrary waiter) or broadcast (wake ALL
  /// waiters) on condition variable `cond`. No precise wakee binding — a
  /// static analysis cannot know which waiter a signal unblocks, so the count
  /// is drained conservatively (signal drops one, broadcast drops all). This
  /// state is MAY and never used to suppress a race.
  void cond_signal(std::uint64_t cond, bool broadcast) {
    std::lock_guard< std::mutex > lock(_mutex);
    _cond_signal_epoch[cond] += 1;
    auto it = _cond_waiters.find(cond);
    if (it == _cond_waiters.end() || it->second == 0) {
      return;
    }
    it->second = broadcast ? 0 : it->second - 1;
  }

  /// \brief Deregister one waiter on `cond` (pthread_cond_wait return).
  void cond_wait_exit(std::uint64_t cond) {
    std::lock_guard< std::mutex > lock(_mutex);
    auto it = _cond_waiters.find(cond);
    if (it != _cond_waiters.end() && it->second > 0) {
      it->second -= 1;
    }
  }

  /// \brief Record the points-to set of the `arg` actual passed to a thread
  /// entry function at a pthread_create site.
  ///
  /// \param func The thread entry function.
  /// \param pts The points-to set of the 4th pthread_create argument.
  /// \param offset The byte offset of the argument pointer (essential for
  ///               `container_of`-style field recovery).
  void record_spawn_arg(ar::Function* func,
                        const PointsToSet< analyzer::MemoryLocation* >& pts,
                        const machine_int::Interval& offset) {
    if (func == nullptr || pts.is_bottom() || pts.is_top()) {
      return; // nothing meaningful to bind
    }
    std::lock_guard< std::mutex > lock(_mutex);
    auto it = _spawn_args.find(func);
    if (it == _spawn_args.end()) {
      _spawn_args.emplace(func, SpawnArg{pts, offset});
    } else {
      // Defensive: multiple spawns of the same function may pass DIFFERENT
      // args — the child may receive any of them, so union the sets AND the
      // offset intervals.
      it->second.pts.join_with(pts);
      it->second.offset.join_with(offset);
    }
    // Any cell the thread `arg` points to has ESCAPED the spawner's scope
    // (e.g. `&i` — a stack local now shared with the child).
    if (pts.is_set()) {
      for (auto* loc : pts) {
        _escaped_locs.insert(loc);
      }
    }
  }

  /// \brief Record the parent's MUST `joined` digest at a pthread_create point.
  ///
  /// Overwrite per (function, create site): the parent's fixpoint is monotone
  /// in the MUST lattice (joined facts only grow), so the last visit at a site
  /// is the converged value. Called on every visit; the map is cleared at each
  /// global-iteration boundary (reset_spawn_counts) so a stale early value
  /// never outlives the iteration.
  void record_spawn_joined(
      ar::Function* func,
      ar::CallBase* create_site,
      bool parent_top,
      const std::unordered_set< std::uint64_t >& parent_set) {
    if (func == nullptr || create_site == nullptr) {
      return;
    }
    std::lock_guard< std::mutex > lock(_mutex);
    _spawn_joined[func][create_site] = SpawnJoined{parent_top, parent_set};
  }

  /// \brief The child's inherited MUST `joined` digest: intersection over all
  /// of its spawn sites (a thread is "definitely joined before the child" only
  /// if the parent had joined it at EVERY site). Returns false when the
  /// function was never spawned at a resolved site (child inherits nothing).
  bool get_spawn_joined(ar::Function* func,
                        bool& top,
                        std::unordered_set< std::uint64_t >& set) const {
    std::lock_guard< std::mutex > lock(_mutex);
    auto it = _spawn_joined.find(func);
    if (it == _spawn_joined.end()) {
      return false;
    }
    bool first = true;
    top = true;
    set.clear();
    for (const auto& kv : it->second) {
      const SpawnJoined& sj = kv.second;
      if (first) {
        top = sj.top;
        set = sj.set;
        first = false;
        continue;
      }
      // MUST join (intersection, ⊤ absorbing) over sites.
      if (sj.top) {
        top = true;
        set.clear();
      } else if (!top) {
        for (auto jt = set.begin(); jt != set.end();) {
          if (sj.set.count(*jt) == 0) {
            jt = set.erase(jt);
          } else {
            ++jt;
          }
        }
      }
    }
    return true;
  }

  /// \brief Was `loc`'s address captured into a pthread_create `arg`?
  /// Consulted by DataRaceChecker to treat escaped stack locals as shared.
  bool is_escaped(analyzer::MemoryLocation* loc) const {
    if (loc == nullptr) {
      return false;
    }
    std::lock_guard< std::mutex > lock(_mutex);
    return _escaped_locs.count(loc) != 0;
  }

  /// \brief Widening threshold for the pointer blackboard channel: once a
  /// global pointer's may-points-to set exceeds this many distinct targets,
  /// the entry degrades to ⊤ (see `join_global_pointer`). ⊤ is the sound
  /// over-approximation for race detection — it can only ADD race
  /// candidates, never suppress one — and it bounds the per-key set size so
  /// `get_global_pointer` copies stay O(1).
  static constexpr std::size_t kPtrBoardWidenThreshold = 16;

  /// \brief Join the points-to set of a global pointer variable into the
  /// pointer blackboard channel. Called by the Store executor when it
  /// writes a pointer-typed value to a global.
  ///
  /// \param glob   The global pointer variable (board key).
  /// \param pts    The written value's may-points-to set.
  /// \param offset The written value's byte offset (offset-fidelity: must
  ///               ride along so readers can invert `container_of`).
  void join_global_pointer(analyzer::MemoryLocation* glob,
                           const PointsToSet< analyzer::MemoryLocation* >& pts,
                           const machine_int::Interval& offset) {
    // Soundness: a TOP write means "the global pointer may alias ANY cell".
    // Dropping it would silently convert "everything" into "nothing"
    // (under-approximation) and hide races. Only BOTTOM (no possible value)
    // may be skipped. The board already stores TOP entries (widen-to-top cap).
    if (glob == nullptr || pts.is_bottom()) {
      return;
    }
    std::lock_guard< std::mutex > lock(_mutex);
    auto r = _global_pointer_board.emplace(glob, GlobalPointer{pts, offset});
    auto it = r.first;
    if (r.second) {
      _is_dirty = true; // new entry: a later restore must see it
    } else if (!it->second.pts.is_top()) {
      std::size_t old_n = it->second.pts.is_set() ? it->second.pts.size() : 0;
      it->second.pts.join_with(pts);
      it->second.offset.join_with(offset);
      if (it->second.pts.is_set() && it->second.pts.size() > old_n) {
        _is_dirty = true; // expanded: keep iterating so a restore re-reads it
      }
    }
    // Widen-to-top cap: a may-set past the threshold is ⊤ (may alias any
    // cell). Sound for race detection (over-approximation), and prevents
    // the unbounded set growth from a loop-malloc'd / big pointer-array
    // global pointer.
    if (it->second.pts.is_set() &&
        it->second.pts.size() > kPtrBoardWidenThreshold) {
      it->second.pts.set_to_top();
    }
    // Maintain the reverse index: every target of a global pointer is
    // cross-thread reachable (a deref through it reads SHARED memory).
    if (it->second.pts.is_set()) {
      for (analyzer::MemoryLocation* tgt : it->second.pts) {
        _global_pointer_targets.insert(tgt);
        // A cell published through a GLOBAL pointer has ESCAPED its owning
        // thread's scope (e.g. `gp = &local` — a stack local now reachable by
        // any thread that derefs `gp`). Mark it escaped so
        // DataRaceChecker::touches_shared_memory treats a deref through the
        // global as a cross-thread access, not a thread-private local
        // (global-pointer-escape FN, gap #6). Mirrors record_spawn_arg.
        _escaped_locs.insert(tgt);
      }
    }
  }

  /// \brief Is `loc` a target of some global pointer recorded on the pointer
  /// blackboard (i.e. cross-thread SHARED, as opposed to a thread-private
  /// malloc)? O(1) set lookup against the reverse index.
  bool is_global_pointer_target(analyzer::MemoryLocation* loc) const {
    if (loc == nullptr) {
      return false;
    }
    std::lock_guard< std::mutex > lock(_mutex);
    return _global_pointer_targets.count(loc) != 0;
  }

  /// \brief Single-key read of the pointer blackboard channel.
  ///
  /// Copies only `loc`'s points-to set + offset (one key lookup + one small
  /// set copy) instead of snapshotting the whole board — the latter was O(K)
  /// per pointer deref and dominated deref-heavy programs.
  ///
  /// \param loc  The global pointer location (board key).
  /// \param[out] pts  Receives a copy of the recorded points-to set.
  /// \param[out] offset  Receives a copy of the recorded byte offset.
  /// \return true iff `loc` is present in the board.
  bool get_global_pointer(analyzer::MemoryLocation* loc,
                          PointsToSet< analyzer::MemoryLocation* >& pts,
                          machine_int::Interval& offset) const {
    if (loc == nullptr) {
      return false;
    }
    std::lock_guard< std::mutex > lock(_mutex);
    auto it = _global_pointer_board.find(loc);
    if (it == _global_pointer_board.end()) {
      return false;
    }
    pts = it->second.pts;
    offset = it->second.offset;
    return true;
  }

  /// \brief Join a heap-field pointer's points-to into the heap pointer board.
  /// Mirrors `join_global_pointer` but keyed by (alloc-site, field-offset).
  /// \param key    Packed (stable_id << 32 | field offset), see heap_pointer_key.
  /// \param pts    The written value's may-points-to set.
  /// \param offset The written value's byte offset (offset-fidelity for
  ///               container_of inversion on the reader side).
  void join_heap_pointer(std::uint64_t key,
                         const PointsToSet< analyzer::MemoryLocation* >& pts,
                         const machine_int::Interval& offset) {
    if (pts.is_top() || pts.is_bottom()) {
      return;
    }
    std::lock_guard< std::mutex > lock(_mutex);
    auto r = _heap_pointer_board.emplace(key, GlobalPointer{pts, offset});
    auto it = r.first;
    if (r.second) {
      _is_dirty = true; // new entry: a later restore must see it
    } else if (!it->second.pts.is_top()) {
      std::size_t old_n = it->second.pts.is_set() ? it->second.pts.size() : 0;
      it->second.pts.join_with(pts);
      it->second.offset.join_with(offset);
      if (it->second.pts.is_set() && it->second.pts.size() > old_n) {
        _is_dirty = true; // expanded: keep iterating so a restore re-reads it
      }
    }
    if (it->second.pts.is_set() &&
        it->second.pts.size() > kPtrBoardWidenThreshold) {
      it->second.pts.set_to_top();
    }
    // Heap-field targets are cross-thread reachable too (a deref through
    // `A->next` reads SHARED memory), so maintain the reverse index.
    if (it->second.pts.is_set()) {
      for (analyzer::MemoryLocation* tgt : it->second.pts) {
        _global_pointer_targets.insert(tgt);
      }
    }
  }

  /// \brief Single-key read of the heap pointer board (mirrors
  /// `get_global_pointer`). \return true iff the key is present.
  bool get_heap_pointer(std::uint64_t key,
                        PointsToSet< analyzer::MemoryLocation* >& pts,
                        machine_int::Interval& offset) const {
    std::lock_guard< std::mutex > lock(_mutex);
    auto it = _heap_pointer_board.find(key);
    if (it == _heap_pointer_board.end()) {
      return false;
    }
    pts = it->second.pts;
    offset = it->second.offset;
    return true;
  }

  /// \brief Retrieve the recorded spawn-arg points-to set + offset for a
  /// thread entry function. Returns false if none was recorded.
  ///
  /// \param func The thread entry function.
  /// \param[out] pts The joined points-to set of all recorded spawn args.
  /// \param[out] offset The joined byte offset of the recorded spawn args.
  /// \return true iff a spawn arg was recorded for `func`.
  bool get_spawn_arg(ar::Function* func,
                     PointsToSet< analyzer::MemoryLocation* >& pts,
                     machine_int::Interval& offset) const {
    std::lock_guard< std::mutex > lock(_mutex);
    auto it = _spawn_args.find(func);
    if (it == _spawn_args.end()) {
      return false;
    }
    pts = it->second.pts;
    offset = it->second.offset;
    return true;
  }

  /// \brief Was the blackboard mutated since the last `clear_dirty()`?
  ///
  /// Consulted by the global fixpoint driver
  /// (`analysis/value/interprocedural/sequential/analysis.cpp`) to decide
  /// whether the worklist must be re-analyzed for another round.
  bool is_dirty() const {
    std::lock_guard< std::mutex > lock(_mutex);
    return _is_dirty;
  }

  /// \brief Reset the dirty flag. Called at the start of each fixpoint
  /// iteration so that the next `is_dirty()` reflects only the work done
  /// during that iteration.
  void clear_dirty() {
    std::lock_guard< std::mutex > lock(_mutex);
    _is_dirty = false;
  }

  /// \brief Clear all state (for testing)
  void clear() {
    std::lock_guard< std::mutex > lock(_mutex);
    _global_board.clear();
    _privatized_writes.clear();
    _lock_partition.clear();
    _discovered_thread_functions.clear();
    _spawn_counts_by_name.clear();
    _spawn_sites_by_name.clear();
    _sequential_cache.clear();
    _pending_cond_locks.clear();
    _glob_flown_vars.clear();
    _site_ids.clear();
    _next_site_id = 1;
    _func_instances.clear();
    _site_func.clear();
    _once_sids.clear();
    _join_summary.clear();
    _spawn_args.clear();
    _escaped_locs.clear();
    _cond_waiters.clear();
    _cond_signal_epoch.clear();
    _global_pointer_board.clear();
    _global_pointer_targets.clear();
    _heap_pointer_board.clear();
    _lock_symbolic_index.clear();
    _is_dirty = false;
  }

  /// \brief Dump the global blackboard state
  void dump(std::ostream& o) const {
    std::lock_guard< std::mutex > lock(_mutex);
    o << "=== ConcurrentGlobalEnv ===" << std::endl;
    o << "Global Blackboard (" << _global_board.size() << " entries):"
      << std::endl;
    for (const auto& entry : _global_board) {
      o << "  0x" << std::hex << entry.first << std::dec << " -> "
        << entry.second << std::endl;
    }
    o << "Discovered Thread Functions ("
      << _discovered_thread_functions.size() << " entries):" << std::endl;
    for (ar::Function* func : _discovered_thread_functions) {
      o << "  " << (func ? func->name() : "(null)") << std::endl;
    }
    o << "Spawn Counts by Name (" << _spawn_counts_by_name.size()
      << " entries):" << std::endl;
    for (const auto& entry : _spawn_counts_by_name) {
      o << "  " << entry.first << " -> " << entry.second
        << (entry.second <= 1 ? "  [unique]" : "  [NOT unique]") << std::endl;
    }
    o << "Privatized Writes (" << _privatized_writes.size()
      << " lock buckets):" << std::endl;
    for (const auto& entry : _privatized_writes) {
      o << "  lock 0x" << std::hex << entry.first << std::dec
        << " -> " << entry.second.size() << " writes" << std::endl;
    }
    o << "Lock Partitions (" << _lock_partition.size()
      << " lock buckets):" << std::endl;
    for (const auto& entry : _lock_partition) {
      const LockPartition& P = entry.second;
      o << "  lock 0x" << std::hex << entry.first << std::dec << " -> "
        << P.intervals.size() << " globals" << std::endl;
      for (const auto& ventry : P.intervals) {
        o << "    glob 0x" << std::hex << ventry.first << std::dec
          << " interval " << ventry.second << std::endl;
      }
    }
  }
};

} // end namespace core
} // end namespace ikos
