/*******************************************************************************
 *
 * \file
 * \brief Single-Tier Must-Lockset abstract domain — tracks real mutexes only
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
 * OR FREEDOM FROM INFRINGEMENT, ANY WARRANTY THAT DOCUMENTATION, IF PROVIDED,
 * WILL CONFORM TO THE SUBJECT SOFTWARE. THIS AGREEMENT DOES NOT, IN ANY MANNER,
 * CONSTITUTE AN ENDORSEMENT BY GOVERNMENT AGENCY OR ANY PARTY TO THE RESULTS,
 * DESIGNING, MANUFACTURING, MARKETING, AND/OR DISTRIBUTING PRODUCTS, OR
 * ANY OTHER APPLICATIONS RESULTING FROM USE OF THE SUBJECT SOFTWARE.  RECIPIENT
 * SHALL INDEMNIFY AND HOLD HARMLESS THE UNITED STATES GOVERNMENT, ITS
 * CONTRACTORS AND SUBCONTRACTORS, AS WELL AS ANY PRIOR RECIPIENT, TO THE
 * EXTENT PERMITTED BY LAW.  RECIPIENT'S SOLE REMEDY FOR ANY SUCH MATTER SHALL
 * BE THE IMMEDIATE, UNILATERAL TERMINATION OF THIS AGREEMENT.
 *
 ******************************************************************************/

#pragma once

#include <ikos/core/domain/abstract_domain.hpp>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace ikos {
namespace core {
namespace lockset {

/// \brief Polarity of a conditional lock: under which value of its guard the
/// lock is held.
enum class CondPolarity {
  HELD_IF_ZERO,    ///< held iff the guard == 0 (trylock success path)
  HELD_IF_NONZERO, ///< held iff the guard != 0 (an `if (i) lock(m)` branch)
};

/// \brief A conditional lock: the mutex addr + the guard polarity. See
/// `LocksetDomain::_cond_locks`.
struct CondLock {
  std::uint64_t lock_addr;
  CondPolarity polarity;

  bool operator==(const CondLock& o) const {
    return lock_addr == o.lock_addr && polarity == o.polarity;
  }
  bool operator!=(const CondLock& o) const {
    return !(*this == o);
  }
};

/// \brief Must-Lockset abstract domain
///
/// Tracks the set of mutex addresses that MUST be held by every path reaching
/// the program point. Used by the race checker for happens-before inference
/// and by Protection-Based Reading (PBR, Goblint SAS'21) to decide whether
/// a write may be deferred until the lock is released.
///
/// Atomic / pseudo-lock primitives (e.g. `__VERIFIER_atomic_*`, `__sync_*`)
/// are intentionally NOT modelled here — the CPU atomicity guarantee is
/// semantic, not a real lock, and conflating the two masked real races.
///
/// Lattice:
///   - ⊥ (bottom, unreachable code): `_is_bottom` flag.
///   - ⊤ (top, no information): per-digest `_xxx_top` flag.
///   - ⊤-equivalent (concrete empty): `_xxx_top == false` with an empty set.
///   - Concrete: non-empty set of must-held addresses.
///
/// The three digests (`_mutex_*`, `_read_*`, `_joined_*`) each carry their own
/// top flag + value set. The old representation used a raw pointer per digest
/// (`nullptr` = ⊤, non-null empty = ⊤-equivalent) with a hand-written Rule of
/// Five; the three-state distinction is preserved here with an explicit
/// `_xxx_top` flag + a by-value `std::unordered_set`, so copy/move/destructor
/// are all `= default`.
class LocksetDomain final : public AbstractDomain< LocksetDomain > {
 private:
  using LockSet = std::unordered_set< std::uint64_t >;
  using ThreadSet = std::unordered_set< std::string >;
  /// Joined-threads digest now tracks thread INSTANCES (create-site ids, a
  /// deterministic uint64) rather than entry-function names — joining `tids[0]`
  /// must mark only that instance, not every array element that maps to the
  /// same function name (28/29/30-join-array FN). The spawned digest stays
  /// name-keyed (MAY, create-edge HB only needs "spawned function F at all").
  using JoinedSet = std::unordered_set< std::uint64_t >;
  /// Conditional lock: result-variable index -> (lock addr, polarity) (trylock
  /// result guard + `if (i) lock` branch guard).
  using CondLockMap = std::unordered_map< std::uint64_t, CondLock >;

  /// True iff this value is ⊥ (unreachable code). Always propagates.
  bool _is_bottom = false;

  /// Mutex digest: `_mutex_top` = ⊤ ("no info", the old `nullptr`); when
  /// false, `_mutex_locks` maps each must-held mutex address to its recursive
  /// depth (number of times held). A recursive mutex locked twice must survive
  /// the first unlock (71-doublelocking_14-rec-dyn-no-race.c).
  bool _mutex_top = true;
  std::unordered_map< std::uint64_t, unsigned > _mutex_locks;

  /// Read-lock (shared rwlock) digest: same lattice as the mutex digest but a
  /// distinct tier — a shared read lock does NOT mutually exclude another read
  /// lock on the same rwlock, whereas the write locks in the mutex digest do.
  bool _read_top = true;
  LockSet _read_locks;

  /// Joined-threads digest (happens-before / create-edges).
  bool _joined_top = true;
  JoinedSet _joined_threads;

  /// Spawned-threads digest (create-edge happens-before): the set of threads
  /// this thread has POSSIBLY spawned (MAY semantics, join = union). Separate
  /// from `_joined_threads` (MUST, join = intersection) because the two need
  /// opposite lattices: a joined thread is a must-fact ("definitely joined"),
  /// while a spawned thread used to prove "access precedes its own create"
  /// must be a may-fact — a MUST spawn set is emptied at a conditional-create
  /// merge and would unsoundly claim the parent never spawned the child
  /// (race-1_3b-join.c FN). Empty = "no thread possibly spawned" (entry state).
  ThreadSet _spawned_threads;

  /// Spawned-thread INSTANCE ids (create-site ids), mirroring
  /// `_spawned_threads` (MAY, join = union). Used for the instance-aware
  /// create/join HB: a parent access is ordered before an instance it has NOT
  /// spawned yet (10-synch_13: main joins t1 then writes =2 before creating
  /// t2 — the name-level digest alone cannot tell t2 apart from t1).
  std::unordered_set< std::uint64_t > _spawned_instances;

  /// \brief Heap nodes (DynAlloc stable ids) that are provably FRESH — a
  /// malloc result whose pointer has NOT yet been stored into a cross-thread
  /// reachable location. MUST digest (join = intersection, ⊤ = ∅): a node is
  /// fresh only if fresh on every incoming edge. A fresh node is thread-private
  /// (only the allocating thread holds its pointer), so its accesses cannot
  /// race (goblint region-domain "fresh bullet" soundness argument). A node
  /// leaves the set once this thread stores its pointer into a global / thread
  /// arg / non-fresh heap field.
  std::unordered_set< std::uint64_t > _fresh_heap_nodes;

  /// Spawned-threads digest TOP flag: "may have spawned ANY thread" (the
  /// universe, unrepresentable as a finite set). Set when an indirect
  /// pthread_create (non-constant thread function) is encountered, so the
  /// create-edge HB check cannot falsely conclude "the parent never spawned
  /// the child" and unsoundly suppress a race. Default false = ∅ ("spawned
  /// nothing yet", the entry state). Note the MAY-lattice polarity: ⊤ is
  /// LEAST constrained here (any thread may be the child), opposite of the
  /// MUST digests where ⊤ = ∅.
  bool _spawned_top = false;

  /// Conditional-lock digest (trylock): maps the trylock RESULT variable's
  /// index to the mutex addr that is held IFF that result is 0. A conditional
  /// lock is NOT a must-held lock: the checker resolves it at the access site
  /// via the interval domain (held iff guard interval == {0}). Same
  /// three-state lattice as the mutex digest (join = key-intersection,
  /// meet = key-union, ⊤ = empty).
  bool _cond_top = true;
  CondLockMap _cond_locks;

  /// Condition-variable SIGNALED digest (MAY, join = union): the set of cond
  /// addresses this thread has POSSIBLY signaled at or before this program
  /// point. Mirror of `_spawned_threads` — a signal is a may-fact ("this
  /// thread may have woken SOME waiter"), so join must be union; intersection
  /// would drop a signal at a conditional merge and (if ever consumed) could
  /// unsoundly claim the signal never happened. NOT consumed by the race
  /// checker (the signal→wait edge is a MAY edge — spurious wakeup / unknown
  /// wakee — so it cannot soundly suppress a race); kept as observable
  /// analysis state (dumped with `--emit-concurrency-invariants`).
  bool _signaled_top = false;
  std::unordered_set< std::uint64_t > _signaled_conds;

  /// Condition-variable AWAITED digest (MAY, join = union): cond addresses
  /// this thread has POSSIBLY waited on. Same lattice as `_signaled_conds`.
  bool _awaited_top = false;
  std::unordered_set< std::uint64_t > _awaited_conds;

  /// \brief Join a single digest in place: SET INTERSECTION.
  ///
  /// Lattice identities: join(⊤, x) = join(x, ⊤) = ⊤ (intersection with
  /// unknown = unknown).
  template < typename Set >
  static void join_digest(bool& top,
                          Set& set,
                          bool other_top,
                          const Set& other) {
    if (other_top) {
      top = true;
      set.clear();
      return;
    }
    if (top) {
      return; // already ⊤
    }
    for (auto it = set.begin(); it != set.end();) {
      if (other.count(*it) == 0) {
        it = set.erase(it);
      } else {
        ++it;
      }
    }
  }

  /// \brief Meet a single digest in place: SET UNION.
  ///
  /// Lattice identities (Must-Lattice, reverse-subset order where meet = ∪
  /// and ⊤ = ∅): meet(x, ⊤) = x and meet(⊤, x) = x — ⊤ is the IDENTITY
  /// element of union, not an absorbing element.
  template < typename Set >
  static void meet_digest(bool& top,
                          Set& set,
                          bool other_top,
                          const Set& other) {
    if (other_top) {
      return; // meet(x, ⊤) = x (identity)
    }
    if (top) {
      top = false;
      set = other; // meet(⊤, x) = x (adopt other's concrete state)
      return;
    }
    set.insert(other.begin(), other.end()); // union
  }

  /// \brief Join a recursive-mutex digest in place: intersection with MIN depth.
  ///
  /// A recursive mutex's depth is the number of times it is definitely held.
  /// On a merge a lock is definitely held min(depth_a, depth_b) times; a lock
  /// missing from either side is not definitely held at all (dropped).
  static void join_mutex(bool& top,
                         std::unordered_map< std::uint64_t, unsigned >& map,
                         bool other_top,
                         const std::unordered_map< std::uint64_t, unsigned >&
                             other) {
    if (other_top) {
      top = true;
      map.clear();
      return;
    }
    if (top) {
      return; // already ⊤
    }
    for (auto it = map.begin(); it != map.end();) {
      auto oit = other.find(it->first);
      if (oit == other.end()) {
        it = map.erase(it);
      } else {
        it->second = std::min(it->second, oit->second);
        ++it;
      }
    }
  }

  /// \brief Meet a recursive-mutex digest in place: union with MAX depth.
  static void meet_mutex(bool& top,
                         std::unordered_map< std::uint64_t, unsigned >& map,
                         bool other_top,
                         const std::unordered_map< std::uint64_t, unsigned >&
                             other) {
    if (other_top) {
      return; // meet(x, ⊤) = x (identity)
    }
    if (top) {
      top = false;
      map = other;
      return;
    }
    for (const auto& kv : other) {
      auto it = map.find(kv.first);
      if (it == map.end()) {
        map[kv.first] = kv.second;
      } else {
        it->second = std::max(it->second, kv.second);
      }
    }
  }

  /// \brief Lattice equality of a recursive-mutex digest (depth-sensitive).
  static bool eq_mutex(
      bool top,
      const std::unordered_map< std::uint64_t, unsigned >& map,
      bool other_top,
      const std::unordered_map< std::uint64_t, unsigned >& other) {
    bool this_empty = top || map.empty();
    bool other_empty = other_top || other.empty();
    if (this_empty || other_empty) {
      return this_empty && other_empty;
    }
    return map == other;
  }

  /// \brief Forward-subset test of a recursive-mutex digest (this ⊆ other),
  /// depth-sensitive: every lock in `this` is in `other` at a depth no greater
  /// (a shallower definitely-held count is a stronger claim).
  static bool leq_mutex(
      bool top,
      const std::unordered_map< std::uint64_t, unsigned >& map,
      bool other_top,
      const std::unordered_map< std::uint64_t, unsigned >& other) {
    if (top) {
      return true;
    }
    if (other_top) {
      return false;
    }
    for (const auto& kv : map) {
      auto oit = other.find(kv.first);
      if (oit == other.end() || kv.second > oit->second) {
        return false;
      }
    }
    return true;
  }

  /// \brief Lattice equality of a single digest (⊤ and empty-set are equal).
  template < typename Set >
  static bool eq_digest(bool top,
                        const Set& set,
                        bool other_top,
                        const Set& other) {
    bool this_empty = top || set.empty();
    bool other_empty = other_top || other.empty();
    if (this_empty || other_empty) {
      return this_empty && other_empty;
    }
    return set == other;
  }

  /// \brief Forward-subset test of a single digest (this ⊆ other), mirroring
  /// the historical `leq` semantics.
  template < typename Set >
  static bool leq_digest(bool top,
                         const Set& set,
                         bool other_top,
                         const Set& other) {
    if (top) {
      return true; // this was ⊤ (nullptr): no constraint
    }
    if (other_top) {
      return false; // other was ⊤ (nullptr)
    }
    for (const auto& e : set) {
      if (other.count(e) == 0) {
        return false;
      }
    }
    return true;
  }

  /// \brief Join (∪) for a MAY digest, with the ⊤ flag.
  /// join(x, ⊤) = ⊤ (union with the universe absorbs everything).
  template < typename Set >
  static void join_spawned(bool& top,
                           Set& set,
                           bool other_top,
                           const Set& other) {
    if (other_top) {
      top = true;
      set.clear();
      return;
    }
    if (top) {
      return; // already ⊤
    }
    set.insert(other.begin(), other.end()); // union
  }

  /// \brief Meet (∩) for a MAY digest, with the ⊤ flag.
  /// meet(x, ⊤) = x and meet(⊤, x) = x (⊤ is the identity of ∩).
  template < typename Set >
  static void meet_spawned(bool& top,
                           Set& set,
                           bool other_top,
                           const Set& other) {
    if (other_top) {
      return; // meet(x, ⊤) = x
    }
    if (top) {
      top = false;
      set = other; // meet(⊤, x) = x
      return;
    }
    for (auto it = set.begin(); it != set.end();) {
      if (other.count(*it) == 0) {
        it = set.erase(it);
      } else {
        ++it;
      }
    }
  }

  /// \brief Equality for a MAY digest (⊤ ≠ ∅ ≠ a concrete set).
  template < typename Set >
  static bool spawned_eq(bool top,
                         const Set& a,
                         bool other_top,
                         const Set& b) {
    if (top != other_top) {
      return false;
    }
    if (top) {
      return true; // both ⊤
    }
    return a == b;
  }

  /// \brief Forward-subset test for a MAY digest, with ⊤.
  /// Lattice order: ∅ ⊑ any set ⊑ ⊤ (fewer possibly-spawned threads = more
  /// precise = leq).
  template < typename Set >
  static bool spawned_leq(bool top,
                          const Set& a,
                          bool other_top,
                          const Set& b) {
    if (top) {
      return other_top; // ⊤ ⊑ x only if x = ⊤
    }
    if (other_top) {
      return true; // any x ⊑ ⊤
    }
    for (const auto& e : a) {
      if (b.count(e) == 0) {
        return false;
      }
    }
    return true;
  }

  /// \brief Join the conditional-lock digest in place: key-INTERSECTION.
  ///
  /// A conditional lock is a MUST-conditional fact ("m held iff guard==p"), so
  /// it joins exactly like the mutex digest: a fact survives the CFG join only
  /// if it holds on every incoming edge (same guard → same lock+polarity).
  /// Union would leak a branch-local fact ("if (g) lock(m)") onto a sibling
  /// path that never took that lock — unsound for a must-set. The checker then
  /// resolves a surviving fact to a must-held lock only when the guard's
  /// interval is a singleton matching the polarity (sound direction).
  static void join_cond(bool& top,
                        CondLockMap& map,
                        bool other_top,
                        const CondLockMap& other) {
    if (other_top) {
      top = true;
      map.clear(); // join(x, ⊤) = ⊤ (intersection with unknown = unknown)
      return;
    }
    if (top) {
      return; // already ⊤
    }
    for (auto it = map.begin(); it != map.end();) {
      auto o = other.find(it->first);
      if (o == other.end() || o->second != it->second) {
        it = map.erase(it); // fact absent (or different) on the other edge
      } else {
        ++it;
      }
    }
  }

  /// \brief Meet the conditional-lock digest in place: key-union.
  ///
  /// ⊤ is the identity (meet(x, ⊤) = meet(⊤, x) = x).
  static void meet_cond(bool& top,
                        CondLockMap& map,
                        bool other_top,
                        const CondLockMap& other) {
    if (other_top) {
      return;
    }
    if (top) {
      top = false;
      map = other;
      return;
    }
    for (const auto& kv : other) {
      map[kv.first] = kv.second;
    }
  }

  /// \brief Lattice equality of the conditional-lock digest.
  static bool eq_cond(bool top,
                      const CondLockMap& map,
                      bool other_top,
                      const CondLockMap& other) {
    bool this_empty = top || map.empty();
    bool other_empty = other_top || other.empty();
    if (this_empty || other_empty) {
      return this_empty && other_empty;
    }
    return map == other;
  }

  /// \brief Forward-subset test of the conditional-lock digest.
  static bool leq_cond(bool top,
                       const CondLockMap& map,
                       bool other_top,
                       const CondLockMap& other) {
    if (top) {
      return true;
    }
    if (other_top) {
      return false;
    }
    for (const auto& kv : map) {
      auto o = other.find(kv.first);
      if (o == other.end() || o->second != kv.second) {
        return false;
      }
    }
    return true;
  }

  /// \brief Private default constructor (member initializers yield ⊤).
  ///
  /// Kept private so `std::is_default_constructible< LocksetDomain >` stays
  /// false — `AbstractDomain` asserts derived domains are NOT default
  /// constructible and must be created via `top()` / `bottom()`.
  LocksetDomain() = default;

 public:
  /// \brief Create ⊤ (top: no mutexes must-held)
  static LocksetDomain top() { return LocksetDomain(); }

  /// \brief Create ⊥ (bottom: unreachable / contradictory)
  static LocksetDomain bottom() {
    LocksetDomain d;
    d._is_bottom = true;
    return d;
  }

  /// \brief Rule of Five — all defaulted: every member is a value type, so
  /// copy/move/destructor are auto-derived (no manual `new`/`delete`).
  ~LocksetDomain() override = default;
  LocksetDomain(const LocksetDomain&) = default;
  LocksetDomain(LocksetDomain&&) = default;
  LocksetDomain& operator=(const LocksetDomain&) = default;
  LocksetDomain& operator=(LocksetDomain&&) = default;

  // ─ ─── Core abstract domain interface ───────────────────────────────

  void normalize() override {}

  bool is_bottom() const override { return _is_bottom; }

  bool is_top() const override {
    if (_is_bottom) {
      return false;
    }
    return (_mutex_top || _mutex_locks.empty()) &&
           (_read_top || _read_locks.empty()) &&
           (_joined_top || _joined_threads.empty()) &&
           (!_spawned_top && _spawned_threads.empty()) &&
           (!_signaled_top && _signaled_conds.empty()) &&
           (!_awaited_top && _awaited_conds.empty()) &&
           (_cond_top || _cond_locks.empty());
  }

  void set_to_bottom() override {
    _is_bottom = true;
    _mutex_top = _read_top = _joined_top = _cond_top = true;
    _spawned_top = false;
    _signaled_top = _awaited_top = false;
    _mutex_locks.clear();
    _read_locks.clear();
    _joined_threads.clear();
    _spawned_threads.clear();
    _spawned_instances.clear();
    _fresh_heap_nodes.clear();
    _signaled_conds.clear();
    _awaited_conds.clear();
    _cond_locks.clear();
  }

  void set_to_top() override {
    _is_bottom = false;
    _mutex_top = _read_top = _joined_top = _cond_top = true;
    _spawned_top = false;
    _signaled_top = _awaited_top = false;
    _mutex_locks.clear();
    _read_locks.clear();
    _joined_threads.clear();
    _spawned_threads.clear();
    _spawned_instances.clear();
    _fresh_heap_nodes.clear();
    _signaled_conds.clear();
    _awaited_conds.clear();
    _cond_locks.clear();
  }

  /// \brief Must-Lockset join: SET INTERSECTION on EACH digest independently.
  void join_with(const LocksetDomain& other) override {
    if (other._is_bottom) {
      return; // join(⊥, x) = x
    }
    if (this->_is_bottom) {
      *this = other;
      return;
    }
    join_mutex(_mutex_top,
               _mutex_locks,
               other._mutex_top,
               other._mutex_locks);
    join_digest(_read_top, _read_locks, other._read_top, other._read_locks);
    join_digest(_joined_top,
                _joined_threads,
                other._joined_top,
                other._joined_threads);
    // Spawned digest is MAY: join = union (a thread spawned on either
    // incoming edge is "possibly spawned" after the merge), with ⊤ flag.
    join_spawned(_spawned_top,
                 _spawned_threads,
                 other._spawned_top,
                 other._spawned_threads);
    if (_spawned_top) {
      _spawned_instances.clear();
    } else {
      _spawned_instances.insert(other._spawned_instances.begin(),
                                other._spawned_instances.end());
    }
    // Fresh heap nodes: MUST, join = intersection (fresh on both edges).
    for (auto it = _fresh_heap_nodes.begin();
         it != _fresh_heap_nodes.end();) {
      if (other._fresh_heap_nodes.count(*it) == 0) {
        it = _fresh_heap_nodes.erase(it);
      } else {
        ++it;
      }
    }
    // Cond-var digests are MAY: join = union (a signal/wait on either edge is
    // "possibly happened" after the merge).
    join_spawned(_signaled_top,
                 _signaled_conds,
                 other._signaled_top,
                 other._signaled_conds);
    join_spawned(_awaited_top,
                 _awaited_conds,
                 other._awaited_top,
                 other._awaited_conds);
    join_cond(_cond_top, _cond_locks, other._cond_top, other._cond_locks);
  }

  void widen_with(const LocksetDomain& other) override {
    this->join_with(other);
  }

  /// \brief Must-Lockset meet: SET UNION on EACH digest independently.
  ///
  /// Component-wise independence: each digest is processed on its own merits.
  /// ⊤ is the identity of union, so meeting a digest with ⊤ leaves that
  /// digest unchanged (meet(x, ⊤) = meet(⊤, x) = x).
  void meet_with(const LocksetDomain& other) override {
    if (other._is_bottom) {
      this->set_to_bottom();
      return;
    }
    if (this->_is_bottom) {
      return;
    }
    meet_mutex(_mutex_top,
               _mutex_locks,
               other._mutex_top,
               other._mutex_locks);
    meet_digest(_read_top, _read_locks, other._read_top, other._read_locks);
    meet_digest(_joined_top,
                _joined_threads,
                other._joined_top,
                other._joined_threads);
    // Spawned digest is MAY: meet = intersection (refinement shrinks the
    // possibly-spawned set), with ⊤ flag.
    meet_spawned(_spawned_top,
                 _spawned_threads,
                 other._spawned_top,
                 other._spawned_threads);
    if (!_spawned_top) {
      for (auto it = _spawned_instances.begin();
           it != _spawned_instances.end();) {
        if (other._spawned_instances.count(*it) == 0) {
          it = _spawned_instances.erase(it);
        } else {
          ++it;
        }
      }
    }
    // Fresh heap nodes: MUST, meet = union (refinement adds freshness).
    _fresh_heap_nodes.insert(other._fresh_heap_nodes.begin(),
                             other._fresh_heap_nodes.end());
    meet_spawned(_signaled_top,
                 _signaled_conds,
                 other._signaled_top,
                 other._signaled_conds);
    meet_spawned(_awaited_top,
                 _awaited_conds,
                 other._awaited_top,
                 other._awaited_conds);
    meet_cond(_cond_top, _cond_locks, other._cond_top, other._cond_locks);
  }

  void narrow_with(const LocksetDomain& other) override {
    this->meet_with(other);
  }

  bool leq(const LocksetDomain& other) const override {
    if (this->_is_bottom) {
      return true;
    }
    if (other._is_bottom) {
      return false;
    }
    return leq_mutex(_mutex_top,
                     _mutex_locks,
                     other._mutex_top,
                     other._mutex_locks) &&
           leq_digest(_read_top,
                      _read_locks,
                      other._read_top,
                      other._read_locks) &&
           leq_digest(_joined_top,
                      _joined_threads,
                      other._joined_top,
                      other._joined_threads) &&
           spawned_leq(_spawned_top,
                       _spawned_threads,
                       other._spawned_top,
                       other._spawned_threads) &&
           spawned_leq(_spawned_top,
                       _spawned_instances,
                       other._spawned_top,
                       other._spawned_instances) &&
           leq_digest(false,
                      _fresh_heap_nodes,
                      false,
                      other._fresh_heap_nodes) &&
           spawned_leq(_signaled_top,
                       _signaled_conds,
                       other._signaled_top,
                       other._signaled_conds) &&
           spawned_leq(_awaited_top,
                       _awaited_conds,
                       other._awaited_top,
                       other._awaited_conds) &&
           leq_cond(_cond_top, _cond_locks, other._cond_top, other._cond_locks);
  }

  bool equals(const LocksetDomain& other) const override {
    if (this->_is_bottom != other._is_bottom) {
      return false;
    }
    if (this->_is_bottom) {
      return true;
    }
    return eq_mutex(_mutex_top,
                    _mutex_locks,
                    other._mutex_top,
                    other._mutex_locks) &&
           eq_digest(_read_top,
                     _read_locks,
                     other._read_top,
                     other._read_locks) &&
           eq_digest(_joined_top,
                     _joined_threads,
                     other._joined_top,
                     other._joined_threads) &&
           spawned_eq(_spawned_top,
                      _spawned_threads,
                      other._spawned_top,
                      other._spawned_threads) &&
           spawned_eq(_spawned_top,
                      _spawned_instances,
                      other._spawned_top,
                      other._spawned_instances) &&
           eq_digest(false,
                     _fresh_heap_nodes,
                     false,
                     other._fresh_heap_nodes) &&
           spawned_eq(_signaled_top,
                      _signaled_conds,
                      other._signaled_top,
                      other._signaled_conds) &&
           spawned_eq(_awaited_top,
                      _awaited_conds,
                      other._awaited_top,
                      other._awaited_conds) &&
           eq_cond(_cond_top, _cond_locks, other._cond_top, other._cond_locks);
  }

  void dump(std::ostream& o) const override {
    if (_is_bottom) {
      o << "⊥";
      return;
    }
    if (is_top()) {
      o << "⊤";
      return;
    }
    o << "Mutex{";
    bool first = true;
    for (const auto& kv : _mutex_locks) {
      if (!first) {
        o << ",";
      }
      o << "0x" << std::hex << kv.first << std::dec;
      if (kv.second > 1) {
        o << "x" << kv.second;
      }
      first = false;
    }
    o << "} Read{";
    first = true;
    for (std::uint64_t lock : _read_locks) {
      if (!first) {
        o << ",";
      }
      o << "0x" << std::hex << lock << std::dec;
      first = false;
    }
    o << "} Joined{";
    first = true;
    for (std::uint64_t sid : _joined_threads) {
      if (!first) {
        o << ",";
      }
      o << sid;
      first = false;
    }
    o << "} Spawned{";
    first = true;
    if (_spawned_top) {
      o << "⊤";
      first = false;
    }
    for (const std::string& t : _spawned_threads) {
      if (!first) {
        o << ",";
      }
      o << t;
      first = false;
    }
    o << "} Signaled{";
    first = true;
    for (std::uint64_t c : _signaled_conds) {
      if (!first) {
        o << ",";
      }
      o << "0x" << std::hex << c << std::dec;
      first = false;
    }
    o << "} Awaited{";
    first = true;
    for (std::uint64_t c : _awaited_conds) {
      if (!first) {
        o << ",";
      }
      o << "0x" << std::hex << c << std::dec;
      first = false;
    }
    o << "} Cond{";
    first = true;
    for (const auto& kv : _cond_locks) {
      if (!first) {
        o << ",";
      }
      o << "g" << kv.first << "->0x" << std::hex << kv.second.lock_addr
        << (kv.second.polarity == CondPolarity::HELD_IF_ZERO ? "" : "!") << std::dec;
      first = false;
    }
    o << "}";
  }

  static std::string name() { return "lockset domain"; }

  // ─ ─── Single-tier (Mutex) operations — the ONLY tier ──────────────

  /// \brief Record that a mutex is held (increment recursive depth).
  void add_mutex(std::uint64_t lock_addr) {
    if (_is_bottom) {
      return;
    }
    _mutex_top = false;
    ++_mutex_locks[lock_addr];
  }

  /// \brief Release one layer of a mutex (recursive depth decrements; only a
  /// depth reaching 0 drops the lock).
  void remove_mutex(std::uint64_t lock_addr) {
    if (_is_bottom || _mutex_top) {
      return;
    }
    auto it = _mutex_locks.find(lock_addr);
    if (it == _mutex_locks.end()) {
      return;
    }
    if (--it->second == 0) {
      _mutex_locks.erase(it);
    }
  }

  /// \brief Check if a mutex is must-held.
  bool holds_mutex(std::uint64_t lock_addr) const {
    if (_is_bottom) {
      return true;
    }
    return !_mutex_top && _mutex_locks.count(lock_addr) > 0;
  }

  /// \brief Snapshot of must-held mutexes.
  std::unordered_set< std::uint64_t > held_mutexes() const {
    if (_is_bottom || _mutex_top) {
      return {};
    }
    std::unordered_set< std::uint64_t > out;
    out.reserve(_mutex_locks.size());
    for (const auto& kv : _mutex_locks) {
      out.insert(kv.first);
    }
    return out;
  }

  /// \brief Snapshot of all held locks (= mutexes; the domain is single-tier).
  std::unordered_set< std::uint64_t > held_locks() const {
    return this->held_mutexes();
  }

  // ─ ─── Read-lock (shared rwlock) operations ───────────────────────

  /// \brief Record that a shared read lock (pthread_rwlock_rdlock) is held.
  void add_read_lock(std::uint64_t lock_addr) {
    if (_is_bottom) {
      return;
    }
    _read_top = false;
    _read_locks.insert(lock_addr);
  }

  /// \brief Release a shared read lock.
  void remove_read_lock(std::uint64_t lock_addr) {
    if (_is_bottom || _read_top) {
      return;
    }
    _read_locks.erase(lock_addr);
  }

  /// \brief Check if a shared read lock is must-held.
  bool holds_read_lock(std::uint64_t lock_addr) const {
    if (_is_bottom) {
      return true;
    }
    return !_read_top && _read_locks.count(lock_addr) > 0;
  }

  /// \brief Snapshot of must-held shared read locks.
  std::unordered_set< std::uint64_t > held_read_locks() const {
    if (_is_bottom || _read_top) {
      return {};
    }
    return _read_locks;
  }

  // ─ ─── Conditional-lock (trylock / branch) operations ─────────────

  /// \brief Record a conditional lock: guard (result-var / branch-cond index)
  /// -> (mutex addr, polarity). Resolved by the checker via the guard's
  /// interval against the polarity.
  void add_cond_lock(std::uint64_t guard_idx,
                     std::uint64_t lock_addr,
                     CondPolarity polarity = CondPolarity::HELD_IF_ZERO) {
    if (_is_bottom) {
      return;
    }
    _cond_top = false;
    _cond_locks[guard_idx] = {lock_addr, polarity};
  }

  /// \brief Drop every conditional lock for `lock_addr` (unlock).
  void remove_cond_lock(std::uint64_t lock_addr) {
    if (_is_bottom || _cond_top) {
      return;
    }
    for (auto it = _cond_locks.begin(); it != _cond_locks.end();) {
      if (it->second.lock_addr == lock_addr) {
        it = _cond_locks.erase(it);
      } else {
        ++it;
      }
    }
  }

  /// \brief Snapshot of conditional locks (guard index -> {lock, polarity}).
  std::unordered_map< std::uint64_t, CondLock > cond_locks() const {
    if (_is_bottom || _cond_top) {
      return {};
    }
    return _cond_locks;
  }

  /// \brief True iff no concrete mutex is must-held (i.e. ⊤).
  bool lockset_unknown() const {
    if (_is_bottom) {
      return true;
    }
    return _mutex_top || _mutex_locks.empty();
  }

  bool lockset_is_top() const {
    return this->lockset_unknown();
  }

  // ─ ─── Single-tier legacy API (canonical aliases) ─────────────────

  /// \brief Add a lock to the must-held set.
  void add_lock(std::uint64_t lock_addr) {
    this->add_mutex(lock_addr);
  }

  /// \brief Remove a lock from the must-held set.
  void remove_lock(std::uint64_t lock_addr) {
    this->remove_mutex(lock_addr);
  }

  /// \brief True iff the lock is must-held.
  bool holds_lock(std::uint64_t lock_addr) const {
    return this->holds_mutex(lock_addr);
  }

  // ─ ─── Joined threads (instance ids) ───────────────────────────────

  std::unordered_set< std::uint64_t > get_joined_threads() const {
    if (_is_bottom || _joined_top) {
      return {};
    }
    return _joined_threads;
  }

  void add_joined_thread(std::uint64_t sid) {
    if (_is_bottom) {
      return;
    }
    _joined_top = false;
    _joined_threads.insert(sid);
  }

  /// \brief Spawned-threads (MAY) digest: threads this thread has possibly
  /// spawned. Consulted by DataRaceChecker's create-edge HB — a parent
  /// access whose spawned digest does NOT contain the child's tid happened
  /// BEFORE the pthread_create that started the child (on every path), hence
  /// HB(access, everything in the child).
  std::unordered_set< std::string > get_spawned_threads() const {
    if (_is_bottom || _spawned_top) {
      return {};
    }
    return _spawned_threads;
  }

  /// \brief Spawned-thread INSTANCE ids (mirror of `get_spawned_threads` at the
  /// create-site level).
  std::unordered_set< std::uint64_t > get_spawned_instances() const {
    if (_is_bottom || _spawned_top) {
      return {};
    }
    return _spawned_instances;
  }

  /// \brief Record that a thread with entry `tid` was spawned at (or before)
  /// the current program point (MAY semantics: once spawned, stays spawned
  /// across CFG joins via union).
  void add_spawned_thread(const std::string& tid) {
    if (_is_bottom || _spawned_top) {
      return;
    }
    _spawned_threads.insert(tid);
  }

  /// \brief Record the spawned thread's instance id (create-site id).
  void add_spawned_instance(std::uint64_t sid) {
    if (_is_bottom || _spawned_top) {
      return;
    }
    _spawned_instances.insert(sid);
  }

  /// \brief True iff heap node `idx` is provably FRESH (its pointer has not
  /// been stored into a cross-thread reachable location). A fresh node is
  /// thread-private, so its accesses cannot race (goblint region-domain "fresh
  /// bullet" argument).
  bool is_heap_node_fresh(std::uint64_t idx) const {
    if (_is_bottom) {
      return false;
    }
    return _fresh_heap_nodes.count(idx) != 0;
  }

  /// \brief Mark heap node `idx` as fresh (a malloc result, not yet published).
  void add_heap_node_fresh(std::uint64_t idx) {
    if (_is_bottom) {
      return;
    }
    _fresh_heap_nodes.insert(idx);
  }

  /// \brief Mark heap node `idx` as published (its pointer was stored into a
  /// shared location), removing it from the fresh set.
  void remove_heap_node_fresh(std::uint64_t idx) {
    if (_is_bottom) {
      return;
    }
    _fresh_heap_nodes.erase(idx);
  }

  /// \brief Set the spawned digest to ⊤ ("may have spawned ANY thread"),
  /// discarding the concrete set. Called when an indirect pthread_create's
  /// thread function cannot be resolved to a constant.
  void set_spawned_top() {
    if (_is_bottom) {
      return;
    }
    _spawned_top = true;
    _spawned_threads.clear();
    _spawned_instances.clear();
  }

  /// \brief True iff the spawned digest is ⊤ ("may have spawned any thread").
  bool spawned_is_top() const {
    return !_is_bottom && _spawned_top;
  }

  /// \brief Record that a condition variable `cond` was SIGNALED at (or
  /// before) this program point (MAY semantics: once signaled, stays signaled
  /// across CFG joins via union).
  void add_signaled_cond(std::uint64_t cond) {
    if (_is_bottom || _signaled_top) {
      return;
    }
    _signaled_conds.insert(cond);
  }

  /// \brief Record that this thread WAITED on condition variable `cond` at (or
  /// before) this program point (MAY semantics).
  void add_awaited_cond(std::uint64_t cond) {
    if (_is_bottom || _awaited_top) {
      return;
    }
    _awaited_conds.insert(cond);
  }

  /// \brief Set of cond addresses this thread has possibly SIGNALED.
  std::unordered_set< std::uint64_t > get_signaled_conds() const {
    if (_is_bottom || _signaled_top) {
      return {};
    }
    return _signaled_conds;
  }

  /// \brief Set of cond addresses this thread has possibly WAITED on.
  std::unordered_set< std::uint64_t > get_awaited_conds() const {
    if (_is_bottom || _awaited_top) {
      return {};
    }
    return _awaited_conds;
  }
};

} // end namespace lockset
} // end namespace core
} // end namespace ikos
