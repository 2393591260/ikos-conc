/*******************************************************************************
 *
 * \file
 * \brief Concurrent (pthread) transfer semantics, extracted from the numerical
 *        execution engine
 *
 * Author: ikos-race-detection
 *
 * \copyright See numerical.hpp for the full license.
 *
 * These free functions carry the pthread / atomic / privatization transfer
 * logic that used to live inline inside NumericalExecutionEngine. They are
 * templated on the engine type `E` (the value engine or the pointer-analysis
 * engine) so both instantiations — the lockset-carrying DomainProduct2 and the
 * plain ExceptionDomain — compile. Every entry point is guarded by
 * `ctx.opts.enable_thread_modular` at the call site, so the sequential value
 * analysis path is byte-for-byte unchanged.
 *
 ******************************************************************************/

#pragma once

#include <ikos/ar/semantic/code.hpp>
#include <ikos/ar/semantic/statement.hpp>
#include <ikos/ar/semantic/value.hpp>

#include <ikos/core/number/z_number.hpp>
#include <ikos/core/domain/machine_int/interval.hpp>
#include <ikos/core/domain/lockset/lockset_domain.hpp>
#include <ikos/core/domain/concurrent_global_env.hpp>

#include <ikos/analyzer/analysis/call_context.hpp>
#include <ikos/analyzer/analysis/execution_engine/symbolic_index.hpp>
#include <ikos/analyzer/analysis/literal.hpp>
#include <ikos/analyzer/analysis/memory_location.hpp>
#include <ikos/analyzer/analysis/variable.hpp>
#include <ikos/analyzer/support/assert.hpp>
#include <ikos/analyzer/support/cast.hpp>
#include <ikos/analyzer/util/log.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ikos {
namespace analyzer {

/// \brief Combine a base MemoryLocation identity with a constant byte offset
/// into a single lock key.
///
/// The memory model folds a whole global struct/array into one
/// MemoryLocation, so two locks on distinct fields/elements (`&m.x` vs `&m.y`,
/// `&m[3]` vs `&m[4]`) share the same base address and would collide if keyed
/// by address alone. Encoding the offset restores field/element sensitivity.
/// The combine is a hash — distinct (addr, offset) pairs collide only with
/// probability 2^-64, matching the "trust object identity" assumption already
/// used for `addr` itself (a raw pointer). Offset 0 short-circuits to `addr`
/// so the common whole-object / first-field case keeps its historical key.
inline std::uint64_t offset_lock_key(std::uint64_t addr, const core::ZNumber& off) {
  // Reversible packing of (base stable_id, instance byte offset): the high 32
  // bits are the MemoryLocation stable_id, the low 32 bits are the offset.
  // Injective for base < 2^32 and offset < 2^32 (stable ids are small monotonic
  // counters; byte offsets are < 4GB), so distinct (base, offset) pairs never
  // collide AND the checker can recover which struct-instance a lock protects
  // (06-symbeq_14-list_entry_rc.c).
  std::uint64_t i = off == core::ZNumber(0) ? 0 : off.to< std::uint64_t >();
  return (addr << 32) | (i & 0xFFFFFFFFULL);
}

inline std::uint64_t lock_key_base(std::uint64_t key) {
  return key >> 32;
}

inline std::uint64_t lock_key_instance(std::uint64_t key) {
  return key & 0xFFFFFFFFULL;
}

/// \brief Pack a heap-field pointer-board key: (alloc-site stable_id << 32 |
/// field byte offset). Mirrors `offset_lock_key`'s reversible packing — the
/// high 32 bits identify the DynAlloc allocation site, the low 32 bits the
/// byte offset of the field WITHIN that allocation, so `N_A->next` and
/// `N_B->next` (and `N_A->datum`) never collide (09-regions_03-list2_rc.c FN).
/// A NON-SINGLETON (variable) offset packs as the sentinel 0xFFFFFFFF — a
/// distinct "whole array / unknown field" slot so an imprecise element write
/// (`B[i] = p`) is JOINED onto one summarised entry instead of being dropped
/// (weaver loop-tiling: B[i][j] otherwise derefs to ⊤).
inline std::uint64_t heap_pointer_key(std::uint64_t stable_id,
                                      const core::machine_int::Interval& off) {
  std::uint64_t i = 0xFFFFFFFFULL; // sentinel: variable / unknown field offset
  if (auto s = off.singleton()) {
    i = s->to_z_number().to< std::uint64_t >();
  }
  return (stable_id << 32) | i;
}

/// \brief Is a dynamic-allocation call site inside a CFG loop?
///
/// The DynAlloc abstraction keys a malloc by (call site, context). A call
/// site inside a loop runs once PER iteration and yields a DISTINCT heap
/// object each time, yet they all collapse onto ONE DynAlloc summary cell.
/// Two pointers to such a summary are may-alias, not must-alias, so the
/// cell must not be admitted as a definite lock (04-mutex_44-malloc_sound.c:
/// `p` = iter-3 object and `q` = iter-7 object alias the same cell, silently
/// sharing a spurious common lock). Detect the loop as a CFG cycle through
/// the call's basic block.
inline bool dynalloc_in_loop(ar::CallBase* call) {
  ar::BasicBlock* bb = call->parent();
  if (bb == nullptr) {
    return false;
  }
  std::vector< ar::BasicBlock* > stack(bb->successor_begin(),
                                       bb->successor_end());
  std::unordered_set< ar::BasicBlock* > seen;
  while (!stack.empty()) {
    ar::BasicBlock* n = stack.back();
    stack.pop_back();
    if (n == bb) {
      return true; // back to the allocation block: it lies on a cycle
    }
    if (!seen.insert(n).second) {
      continue;
    }
    for (auto it = n->successor_begin(), et = n->successor_end(); it != et;
         ++it) {
      stack.push_back(*it);
    }
  }
  return false;
}

/// \brief Classification of hardware-atomic / pseudo-locked intrinsics.
enum class AtomicKind {
  None,          // not an atomic primitive
  SectionBegin,  // __VERIFIER_atomic_begin / acquire — pseudo-lock added
                 // and kept across the call
  SectionEnd,    // __VERIFIER_atomic_end / release — pseudo-lock removed
                 // after the call (assumed on from the matching begin)
  SingleCall,    // __sync_*, __atomic_*, __VERIFIER_atomic_CAS/TAS/w/assert
                 // — pseudo-lock wraps just the call
};

/// \brief Single source of truth for atomic-intrinsic detection.
/// Centralised here so the DECLARATION-call, match_down, and match_up
/// sites all agree on what counts as atomic.
inline AtomicKind classify_atomic_intrinsic(const std::string& fname) {
  // SV-COMP atomic-section markers. NOTE: only the standard
  // __VERIFIER_atomic_begin/end are true section markers (the tool must
  // treat the delimited block as non-interleaved). __VERIFIER_atomic_acquire
  // /release are NOT standard intrinsics — in the pthread benchmark family
  // they are DEFINED hand-rolled flags (`assume(MTX==0); MTX=1`), i.e. a
  // RACY lock. Treating them by NAME as atomic sections injects the
  // pseudo-lock and silently suppresses the real race (40_barrier_vf-b.c).
  if (fname == "__VERIFIER_atomic_begin") {
    return AtomicKind::SectionBegin;
  }
  if (fname == "__VERIFIER_atomic_end") {
    return AtomicKind::SectionEnd;
  }
  // GCC __sync_* and __atomic_* families are single-call hardware atomics.
  if (fname.size() >= 7 && fname.compare(0, 7, "__sync_") == 0) {
    return AtomicKind::SingleCall;
  }
  if (fname.size() >= 9 && fname.compare(0, 9, "__atomic_") == 0) {
    return AtomicKind::SingleCall;
  }
  // SV-COMP single-call primitives. __VERIFIER_atomic_w is treated as a
  // single-call atomic (no companion end-marker; it just flips a flag).
  if (fname == "__VERIFIER_atomic_CAS" ||
      fname == "__VERIFIER_atomic_TAS" ||
      fname == "__VERIFIER_atomic_w" ||
      fname == "__VERIFIER_atomic_assert") {
    return AtomicKind::SingleCall;
  }
  // A user-DEFINED function named __VERIFIER_atomic_<suffix> is atomic by the
  // SV-COMP convention (goblint ana.sv-comp.functions): the whole body is a
  // non-interleaved section (29-svcomp_17-atomic_fun_nr.c). The standard
  // markers/primaries are matched above; acquire/release are deliberately NOT
  // atomic — in the pthread benchmark family they are hand-rolled RACY flags
  // (`assume(MTX==0); MTX=1`), and treating them atomic suppresses the real
  // race (40_barrier_vf-b.c).
  if (fname.size() > 18 && fname.compare(0, 18, "__VERIFIER_atomic_") == 0 &&
      fname != "__VERIFIER_atomic_acquire" &&
      fname != "__VERIFIER_atomic_release") {
    return AtomicKind::SingleCall;
  }
  return AtomicKind::None;
}

/// \brief Synthetic lock address used as the pseudo-lock for all atomic
/// sites. Constant so every atomic collapses onto the same lock,
/// regardless of the actual memory operand.
/// 0xA7E0AD1C = mnemonically "ATE-OMIC-1-C".
constexpr std::uint64_t PSEUDO_ATOMIC_LOCK = 0xA7E0AD1CULL;

/// \name Implement pthread operations
/// @{

/// \brief Execute pthread_mutex_lock
///
/// Plan Y (lockset-precision fix, prerequisite for PBR):
///
/// The default IKOS modelling of an unknown call (`exec_unknown_call`)
/// applies `throw_unknown_exceptions()` because it conservatively assumes
/// the call MAY fail. For pthread_mutex_lock/unlock this is catastrophic:
/// the exception join collapses the caller's lockset into ⊤, which then
/// causes our PBR hooks (exec_store / exec_load) to observe
/// `lockset_is_top() == true` at the global variable access site — PBR
/// becomes dead code.
///
/// SV-COMP programs assume the mutex lock/unlock always succeeds (the
/// contract of a well-synchronised program). We therefore BYPASS
/// exec_unknown_call entirely and perform only the minimum side-effects:
///   1. Check that the mutex pointer operand is initialised.
///   2. Update the lockset (add_lock).
///   3. Skip mem_forget_reachable(&m) — we don't model mutex internals,
///      and skipping it preserves the points-to set so the matching
///      unlock gets the SAME lock_addr and can flush the same PBR bucket.
///   4. Skip throw_unknown_exceptions() — pthread synchronisation
///      primitives are assumed not to throw in race-detection analyses.
///   5. Initialise the return value to a non-deterministic int (the
///      success code is unconstrained).
/// \brief Struct-instance base offset of a pointer (container_of inverse).
///
/// For a pointer that is the result of a PointerShift (`&s->mutex = s + 8`),
/// its BASE pointer `s` is the containing struct/array element; its offset
/// is the struct-instance base. Keying a lock by that base — instead of the
/// flat field offset — makes `A[0].mutex` and `A[1].mutex` distinct locks
/// while keeping `A[0].mutex` protecting `A[0].datum` (same instance) and
/// NOT `A[1].datum` (06-symbeq_14-list_entry_rc.c). Falls back to the flat
/// offset for whole-object pointers.
template < typename E >
std::uint64_t instance_base_offset(E& eng, ar::Value* ptr) {
  auto flat_of = [&](ar::Value* v) -> std::uint64_t {
    const Literal& lit = eng.lit_factory().get(v);
    if (lit.is_scalar() && lit.scalar().is_pointer_var()) {
      auto off =
          eng.data().normal().pointer_to_pointer(lit.scalar().var()).offset();
      if (!off.is_top() && !off.is_bottom() && off.lb() == off.ub()) {
        core::ZNumber z = off.lb().to_z_number();
        return z.to< std::uint64_t >();
      }
    }
    return 0;
  };
  ar::Value* base = pointershift_base(ptr);
  if (base != nullptr) {
    // ARRAY element (`&m[i]`: the PointerShift base is the array `m`, whose
    // pointee is an ArrayType). ikos-pp FLATTENS `m[i].field` into one
    // PointerShift (`&cache[5].refs_mutex` → offset 248), so the flat
    // offset is the FIELD offset, not the element base. Round DOWN to the
    // element boundary so a field shares its element base with its siblings
    // (cache[5].refs_mutex at 248 → 240, matching cache[5].refs at 240) —
    // the flat self-lock compares element bases, not field offsets
    // (06-symbeq_05-funloop_hard2.c). STRUCT field (`&s->mutex`: the base
    // `s` is a struct) — the base's offset is the struct instance base.
    auto* bt = dyn_cast< ar::PointerType >(base->type());
    if (bt != nullptr && isa< ar::ArrayType >(bt->pointee())) {
      std::uint64_t off = flat_of(ptr);
      auto* at = cast< ar::ArrayType >(bt->pointee());
      std::uint64_t elem_size =
          eng.data_layout().alloc_size_in_bytes(at->element_type())
              .template to< std::uint64_t >();
      return (elem_size != 0) ? off - (off % elem_size) : off;
    }
    return flat_of(base); // struct field
  }
  return flat_of(ptr); // whole object
}

/// \brief Execute pthread_cond_signal / pthread_cond_broadcast
///
/// A signal/broadcast only wakes a waiter — for RACE DETECTION it has no
/// side effect on the lockset or memory (the signal→wait happens-before
/// edge is a later precision refinement, not this soundness stub). Only the
/// int return code is nondet. Deliberately NOT exec_unknown_extern_call:
/// its may_throw_exc → throw_unknown_exceptions() collapses the
/// must-lockset to ⊤ (FN).
template < typename E >
void exec_pthread_cond_signal(E& eng, ar::CallBase* call, bool broadcast = false) {
  // Resolve the cond address (arg0) to a (base, offset) key, mirroring
  // exec_pthread_mutex_lock's strong admission. Unresolvable / non-singleton
  // → key 0, so the queue + digest are left untouched (conservative: the
  // signal may target any cond; not recording it only loses precision).
  std::uint64_t cond_key = 0;
  if (call->arg_begin() != call->arg_end()) {
    ar::Value* cond_arg = *call->arg_begin();
    const auto& lit = eng.lit_factory().get(cond_arg);
    if (lit.is_scalar() && lit.scalar().is_pointer_var()) {
      auto pt = eng.data().normal().pointer_to_pointer(lit.scalar().var());
      if (!pt.is_bottom() && !pt.is_top() && pt.points_to().is_set() &&
          pt.points_to().size() == 1) {
        MemoryLocation* loc = *pt.points_to().begin();
        const auto& off = pt.offset();
        if (!off.is_top() && !off.is_bottom() && off.lb() == off.ub()) {
          std::uint64_t base =
              ikos::core::IndexableTraits< MemoryLocation* >::index(loc);
          cond_key = offset_lock_key(base, off.lb().to_z_number());
        }
      }
    }
  }
  if (cond_key != 0) {
    eng.ctx().concurrent_env.cond_signal(cond_key,
                                                                broadcast);
    // MAY digest: record that this thread signaled cond (observable state,
    // not consumed by the race checker).
    eng.lockset().add_signaled_cond(cond_key);
  }
  if (call->has_result()) {
    const Literal& ret = eng.lit_factory().get(call->result());
    if (ret.is_scalar()) {
      ikos_assert_msg(ret.scalar().is_var(),
                      "left hand side is not a variable");
      eng.data().normal().scalar_assign_nondet(ret.scalar().var());
    }
  }
}

/// \brief Execute pthread_cond_wait(&cond, &mutex)
///
/// POSIX: atomically RELEASE `mutex`, wait, then RE-ACQUIRE it — net effect
/// on the must-lockset is UNCHANGED. The sound minimal model therefore
/// leaves the lockset alone; the faithful release/re-acquire is reproduced
/// only for a MUST-HELD mutex (re-adding a lock that was not definitely
/// held would FABRICATE one — unsound). Crucially we do NOT fall through to
/// exec_unknown_extern_call, whose may_throw_exc → throw_unknown_exceptions()
/// collapses the must-lockset to ⊤ and mem_forget_reachable(&mutex) forgets
/// the mutex memory — the FN in thread-join-counter-inner-race-3 (the data
/// write's must-lock vanished, so the unlocked read no longer raced).
template < typename E >
void exec_pthread_cond_wait(E& eng, ar::CallBase* call) {
  // Resolve the cond address (arg0) to a (base, offset) key (mirroring the
  // mutex resolution below). Unresolvable → 0, so the queue + digest are
  // skipped (conservative: only loses precision).
  std::uint64_t cond_key = 0;
  if (call->arg_begin() != call->arg_end()) {
    ar::Value* cond_arg = *call->arg_begin();
    const auto& clit = eng.lit_factory().get(cond_arg);
    if (clit.is_scalar() && clit.scalar().is_pointer_var()) {
      auto cpt = eng.data().normal().pointer_to_pointer(clit.scalar().var());
      if (!cpt.is_bottom() && !cpt.is_top() && cpt.points_to().is_set() &&
          cpt.points_to().size() == 1) {
        MemoryLocation* cloc = *cpt.points_to().begin();
        const auto& coff = cpt.offset();
        if (!coff.is_top() && !coff.is_bottom() && coff.lb() == coff.ub()) {
          std::uint64_t cbase =
              ikos::core::IndexableTraits< MemoryLocation* >::index(cloc);
          cond_key = offset_lock_key(cbase, coff.lb().to_z_number());
        }
      }
    }
  }
  if (call->arg_begin() + 1 != call->arg_end()) {
    ar::Value* mutex_arg = *(call->arg_begin() + 1);
    const auto& lit = eng.lit_factory().get(mutex_arg);
    if (lit.is_scalar() && lit.scalar().is_pointer_var()) {
      auto pt = eng.data().normal().pointer_to_pointer(lit.scalar().var());
      if (!pt.is_bottom() && !pt.is_top() && pt.points_to().is_set() &&
          pt.points_to().size() == 1) {
        MemoryLocation* loc = *pt.points_to().begin();
        const auto& off = pt.offset();
        if (!off.is_top() && !off.is_bottom() && off.lb() == off.ub()) {
          std::uint64_t base =
              ikos::core::IndexableTraits< MemoryLocation* >::index(loc);
          std::uint64_t key = offset_lock_key(base, off.lb().to_z_number());
          if (eng.lockset().holds_lock(key)) {
            eng.lockset().remove_lock(key);
            eng.lockset().add_lock(key);
          }
        }
      }
    }
  }
  // Register this thread on the cond wait queue + mark the awaited digest
  // (MAY state, observable, not consumed by the race checker).
  if (cond_key != 0) {
    auto& env = eng.ctx().concurrent_env;
    env.cond_wait_enter(cond_key);
    eng.lockset().add_awaited_cond(cond_key);
  }
  // The wait BLOCKS until a signal: while it blocks, OTHER threads run and
  // may change the shared globals (threads_alive, data, …). Forget the
  // memory so a `while(threads_alive) pthread_cond_wait(...)` loop can EXIT
  // — without this, the no-op body leaves the loop's exit ⊥ in the fixpoint
  // and the unlocked read AFTER the loop (that races) is unreachable (FN:
  // thread-join-counter-inner-race-3).
  eng.data().normal().mem_forget_all();
  if (call->has_result()) {
    const Literal& ret = eng.lit_factory().get(call->result());
    if (ret.is_scalar()) {
      ikos_assert_msg(ret.scalar().is_var(),
                      "left hand side is not a variable");
      eng.data().normal().scalar_assign_nondet(ret.scalar().var());
    }
  }
}

template < typename E >
void exec_pthread_mutex_lock(E& eng, ar::CallBase* call,
                             bool read_lock = false,
                             bool trylock = false) {
  // Get the mutex argument (first argument). An UNINITIALISED mutex pointer
  // is a dangling reference whose points-to is ⊤. Going BOTTOM here (the old
  // `uninit_assert_initialized`) would mark the whole path dead and silently
  // drop every subsequent access, hiding a real race (04-mutex_31-uninitialized.c
  // FN). Instead let the admission/release logic below handle ⊤ soundly:
  // lock admits nothing (lockset unchanged), unlock treats "any mutex may have
  // been released" and strips the must-lockset — both only widen the race set.
  ar::Value* mutex_arg = *call->arg_begin();

  // Strong Lock (must-lockset soundness): the lockset domain is a
  // MUST-set ("locks certainly held"). Only when the argument's
  // points-to set designates EXACTLY ONE mutex can that mutex be
  // added as certainly-held. For a polymorphic pointer
  // (`m = if(i) &m1 else &m2`, points-to = {mutex1, mutex2}) adding
  // either candidate would fabricate a definite protection the
  // program does not have — the FN root cause in
  // 04-mutex_24-sound_lock.c: main's `myglobal+1` was recorded as
  // mutex1-protected while at runtime it may hold mutex2, silently
  // sharing a (spurious) common lock with t_fun. Ambiguous / TOP /
  // bottom / non-pointer arguments therefore leave the lockset
  // UNCHANGED (fewer certainly-held locks only widens the reported
  // race set — sound for race detection).
  std::vector< std::uint64_t > acquired_addrs;
  const auto& mutex_lit = eng.lit_factory().get(mutex_arg);
  if (mutex_lit.is_scalar()) {
    const auto& scalar_lit = mutex_lit.scalar();
    if (scalar_lit.is_pointer_var()) {
      auto points_to = eng.data().normal().pointer_to_pointer(scalar_lit.var());
      if (!points_to.is_bottom() && !points_to.is_top() &&
          points_to.points_to().is_set() &&
          points_to.points_to().size() == 1) {
        MemoryLocation* loc = *points_to.points_to().begin();
        // 🧱 Loop-allocated DynAlloc guard: a malloc site inside a loop
        // yields a DISTINCT object per iteration, yet they all fold onto
        // ONE DynAlloc summary cell. Two pointers to such a cell are
        // may-alias, not must-alias — admitting it as a definite lock would
        // let `p` (iter 3) and `q` (iter 7) share a spurious common lock
        // (04-mutex_44-malloc_sound.c FN). Refuse it: fewer certainly-held
        // locks only widens the race set — the sound direction.
        bool loop_summary = false;
        if (auto* dalloc = dyn_cast< DynAllocMemoryLocation >(loc)) {
          loop_summary = dynalloc_in_loop(dalloc->call());
          // A malloc inside a HELPER called from a loop is also
          // loop-allocated: the malloc's own block may be loop-free while
          // the helper's CALL SITE sits in a caller's loop
          // (04-mutex_44-malloc_sound.c FN: safe_malloc's malloc is not on
          // a cycle directly, but main calls safe_malloc from its loop).
          // Walk the call-context chain and test each call site for a CFG
          // cycle.
          for (CallContext* ctx = dalloc->context();
               !loop_summary && ctx != nullptr && !ctx->empty();
               ctx = ctx->parent()) {
            loop_summary = dynalloc_in_loop(ctx->call());
          }
        }
        // ⭐ Offset-aware locking: the memory model collapses a whole
        // global struct/array into ONE MemoryLocation (`struct{...} m`
        // → `@m`, `pthread_mutex_t m[10]` → `@m`). Keying the lock by
        // address alone would make `&m.x` and `&m.y` (or `&m[3]` and
        // `&m[4]`) the same lock. The runtime pointer value of the
        // argument carries the byte offset INTO the collapsed node, so
        // a CONSTANT offset (&m.y, &m[4]) is a definite, distinct lock
        // — admit it as (base, offset). A VARIABLE offset (&m[i]) is
        // ambiguous and must NOT be admitted (sound: fewer certainly-
        // held locks only widens the race set) — this is what keeps
        // 05-lval_ls_13-idxunknown_lock.c RACE.
        if (!loop_summary) {
          const auto& poff = points_to.offset();
          std::uint64_t base =
              ikos::core::IndexableTraits<MemoryLocation*>::index(loc);
          auto& env = eng.ctx().concurrent_env;
          if (!poff.is_top() && !poff.is_bottom() && poff.lb() == poff.ub()) {
            // Key by the FLAT byte offset (lock IDENTITY: m[0]/m[1],
            // m.x/m.y are distinct). The struct-instance base (protection
            // region) is recorded alongside so the checker can refuse to let
            // A[0].mutex protect A[1].datum (06-symbeq_14-list_entry_rc.c).
            std::uint64_t key =
                offset_lock_key(base, poff.lb().to_z_number());
            acquired_addrs.push_back(key);
            env.record_lock_instance(key, instance_base_offset(eng, mutex_arg));
          } else {
            // Variable offset (&mutex[i]): the flat offset cannot distinguish
            // mutex[j] from mutex[k], so key it SYMBOLICALLY. Bind
            // (base, SymbolicIndex) in a side table under a deterministic key
            // so the checker can match "lock slot == access slot"
            // (09-regions_11-arraylist_nr.c FP — flat-array summarization).
            // A non-affine index bails (sound: the lock is not admitted).
            core::ConcurrentGlobalEnv::SymbolicIndex sym;
            if (symbolic_index_of(mutex_arg, sym, eng.call_context())) {
              std::uint64_t key = env.record_lock_symbolic_index(base, sym);
              acquired_addrs.push_back(key);
            }
          }
        }
      } else if (!points_to.is_bottom() && !points_to.is_top() &&
                 points_to.points_to().is_set() &&
                 points_to.points_to().size() > 1) {
        // Polymorphic lock (`m = &s->mutex` where `s = x ? &A : &B`, so
        // points-to = {A, B}): the must-lockset needs a singleton and would
        // drop this lock entirely (06-symbeq_11-equ_nr.c FP). But the lock
        // and the access share the SAME SSA base pointer `s`, so admit it
        // SYMBOLICALLY keyed by var_id(s) — the checker matches it against
        // the access's own base identity (local self-consistency). base=0:
        // the identity is the pointer, not any one cell. Non-pointer-base
        // args bail (sound: fewer locks only widens the race set).
        core::ConcurrentGlobalEnv::SymbolicIndex sym;
        if (symbolic_base_of(mutex_arg, sym)) {
          std::uint64_t key =
              eng.ctx().concurrent_env
                  .record_lock_symbolic_index(0, sym);
          acquired_addrs.push_back(key);
        }
      }
      // TOP/bottom: ambiguous — add NOTHING (see doc).
    }
    // Non-pointer argument: nothing certain to acquire.
  }

  // === Lockset-precision fix (Plan Y) ===
  //
  // add_lock MUST happen on the LHS-invariant that already reflects the
  // pre-call state. Because we bypass exec_unknown_call, nothing in this
  // function call will collapse the invariant, so the lockset stays
  // concrete at every successor of this call site.
  //
  // === Strong Lock admission ===
  //
  // Only a UNIQUE designation admits the lock into the must-set. An
  // ambiguous acquisition leaves the lockset untouched — never add a
  // possibly-not-held lock (unsound protection, the
  // 04-mutex_24-sound_lock.c FN), and never fabricate the synthetic
  // address 0x0 (Bug 4: it would silently alias every unresolvable
  // lock site). Soundness note: NOT adding a lock can only shrink
  // the must-set, which for race detection merely widens the
  // reported race set — the sound direction.
  // Conditional-lock (trylock) guard: the result variable's index. The
  // result is assigned nondet below; its interval at an access site tells
  // the checker whether the lock is held (== 0) or not (EBUSY/…). Only a
  // SCALAR result can serve as the guard; the index is the variable
  // address (IndexableTraits<Variable*>), so the checker can recover the
  // variable with a reinterpret_cast.
  std::uint64_t cond_guard = 0;
  if (trylock && call->has_result()) {
    const Literal& ret = eng.lit_factory().get(call->result());
    if (ret.is_scalar() && ret.scalar().is_var()) {
      cond_guard =
          ikos::core::IndexableTraits< Variable* >::index(ret.scalar().var());
    }
  }

  // === Phase 1: conditional-lock hoisting (`if (cond) lock(m)`) =========
  //
  // ikos-pp lowers `if (i) lock(m)` so the branch is IMPLICIT (the block has
  // two successors) and the SIBLING successor carries the condition as a
  // Comparison (`i == 0`) that refines its path. So the lock — in the other
  // successor — is held iff that Comparison is FALSE. Record it as a
  // HELD_IF_NONZERO/HELD_IF_ZERO conditional lock instead of a must-lock so
  // the correlation survives the join (04-mutex_07-ps_nr.c FP). The checker
  // resolves it only when the guard's interval is a singleton matching the
  // polarity — the sound direction (unknown guard ⇒ lock not admitted).
  std::uint64_t branch_guard = 0;
  core::lockset::CondPolarity branch_polarity =
      core::lockset::CondPolarity::HELD_IF_NONZERO;
  bool conditional_lock = false;
  ar::BasicBlock* branch_false_bb = nullptr; // false-branch block (for replay)
  if (!read_lock && !trylock) {
    ar::BasicBlock* bb = call->parent();
    if (bb != nullptr && bb->num_predecessors() == 1) {
      ar::BasicBlock* pred = *bb->predecessor_begin();
      if (pred->num_successors() == 2) {
        auto sit = pred->successor_begin();
        ar::BasicBlock* s0 = *sit;
        ++sit;
        ar::BasicBlock* s1 = *sit;
        ar::BasicBlock* sibling = (s0 == bb) ? s1 : s0;
        branch_false_bb = sibling;
        // The sibling must carry the branch Comparison and no call (no lock
        // on the other path) — otherwise the correlation is not certain.
        const ar::Comparison* cmp = nullptr;
        bool sibling_has_call = false;
        for (ar::Statement* ss : *sibling) {
          if (auto* c = dyn_cast< ar::Comparison >(ss)) {
            cmp = c;
          } else if (isa< ar::CallBase >(ss)) {
            sibling_has_call = true;
          }
        }
        bool pred_eq = false;
        bool supported_pred = false;
        if (cmp != nullptr && !sibling_has_call) {
          auto* rc = dyn_cast< ar::IntegerConstant >(cmp->right());
          if (rc != nullptr && rc->value().is_zero()) {
            switch (cmp->predicate()) {
              case ar::Comparison::SIEQ:
              case ar::Comparison::UIEQ:
                pred_eq = true;
                supported_pred = true;
                break;
              case ar::Comparison::SINE:
              case ar::Comparison::UINE:
                pred_eq = false;
                supported_pred = true;
                break;
              default:
                break; // unsupported predicate: bail (sound)
            }
          }
        }
        if (supported_pred) {
          // The lock is on the OPPOSITE path of the sibling's condition:
          // sibling `i == 0` (eq) ⇒ lock held iff i != 0 ⇒ NONZERO;
          // sibling `i != 0` (ne) ⇒ lock held iff i == 0 ⇒ ZERO.
          bool held_iff_nonzero = pred_eq;
          if (held_iff_nonzero) {
            // NONZERO branch lock: canonicalize the guard (peel the affine
            // i++/i-- chain so `if (i-1)` after `i++` resolves to `i`), and
            // store the base AR variable pointer. The checker matches it
            // against the access's OWN branch guard by SSA identity
            // (04-mutex_17-ps_add1_nr.c / 04-mutex_07-ps_nr.c).
            LinearIndex lin;
            if (resolve_linear_index(cmp->left(), lin) && lin.coeff == 1 &&
                lin.const_term == 0) {
              branch_guard = lin.var_id;
              branch_polarity = core::lockset::CondPolarity::HELD_IF_NONZERO;
              conditional_lock = true;
            }
          } else {
            const Literal& g = eng.lit_factory().get(cmp->left());
            if (g.is_scalar() && g.scalar().is_var()) {
              branch_guard = ikos::core::IndexableTraits< Variable* >::index(
                  g.scalar().var());
              branch_polarity = core::lockset::CondPolarity::HELD_IF_ZERO;
              conditional_lock = true;
            }
          }
        }
      }
    }
  }

  for (std::uint64_t la : acquired_addrs) {
    if (read_lock) {
      eng.lockset().add_read_lock(la);
    } else if (trylock) {
      eng.lockset().add_cond_lock(cond_guard, la);
    } else if (conditional_lock) {
      eng.lockset().add_cond_lock(branch_guard, la,
                                                branch_polarity);
      // Replay the correlation onto the FALSE branch too, so the
      // must-lockset intersection at the join PRESERVES it (both branches
      // carry the "m held iff guard!=0" fact; on the false branch guard==0
      // makes it vacuously true). The Comparison in the false branch reads
      // this and calls lockset_add_cond_lock on its own state.
      if (branch_false_bb != nullptr &&
          branch_polarity == core::lockset::CondPolarity::HELD_IF_NONZERO) {
        auto& env = eng.ctx().concurrent_env;
        env.record_pending_cond_lock(branch_false_bb, branch_guard, la,
                                     /*is_nonzero=*/true);
      }
    } else {
      eng.lockset().add_lock(la);
    }
  }
  // === Plan Y2 (Best of Both Worlds) ===
  // We DO NOT need to track the mutex's internal memory across this call
  // (its owner field, lock count, etc. are pthread runtime internals).
  // Forgetting the reachable memory from the mutex pointer collapses
  // pthread runtime state back to ⊤ in the value/pointer domains — this
  // is what exec_unknown_call used to do for us with mem_forget_reachable,
  // but with the side-effect of also collapsing the lockset via the
  // exception join. We reproduce only the precision-collapsing part here
  // (lockset is preserved because we update it directly above).
  //
  // Defensive: only attempt the forget if the operand is a tracked
  // internal variable. If nullity lookup is unavailable (e.g. the pointer
  // was just created by an indirect call), we still want the pthread
  // internals forgotten — falling back to a no-op preserves soundness
  // and lets the lockset still apply.
  if (auto iv = dyn_cast< ar::InternalVariable >(mutex_arg)) {
    Variable* ptr = eng.var_factory().get_internal(iv);
    try {
      if (!eng.data().normal().nullity_is_null(ptr)) {
        eng.data().normal().mem_forget_reachable(ptr);
      }
    } catch (...) {
      // nullity_is_null may assert on variables that haven't been
      // initialised by the inliner yet; defer the forget rather than
      // crash. The post-unlock flush still drives the PBR release.
    }
  }

  // Initialise the return value (int rc) to a non-deterministic int.
  // pthread_mutex_lock may legally return success (0) or specific error
  // codes (EINVAL, EDEADLK, ENOMEM) — none of which affect the lockset
  // semantics for our race analysis.
  if (call->has_result()) {
    const Literal& ret = eng.lit_factory().get(call->result());
    if (ret.is_scalar()) {
      ikos_assert_msg(ret.scalar().is_var(),
                      "left hand side is not a variable");
      eng.data().normal().scalar_assign_nondet(ret.scalar().var());
    }
  }

  // Diagnostic for verification (debug-level; off by default).
  if (ikos_unlikely(
          ikos::analyzer::log::is_enabled_for(
              ikos::analyzer::LogLevel::Debug))) {
    log::msg() << "[Lockset] pthread_mutex_lock acquired "
               << acquired_addrs.size() << " unique lock(s)\n";
  }
}

/// \brief Execute pthread_mutex_unlock
///
/// Plan Y (lockset-precision fix, prerequisite for PBR): symmetric to
/// exec_pthread_mutex_lock — bypass exec_unknown_call so that the
/// lockset stays concrete across the unlock boundary, and so that the
/// privatized write flush happens on an invariant that still holds the
/// lock (we remove the lock FIRST, then publish).
template < typename E >
void exec_pthread_mutex_unlock(E& eng, ar::CallBase* call, bool also_read = false) {
  // Get the mutex argument (first argument). An UNINITIALISED mutex pointer
  // is a dangling reference whose points-to is ⊤. Going BOTTOM here (the old
  // `uninit_assert_initialized`) would mark the whole path dead and silently
  // drop every subsequent access, hiding a real race (04-mutex_31-uninitialized.c
  // FN). Instead let the admission/release logic below handle ⊤ soundly:
  // lock admits nothing (lockset unchanged), unlock treats "any mutex may have
  // been released" and strips the must-lockset — both only widen the race set.
  ar::Value* mutex_arg = *call->arg_begin();

  // Weak Unlock (must-lockset exclusion): collect EVERY mutex the
  // argument's points-to set MAY designate. The lockset domain is a
  // MUST-set ("locks certainly held"); when the argument can denote
  // several mutexes, a release MIGHT unlock any one of them — every
  // candidate must leave the must-set, otherwise a lock that was
  // actually released still counts as held and its protected writes
  // keep a spurious common-lock with concurrent accesses (the FN
  // root cause in 04-mutex_23-sound_unlock.c: `m = if(i) &m1 else
  // &m2` truncated to `.begin()` removed only mutex2).
  std::vector< std::uint64_t > released_addrs;
  bool released_unknown = false; // points-to is TOP: any lock possible
  const auto& mutex_lit = eng.lit_factory().get(mutex_arg);
  if (mutex_lit.is_scalar()) {
    const auto& scalar_lit = mutex_lit.scalar();
    if (scalar_lit.is_pointer_var()) {
      auto points_to = eng.data().normal().pointer_to_pointer(scalar_lit.var());
      if (points_to.is_top()) {
        // Points-to completely unknown: the released lock could be
        // ANY mutex — no protection can be trusted afterwards.
        released_unknown = true;
      } else if (points_to.is_bottom()) {
        // No target at all: nothing to release.
        released_addrs.clear();
      } else {
        const auto& inner = points_to.points_to();
        // Contract: `begin()` asserts kind == SetKind. A TopKind
        // inner set (is_empty() is false for it — kind-based test)
        // must be treated as "any mutex possible".
        if (!inner.is_set()) {
          released_unknown = true;
        } else {
          // Offset-aware release (mirror of the lock side): a CONSTANT
          // offset resolves to one distinct (base, offset) key. A
          // VARIABLE offset (`unlock(&m[i])`) cannot name which field/
          // element was held, so EVERY candidate must leave the
          // must-set (conservative weak unlock).
          const auto& poff = points_to.offset();
          bool const_off =
              !poff.is_top() && !poff.is_bottom() && poff.lb() == poff.ub();
          if (!const_off) {
            released_unknown = true;
          } else {
            for (MemoryLocation* loc : inner) {
              std::uint64_t base =
                  ikos::core::IndexableTraits<MemoryLocation*>::index(loc);
              released_addrs.push_back(
                  offset_lock_key(base, poff.lb().to_z_number()));
            }
          }
        }
      }
    } else {
      // Non-pointer argument (e.g. resolved to an integer literal):
      // nothing known to release.
      released_unknown = true;
    }
  } else {
    released_unknown = true;
  }
  const bool unlock_addr_known =
      (!released_addrs.empty() || released_unknown);
  const std::uint64_t lock_addr =
      released_addrs.empty() ? 0 : released_addrs[0];

  // Remove lock from lockset domain FIRST so the post-unlock invariant
  // is consistent: this thread holds no locks after the unlock.
  //
  // `unlock_addr_known` guards the flush dispatch below. It
  // mirrors the `lock_addr_known` discipline in exec_pthread_mutex_lock
  // (line 4205): if points-to extraction failed entirely, we MUST
  // NOT route the release through a single flush_privatized(0) —
  // the value 0 is reserved as `kNoLockPartitionAddr` for the
  // "unlocked writes" sentinel partition, and a spurious unlock-side
  // flush would prematurely drain that partition mid-iteration,
  // violating strict unlock-flushing.

  // Must-lockset full exclusion (weak unlock): remove EVERY
  // candidate; a TOP points-to strips the whole lockset (any lock
  // may have been released — no protection survives). PRESERVE the
  // join/spawn digest: "this thread spawned T" is a monotonic fact,
  // not a lock, and wiping it here lets the create-HB edge misfire
  // and mask a real race (05-lval_ls_16-idxunknown_unlock.c).
  if (released_unknown) {
    for (std::uint64_t la : eng.lockset().held_locks()) {
      eng.lockset().remove_lock(la);
    }
    // Any lock may have been released: drop every conditional lock too.
    for (const auto& kv : eng.lockset().cond_locks()) {
      eng.lockset().remove_cond_lock(kv.second.lock_addr);
    }
    if (also_read) {
      for (std::uint64_t la :
           eng.lockset().held_read_locks()) {
        eng.lockset().remove_read_lock(la);
      }
    }
  } else {
    for (std::uint64_t ra : released_addrs) {
      eng.lockset().remove_lock(ra);
      eng.lockset().remove_cond_lock(ra);
      if (also_read) {
        eng.lockset().remove_read_lock(ra);
      }
    }
  }

  // PBR release (Goblint SAS'21) + AstréeA partition deposit:
  // atomically drain the privatized registry for this lock, join
  // per-address, and join_partition_nolock each into the lock's
  // partition `_lock_partition[lock_addr]`. After this call, the
  // protected writes are visible to lock-aware reads by any thread
  // holding the same lock; unlocked reads see them via the flat
  // `_global_board` (which they only consult if no held lock has a
  // partition entry — see read_global_with_lockset).
  //
  // The lockset_remove_lock above happens FIRST so that the local
  // lattice state is consistent: post-unlock, this thread holds no
  // locks, and concurrent analyses will see the freshly published
  // partition state.
  //
  // Note: flush_privatized takes the global env's internal mutex,
  // which serializes against record_privatized_write in other
  // threads. Since pthread mutexes are mutually exclusive at runtime,
  // only the releasing thread ever has writes in its privatized set
  // — no cross-thread contention is possible in a correct program.
  //
  // flush_privatized internally calls join_partition_nolock (NOT
  // join_partition) because it already holds the env mutex —
  // re-acquiring a non-recursive std::mutex would deadlock.
  if (unlock_addr_known) {
    auto& env = eng.ctx().concurrent_env;
    // Weak-unlock flush dispatch. With multiple candidate locks, EACH
    // candidate is a possible release point. With an unknown (TOP) target,
    // or a lockset already TOP, no bucket can be attributed to this
    // release: fall back to the full over-approximation drain (soundness
    // guard — publishing every bucket only widens partitions, never
    // narrows).
    if (released_unknown || eng.lockset().lockset_is_top()) {
      env.flush_all_privatized_locks();
    } else {
      for (std::uint64_t ra : released_addrs) {
        env.flush_privatized(ra);
      }
    }
  }

  // === Plan Y2 (Best of Both Worlds) — symmetric to lock handler ===
  //
  // Collapse pthread runtime internals (owner field, lock count, etc.)
  // back to ⊤ so the post-unlock fixpoint converges quickly. The
  // lockset has already been updated above, so this forget does NOT
  // touch race-detection precision.
  if (auto iv = dyn_cast< ar::InternalVariable >(mutex_arg)) {
    Variable* ptr = eng.var_factory().get_internal(iv);
    try {
      if (!eng.data().normal().nullity_is_null(ptr)) {
        eng.data().normal().mem_forget_reachable(ptr);
      }
    } catch (...) {
      // nullity lattice not initialised yet — defer the forget and let
      // the next iteration handle it. PBR flush has already happened above.
    }
  }

  // Initialise the return value (int rc) to a non-deterministic int.
  if (call->has_result()) {
    const Literal& ret = eng.lit_factory().get(call->result());
    if (ret.is_scalar()) {
      ikos_assert_msg(ret.scalar().is_var(),
                      "left hand side is not a variable");
      eng.data().normal().scalar_assign_nondet(ret.scalar().var());
    }
  }

  // Diagnostic for verification (debug-level; off by default).
  if (ikos_unlikely(
          ikos::analyzer::log::is_enabled_for(
              ikos::analyzer::LogLevel::Debug))) {
    log::msg() << "[Lockset] pthread_mutex_unlock released lock 0x" << std::hex
               << lock_addr << std::dec << "\n";
  }
}

/// \brief Execute pthread_join
///
/// Resolves the pthread_t handle (arg 0) to the thread INSTANCE (create-site
/// id) that `pthread_create` stored into it, and stamps that instance into the
/// join digest. pthread_create now writes the instance id as the handle's
/// CONCRETE VALUE, so pthread_join reads it back: a singleton interval names
/// the exact instance to join (28/29/30-join-array, where `tids[0]` holds a
/// different id than `tids[1]`).
///
/// An UNKNOWN handle (⊤ interval — e.g. `foo(&id2)` clobbered the value in
/// 07-trivial-unknowntid, or an array element we never assigned) yields NO
/// instance → the join adds NOTHING. The old "join ALL threads" fallback
/// fabricated a happens-before edge that may not exist; an unknown join must
/// not suppress a race. Sound direction: fewer HB edges ⇒ wider race set.
template < typename E >
void exec_pthread_join(E& eng, ar::CallBase* call) {
  auto& env = eng.ctx().concurrent_env;
  std::unordered_set< std::uint64_t > joined_sids;

  ar::Value* handle_arg = *call->arg_begin();
  const auto& handle_lit = eng.lit_factory().get(handle_arg);
  if (handle_lit.is_scalar()) {
    const auto& scalar_lit = handle_lit.scalar();
    if (scalar_lit.is_machine_int_var()) {
      auto iv = eng.data().normal().int_to_interval(scalar_lit.var());
      // A global pthread_t written by pthread_self() in main reads back as
      // [0, kMainThreadSid] — the thread-modular global blackboard joins the
      // zero-init lower bound with the stored main id. That interval can only
      // mean "uninitialized 0 (UB for join) or main's self", so treat it as
      // joining main (sound: the 0 arm is UB and may be ignored).
      if (!iv.is_top() && !iv.is_bottom() &&
          iv.ub().to_z_number().template to< std::uint64_t >() ==
              core::kMainThreadSid &&
          iv.lb().to_z_number() == core::ZNumber(0)) {
        joined_sids.insert(core::kMainThreadSid);
      } else if (auto single = iv.singleton()) {
        std::uint64_t sid =
            single->to_z_number().template to< std::uint64_t >();
        if (sid == core::kMainThreadSid ||
            (sid != 0 && env.site_func(sid) != nullptr)) {
          joined_sids.insert(sid);
        }
      }
    } else if (scalar_lit.is_machine_int()) {
      std::uint64_t sid = scalar_lit.machine_int()
                              .to_z_number()
                              .template to< std::uint64_t >();
      if (sid == core::kMainThreadSid ||
          (sid != 0 && env.site_func(sid) != nullptr)) {
        joined_sids.insert(sid);
      }
    }
    // Pointer / other arg forms carry no instance value → nothing to join.
  }

  for (std::uint64_t sid : joined_sids) {
    eng.lockset().add_joined_thread(sid);
    // Transitive join HB: joining a thread also joins every thread IT
    // definitely joined before exiting (main joins t_benign, t_benign joins
    // t_fun ⇒ main transitively joins t_fun — 51-threadjoins/01-trivial.c).
    // The summary is the joined thread's exit digest, transitively closed
    // because this same site pulls the summary of whatever IT joined.
    ar::Function* f = env.site_func(sid);
    if (f != nullptr) {
      for (std::uint64_t t : env.get_join_summary(f)) {
        eng.lockset().add_joined_thread(t);
      }
    }
  }

  // Execute default unknown call handling (writes to *retval may happen,
  // but no other side effects).
  eng.exec_unknown_call(call,
                           /* may_write_params = */ true,
                           /* ignore_unknown_write = */ true,
                           /* may_write_globals = */ false,
                           /* may_throw_exc = */ false);
}

/// \brief Execute pthread_once(&once_control, init_routine).
///
/// Sound model: pthread_once runs `init_routine` EXACTLY once, serialized
/// before any pthread_once on the same control returns. For race detection
/// that is precisely "spawn init_routine then join it": the callback's
/// accesses happen-before every caller's post-once accesses, so its unlocked
/// init write must not race the (locked) uses that follow (87-once/* FP).
/// The callback has no data side effects beyond its own body; the
/// once-control write is pthread runtime state (excluded on the checker
/// side). We do NOT mark it `spawned` — it is not concurrent with the caller.
template < typename E >
void exec_pthread_once(E& eng, ar::CallBase* call) {
  // arg0 = pthread_once_t* (ignored: runtime sync machinery), arg1 = callback.
  ar::Function* once_func = nullptr;
  if (call->arg_begin() + 1 != call->arg_end()) {
    ar::Value* fn_val = *(call->arg_begin() + 1);
    // Peel value-preserving casts (a signature mismatch, mirrors the
    // pthread_create thread-function resolution).
    for (int depth = 0; depth < 8; ++depth) {
      if (isa< ar::FunctionPointerConstant >(fn_val)) {
        break;
      }
      auto* iv = dyn_cast< ar::InternalVariable >(fn_val);
      if (iv == nullptr) {
        break;
      }
      ar::Statement* def = unique_def(iv);
      if (auto* un = dyn_cast_or_null< ar::UnaryOperation >(def)) {
        if (un->op() == ar::UnaryOperation::Bitcast) {
          fn_val = un->operand();
          continue;
        }
      }
      break;
    }
    if (auto* fpc = dyn_cast< ar::FunctionPointerConstant >(fn_val)) {
      once_func = fpc->function();
    }
  }
  if (once_func != nullptr) {
    // Key the once-instance by the ONCE-CONTROL object (arg0), not the
    // callback name or call site: pthread_once runs the callback once PER
    // distinct control. Resolve the control STATICALLY (peel PointerShift/
    // Bitcast to a GlobalVariable) — the runtime points-to is TOP during the
    // first thread's analysis (globals not yet materialized), so keying by it
    // would split ONE control into two instances and defeat "all instances
    // joined" (87-once/02 FP). A non-global control (optr loaded from a
    // pointer global) is ambiguous → keyed by call site, never joinable.
    std::uint64_t key = 0;
    bool joinable = false;
    if (call->arg_begin() != call->arg_end()) {
      ar::Value* ctrl_arg = *call->arg_begin();
      for (int depth = 0; depth < 16; ++depth) {
        if (auto* gv = dyn_cast< ar::GlobalVariable >(ctrl_arg)) {
          key = ikos::core::IndexableTraits< MemoryLocation* >::index(
              eng.ctx().mem_factory->get_global(gv));
          joinable = true;
          break;
        }
        auto* iv = dyn_cast< ar::InternalVariable >(ctrl_arg);
        if (iv == nullptr) {
          break;
        }
        ar::Statement* def = unique_def(iv);
        if (auto* ps = dyn_cast_or_null< ar::PointerShift >(def)) {
          ctrl_arg = ps->pointer();
        } else if (auto* un = dyn_cast_or_null< ar::UnaryOperation >(def)) {
          switch (un->op()) {
            case ar::UnaryOperation::Bitcast:
            case ar::UnaryOperation::PtrToUI:
            case ar::UnaryOperation::UIToPtr:
              ctrl_arg = un->operand();
              break;
            default:
              ctrl_arg = nullptr;
          }
        } else {
          break; // Load / call / phi: ambiguous control
        }
        if (ctrl_arg == nullptr) {
          break;
        }
      }
    }
    if (!joinable) {
      // Ambiguous control: key by the (stable) call-site identity so the
      // callback gets a distinct, never-joined instance per site.
      key = reinterpret_cast< std::uint64_t >(call);
    }
    std::uint64_t sid =
        eng.ctx().concurrent_env.register_once_thread(once_func, key, joinable);
    if (sid != 0) {
      eng.lockset().add_joined_thread(sid);
    }
  }
  // The callback returns void and pthread_once has no data side effects; only
  // the int return code (if any) is nondet.
  if (call->has_result()) {
    const Literal& ret = eng.lit_factory().get(call->result());
    if (ret.is_scalar()) {
      ikos_assert_msg(ret.scalar().is_var(),
                      "left hand side is not a variable");
      eng.data().normal().scalar_assign_nondet(ret.scalar().var());
    }
  }
}

/// \brief Execute pthread_self().
///
/// pthread_self returns the CALLING thread's id. For the MAIN thread (the
/// entry function, whose call-context chain roots in "main") return the
/// reserved kMainThreadSid, so a later `pthread_join(mainid)` in a child can
/// recognise it and establish the "child joined main ⇒ main's writes HB the
/// child's post-join writes" edge (51-threadjoins/09-join-main.c). For a
/// spawned thread, a self-join carries no race-detection meaning, so return a
/// nondet value (sound: a nondet handle cannot be matched by pthread_join).
template < typename E >
void exec_pthread_self(E& eng, ar::CallBase* call) {
  // Determine the enclosing thread entry: walk the call-context chain UP to
  // the outermost non-empty frame — its call statement's containing function
  // is the thread entry (main / t_fun), mirroring DataRaceChecker::
  // current_thread_id. An empty chain means we are directly in the entry (main).
  bool in_main = true;
  const CallContext* cc = eng.call_context();
  const CallContext* outermost = nullptr;
  while (cc != nullptr && cc->has_parent()) {
    outermost = cc;
    cc = cc->parent();
  }
  if (outermost != nullptr) {
    if (ar::Function* fn = outermost->call()->code()->function_or_null()) {
      in_main = (fn->name() == "main");
    }
  }
  if (call->has_result()) {
    const Literal& ret = eng.lit_factory().get(call->result());
    if (ret.is_scalar() && ret.scalar().is_var()) {
      if (in_main) {
        ar::Type* rt = ret.scalar().var()->type();
        auto* ity = ar::cast< ar::IntegerType >(rt);
        ikos_assert_msg(ity != nullptr, "pthread_self result is not an int");
        core::MachineInt mid(static_cast< std::uint64_t >(core::kMainThreadSid),
                             ity->bit_width(), ity->sign());
        eng.data().normal().int_assign(ret.scalar().var(), mid);
      } else {
        eng.data().normal().scalar_assign_nondet(ret.scalar().var());
      }
    }
  }
}

/// @}


/// \brief Handle a hardware-atomic / pseudo-lock extern call (__sync_*,
/// __atomic_*, __VERIFIER_atomic_*).
///
/// Sound model: when a thread executes a hardware-atomic operation, the CPU
/// itself acts as a lock for the duration of the call. We approximate this by
/// injecting a synthetic pseudo-lock (a reserved, fixed address) into the
/// lockset so that any Load/Store happening during (or in the section
/// delimited by) the atomic region shares the same lock and is therefore never
/// reported as a race. The pseudo-lock address is constant (0xA7E0AD1C) so all
/// atomic sites collapse onto the same lock.
///
/// Distinguish three sub-cases (see `classify_atomic_intrinsic`):
///   1. SingleCall atomics: pseudo-lock wraps just the call.
///   2. SectionBegin markers: pseudo-lock is added and LEFT ON until the
///      matching end/release.
///   3. SectionEnd markers: pseudo-lock (assumed on since the matching
///      begin/acquire) is removed AFTER exec.
///
/// \return True when the call was handled (SectionBegin/End/SingleCall);
///         false for non-atomic calls (caller falls through to the default
///         unknown-call modelling).
template < typename E >
bool exec_atomic_extern_call(E& eng,
                             ar::CallBase* call,
                             const std::string& func_name) {
  switch (classify_atomic_intrinsic(func_name)) {
    case AtomicKind::SectionBegin:
      eng.lockset().add_lock(PSEUDO_ATOMIC_LOCK);
      eng.exec_unknown_call(call,
                            /* may_write_params = */ true,
                            /* ignore_unknown_write = */ true,
                            /* may_write_globals = */ false,
                            /* may_throw_exc = */ false);
      return true;
    case AtomicKind::SectionEnd:
      eng.exec_unknown_call(call,
                            /* may_write_params = */ true,
                            /* ignore_unknown_write = */ true,
                            /* may_write_globals = */ false,
                            /* may_throw_exc = */ false);
      eng.lockset().remove_lock(PSEUDO_ATOMIC_LOCK);
      return true;
    case AtomicKind::SingleCall:
      eng.lockset().add_lock(PSEUDO_ATOMIC_LOCK);
      eng.exec_unknown_call(call,
                            /* may_write_params = */ true,
                            /* ignore_unknown_write = */ true,
                            /* may_write_globals = */ false,
                            /* may_throw_exc = */ false);
      eng.lockset().remove_lock(PSEUDO_ATOMIC_LOCK);
      return true;
    case AtomicKind::None:
      return false;
  }
  ikos_unreachable("unreachable");
}

/// \brief Inject the pseudo-lock into the caller's invariant for a CALL TO A
/// DEFINITION (match_down side of the pseudo-lock model).
///
/// For a CALL TO A DEFINITION (e.g., __VERIFIER_atomic_acquire, which has a
/// body) the body is analyzed as a SEPARATE function fixpoint whose initial
/// state is the caller's invariant as of the call site. We inject the
/// pseudo-lock into this initial state so Load/Store statements inside the
/// body are recorded with the pseudo-lock in their snapshot, eliminating
/// spurious races on internal synchronization flags.
///
/// For SECTION MARKERS (acquire/release, begin/end), the pseudo-lock is also
/// kept in the caller's state across the call (not removed by match_up) so
/// that intervening statements in the caller — the actual critical-section
/// body — also see the pseudo-lock. For SINGLE-CALL ATOMICS the pseudo-lock
/// wraps the call only: match_down adds it, match_up removes it.
template < typename E >
void add_atomic_pseudo_lock(E& eng, ar::Function* called) {
  if (called == nullptr) {
    return;
  }
  switch (classify_atomic_intrinsic(called->name())) {
    case AtomicKind::SectionBegin:
    case AtomicKind::SingleCall:
      eng.lockset().add_lock(PSEUDO_ATOMIC_LOCK);
      break;
    case AtomicKind::SectionEnd:
    case AtomicKind::None:
      // SectionEnd: pseudo-lock was added by the matching begin/acquire;
      // leave it across the call so match_up can remove it.
      // None: not an atomic primitive.
      break;
  }
}

/// \brief Remove the pseudo-lock after an atomic definition call (match_up
/// side of the pseudo-lock model).
///
/// For SECTION-END markers, the pseudo-lock was added by the matching
/// acquire/begin; remove it so the caller's state after the call no longer
/// carries the atomic section. For SINGLE-CALL atomics (GCC builtins), the
/// pseudo-lock was added in match_down; remove it here.
template < typename E >
void remove_atomic_pseudo_lock(E& eng, ar::Function* fn) {
  if (fn == nullptr) {
    return;
  }
  switch (classify_atomic_intrinsic(fn->name())) {
    case AtomicKind::SectionEnd:
    case AtomicKind::SingleCall:
      eng.lockset().remove_lock(PSEUDO_ATOMIC_LOCK);
      break;
    case AtomicKind::SectionBegin:
    case AtomicKind::None:
      // SectionBegin: caller keeps the pseudo-lock until the matching end
      // marker closes the section. None: not an atomic primitive.
      break;
  }
}

/// \brief Replay a pending conditional lock (Comparison executor).
///
/// Recorded by the `if (i) lock(m)` detection in the TRUE branch: this
/// Comparison sits in the FALSE branch and must carry the same "m held iff
/// guard != 0" fact so the join's must-lockset intersection PRESERVES the
/// correlation (on this guard == 0 path the fact is vacuously true) instead of
/// dropping it (04-mutex_07-ps_nr / 17-ps_add1_nr.c).
template < typename E >
void on_comparison_cond_lock(E& eng, ar::Comparison* s) {
  auto& env = eng.ctx().concurrent_env;
  const auto* pend = env.pending_cond_locks(s->parent());
  if (pend != nullptr) {
    for (const auto& p : *pend) {
      eng.lockset().add_cond_lock(
          p.guard, p.lock_addr,
          p.is_nonzero ? core::lockset::CondPolarity::HELD_IF_NONZERO
                       : core::lockset::CondPolarity::HELD_IF_ZERO);
    }
  }
}

/// \brief Shared-read uninit decoupling (Load executor).
///
/// A LOAD from SHARED memory may be initialized by another thread under a
/// shared lock, so the per-thread uninit domain must NOT keep
/// "definitely-uninitialized" for it — a downstream `uninit_assert_initialized`
/// would otherwise bottom out the whole continuation
/// (02-base_24-malloc_races.c FN). Precise shared criterion: GLOBAL addresses,
/// DynAllocs reachable through the global pointer board, and escaped locals —
/// thread-private heap keeps its uninit precision.
template < typename E >
void on_load_shared_uninit(E& eng,
                           ar::Load* s,
                           const ScalarLit& ptr,
                           const ScalarLit& lhs) {
  auto rpts = eng.data().normal().pointer_to_points_to(ptr.var());
  bool shared = false;
  if (!rpts.is_top() && !rpts.is_bottom()) {
    auto& env = eng.ctx().concurrent_env;
    for (MemoryLocation* ml : rpts) {
      if (isa< GlobalMemoryLocation >(ml) || env.is_escaped(ml) ||
          env.is_global_pointer_target(ml)) {
        shared = true;
        break;
      }
    }
  }
  // Also relax when the read SOURCE is a global variable directly
  // (e.g. `%p = load @v` reading a global POINTER whose points-to is
  // TOP in this thread's local view — another thread wrote it, so the
  // value is cross-thread SHARED even though its target is unknown
  // locally).
  if (!shared && isa< ar::GlobalVariable >(s->operand())) {
    shared = true;
  }
  if (shared) {
    eng.data().normal().uninit_assign_maybe(lhs.var());
  }
}

/// \brief Pointer blackboard restoration (Load executor).
///
/// A global POINTER's DynAlloc points-to is invisible to the interval-only
/// flat board, so `mem_read` materializes an empty/TOP pointer. Restore the
/// board's points-to onto lhs (02-base_24-malloc_races.c FN: t_fun's `*y`
/// otherwise dereferences to an empty set).
template < typename E >
void on_load_pointer_restore(E& eng,
                             const ScalarLit& ptr,
                             const ScalarLit& lhs) {
  auto src_pts = eng.data().normal().pointer_to_points_to(ptr.var());
  if (src_pts.is_set()) {
    auto& env = eng.ctx().concurrent_env;
    for (MemoryLocation* ml : src_pts) {
      auto* gml = dyn_cast< GlobalMemoryLocation >(ml);
      if (gml == nullptr) {
        // Heap-field pointer restore (09-regions_03-list2_rc.c FN):
        // a DynAlloc field (`A->next`) may hold a cross-thread
        // pointer written on the heap board; restore it so the
        // downstream `->datum` deref lands on the shared node.
        auto* dal = dyn_cast< DynAllocMemoryLocation >(ml);
        if (dal == nullptr) {
          continue;
        }
        auto src_ptr = eng.data().normal().pointer_to_pointer(ptr.var());
        // Variable element offset (`B[i]`) reads the sentinel "whole array"
        // key (heap_pointer_key packs 0xFFFFFFFF), restoring the JOINed
        // pointer instead of falling through to the memory domain's ⊤.
        std::uint64_t key = heap_pointer_key(
            static_cast< std::uint64_t >(
                core::IndexableTraits< MemoryLocation* >::index(dal)),
            src_ptr.offset());
        core::PointsToSet< MemoryLocation* > tgt_pts =
            core::PointsToSet< MemoryLocation* >::empty();
        core::machine_int::Interval tgt_offset =
            core::machine_int::Interval::bottom(64, core::Signed);
        if (!env.get_heap_pointer(key, tgt_pts, tgt_offset) ||
            !tgt_pts.is_set() || tgt_pts.size() == 0) {
          continue;
        }
        if (tgt_pts.size() >
            ikos::core::ConcurrentGlobalEnv::kPtrBoardWidenThreshold) {
          continue;
        }
        // Strong-bind the whole may-set + offset (same union-fold as
        // the global path below): reset to nondet then refine.
        eng.data().normal().pointer_assign_nondet(lhs.var());
        eng.data().normal().pointer_refine(lhs.var(), tgt_pts, tgt_offset);
        continue;
      }
      core::PointsToSet< MemoryLocation* > tgt_pts =
          core::PointsToSet< MemoryLocation* >::empty();
      core::machine_int::Interval tgt_offset =
          core::machine_int::Interval::bottom(64, core::Signed);
      if (!env.get_global_pointer(gml, tgt_pts, tgt_offset)) {
        continue;
      }
      if (tgt_pts.is_top()) {
        // TOP board entry (a TOP write or board-widened): the global
        // pointer may alias ANY cell — conservatively leave lhs as TOP
        // (universal access), never mask it to empty.
        eng.data().normal().pointer_assign_nondet(lhs.var());
        continue;
      }
      if (tgt_pts.size() == 0) {
        continue;
      }
      // Region fold (union-fold): STRONGLY bind the FULL points-to
      // set with its transported byte offset in one shot — not the
      // first target only. `pointer_refine` INTERSECTS, so first
      // reset lhs to nondet (TOP) — mem_read left it bottom (empty local
      // cell for a global pointer) or TOP; refining a bottom stays bottom
      // (the 02-base_24-malloc_races.c family FN). Reset->refine == strong
      // assign of the whole may-set + offset.
      eng.data().normal().pointer_assign_nondet(lhs.var());
      eng.data().normal().pointer_refine(lhs.var(), tgt_pts, tgt_offset);
    }
  }
}

/// \brief Mark a pointer-typed LOAD from global memory as cross-thread
/// (Load executor, FS+FI hybrid registry).
///
/// The value may reflect stores performed by OTHER threads — mark it so the
/// indirect-call resolver may join the FPA set for thread entries.
/// Argument-passed pointers (thread-local precision) never go through this
/// path. NOTE: this belongs to the thread-modular engine, NOT the (removed)
/// relational DBM — it was previously mis-gated on
/// `enable_relational_interference`.
template < typename E >
void on_load_glob_flown(E& eng, const ScalarLit& ptr, const ScalarLit& lhs) {
  auto src_pts = eng.data().normal().pointer_to_points_to(ptr.var());
  if (!src_pts.is_top() && !src_pts.is_bottom()) {
    for (MemoryLocation* ml : src_pts) {
      if (isa< GlobalMemoryLocation >(ml)) {
        eng.ctx().concurrent_env
            .mark_glob_flown(lhs.var());
        break;
      }
    }
  }
}

/// \brief Privatization probe (Store executor, Bug 2/3 first-principles
/// rewrite, Stage 3).
///
/// Two and only two cases determine whether an unlocked-store probe is fired
/// or a PBR private-cache write is recorded:
///
///   1. `held_mutexes.empty()` — no mutex visible (either TOP of the lockset
///      lattice, or concrete-empty). We have no proof that any lock protects
///      this access — FORCED publish to the global blackboard so concurrent
///      threads can observe the write as interference. Sound
///      over-approximation.
///
///   2. `held_mutexes` non-empty — at least one mutex is must-held — FORCED
///      defer into the PBR privatized registry under each held lock.
///      Publication to the blackboard waits for the unlock.
///
/// No `lockset_unknown` inverted branches, no `!lockset_unknown` ternaries.
/// The empty-vs-non-empty check on the snapshot IS the PBR gate.
template < typename E >
void unfresh_pointer_targets(E& eng, const ScalarLit& rhs) {
  // Remove every heap node `rhs` may point to from the flow-sensitive "fresh"
  // set: storing `rhs` into a shared location PUBLISHES its targets (goblint
  // fresh-bullet materialization). Soundness: a published node is cross-thread
  // reachable, so its accesses must race like any shared cell.
  auto rhs_pts = eng.data().normal().pointer_to_points_to(rhs.var());
  if (!rhs_pts.is_set()) {
    return;
  }
  for (MemoryLocation* tgt : rhs_pts) {
    if (isa< DynAllocMemoryLocation >(tgt)) {
      eng.lockset().remove_heap_node_fresh(
          core::IndexableTraits< MemoryLocation* >::index(tgt));
    }
  }
}

template < typename E >
void on_store_privatize(E& eng,
                        const ScalarLit& ptr,
                        const Literal& val) {
  auto points_to = eng.data().normal().pointer_to_points_to(ptr.var());
  if (!points_to.is_top() && !points_to.is_bottom()) {
    auto& env = eng.ctx().concurrent_env;
    auto held_mutexes = eng.lockset().held_mutexes();
    const bool has_locks = !held_mutexes.empty();

    for (MemoryLocation* loc : points_to) {
      // Heap-field pointer propagation (09-regions_03-list2_rc.c FN):
      // a DynAlloc field (`A->next = p`) is invisible to the global-only
      // boards, so a cross-thread deref through it loses the datum
      // access. Propagate pointer-typed writes onto the heap pointer
      // board keyed by (alloc-site, field-offset) — precise only when
      // the field offset is a singleton; imprecise fields are skipped
      // (no worse than today's whole-DynAlloc skip).
      if (isa< DynAllocMemoryLocation >(loc)) {
        if (val.is_scalar()) {
          const ScalarLit& rhs = val.scalar();
          if (rhs.is_pointer_var()) {
            auto store_ptr =
                eng.data().normal().pointer_to_pointer(ptr.var());
            auto rhs_ptr =
                eng.data().normal().pointer_to_pointer(rhs.var());
            // Variable element offset (`B[i] = p`) now lands on the sentinel
            // "whole array" key (heap_pointer_key packs 0xFFFFFFFF), so the
            // pointer is JOINed onto one summarised entry instead of dropped.
            std::uint64_t key = heap_pointer_key(
                static_cast< std::uint64_t >(
                    core::IndexableTraits< MemoryLocation* >::index(loc)),
                store_ptr.offset());
            env.join_heap_pointer(key, rhs_ptr.points_to(),
                                  rhs_ptr.offset());
            // Heap-node publication: if this heap node (the store destination)
            // is NOT fresh (i.e. it is already shared), then storing `rhs`
            // into its field publishes `rhs`'s targets too (`A->next = p` with
            // A shared ⟹ p shared). A store into a still-fresh node publishes
            // nothing (both stay thread-private).
            if (!eng.lockset().is_heap_node_fresh(
                    core::IndexableTraits< MemoryLocation* >::index(loc))) {
              unfresh_pointer_targets(eng, rhs);
            }
          }
        }
        continue;
      }
      // Only process global variables
      if (!isa< GlobalMemoryLocation >(loc)) {
        continue;
      }
      std::uint64_t addr =
          core::IndexableTraits< MemoryLocation* >::index(loc);
      GlobalMemoryLocation* gv = cast< GlobalMemoryLocation >(loc);
      std::string var_name = gv->global_var()->name();

      // Extract interval from the written value. This stays the
      // FALLBACK + wrap-guard anchor — the expression above is the
      // precision channel, not a replacement.
      core::machine_int::Interval val_interval =
          core::machine_int::Interval::top(64, core::Signed);
      if (val.is_scalar()) {
        const ScalarLit& rhs = val.scalar();
        if (rhs.is_machine_int()) {
          // Constant: create interval [n, n]
          val_interval = core::machine_int::Interval(rhs.machine_int());
        } else if (rhs.is_machine_int_var()) {
          // Variable: get its interval
          val_interval = eng.data().normal().int_to_interval(rhs.var());
        } else if (rhs.is_pointer_var()) {
          // Pointer-typed RHS (`x = malloc(...)`): the flat interval
          // board cannot carry a DynAlloc points-to, so record it on
          // the dedicated pointer blackboard channel instead —
          // otherwise the child thread dereferences the global
          // pointer to an EMPTY set (02-base_24-malloc_races.c FN).
          // Offset-fidelity: capture pointer_to_pointer (points-to +
          // offset), NOT pointer_to_points_to (points-to only) — a
          // dropped field offset breaks the reader's container_of
          // inversion (09-regions_20-arrayloop2_rc.c FN).
          auto rhs_ptr = eng.data().normal().pointer_to_pointer(rhs.var());
          env.join_global_pointer(gv, rhs_ptr.points_to(), rhs_ptr.offset());
          // A store into a GLOBAL publishes `rhs`'s heap nodes.
          unfresh_pointer_targets(eng, rhs);
        }
      }

      if (!has_locks) {
        // Strict unlock-flushing (AstreeA-style): route unlocked
        // writes to the sentinel "no_lock" partition. They
        // accumulate during this iteration and are drained
        // atomically at the iteration boundary by the worklist
        // driver (see thread_modular.cpp, `flush_unlocked_partition`
        // inserted between the `for func in worklist` body and the
        // `is_dirty()` fixpoint check). Replaces the previous
        // per-store `join_global` which forced every unlocked write
        // to widen to TOP within the same iteration.
        env.record_privatized_write(
            ikos::core::ConcurrentGlobalEnv::kNoLockPartitionAddr, addr,
            val_interval);
        if (log::is_enabled_for(LogLevel::Debug)) {
          log::msg() << "[Strict Flush] Deferred unlocked write to global: "
                     << var_name << " (val_interval=" << val_interval << ")"
                     << "\n";
        }
      } else {
        // Case 2: PBR (Goblint SAS'21) deferral. Register the
        // (addr, val) pair against every held lock; flush on unlock
        // routes the aggregated value into `_lock_partition[lock]`
        // (AstreeA-style interference), NOT into the flat blackboard.
        // Reads by lock-aware `read_global_with_lockset` consult
        // these partitions in priority order.
        //
        // Approximation (CI-3): we register against every held lock
        // rather than only the lock that statically protects `addr`.
        // IKOS lacks an `address -> protecting_lock` alias mapping;
        // over-registration only widens the published value, never
        // misses a race.
        for (std::uint64_t lock_addr : held_mutexes) {
          env.record_privatized_write(lock_addr, addr, val_interval);
        }
        if (log::is_enabled_for(LogLevel::Debug)) {
          log::msg() << "[PBR] Deferred write to global variable: "
                     << var_name << " under " << held_mutexes.size()
                     << " mutex(es) (val_interval=" << val_interval << ")"
                     << "\n";
        }
      }
    }
  }
}

/// \brief pthread_create detection and modelling.
///
/// Concurrency is opt-in: only run when the thread-modular driver is enabled.
/// Registers the thread function, updates the create-happens-before digest,
/// maps the pthread_t handle, marks *handle initialized, and records the
/// spawned-argument points-to for IPA context binding. pthread_create has no
/// side effects on user state other than writing to *handle — the caller
/// deliberately skips `exec_unknown_extern_call` so the thread body is
/// explored independently by the concurrent inliner.
template < typename E >
void exec_pthread_create(E& eng, ar::CallBase* call) {
  // pthread_create(&thread, &attr, start_routine, arg)
  // The 3rd argument (index 2) is the thread function pointer.
  // The 1st argument is the pthread_t* that receives the handle.
  std::string thread_func_name;
  ar::Function* thread_func = nullptr;
  bool spawn_resolved = false;
  // Distinguishing create site. Defaults to the pthread_create statement; when
  // the thread function is resolved through a WRAPPER formal it becomes the
  // wrapper's call site, so each call to the wrapper is a DISTINCT instance.
  ar::CallBase* create_site = call;
  if (call->arg_begin() + 2 != call->arg_end()) {
    ar::Value* thread_func_val = *(call->arg_begin() + 2);
    CallContext* cc = eng.call_context();
    // Resolve the thread function through value-preserving casts (a signature
    // mismatch, 03-practical_32-smtprc-tid.c) and through WRAPPER formals (a
    // `my_pthread_create` that just forwards start_routine to pthread_create).
    for (int depth = 0; depth < 8; ++depth) {
      if (isa< ar::FunctionPointerConstant >(thread_func_val)) {
        break;
      }
      auto* iv = dyn_cast< ar::InternalVariable >(thread_func_val);
      if (iv == nullptr) {
        break;
      }
      ar::Statement* def = unique_def(iv);
      if (auto* un = dyn_cast_or_null< ar::UnaryOperation >(def)) {
        if (un->op() == ar::UnaryOperation::Bitcast) {
          thread_func_val = un->operand();
          continue;
        }
      }
      // Formal parameter (thread-create wrapper): resolve to the actual
      // argument at the current call site and keep peeling; the wrapper's call
      // site becomes the distinguishing create site.
      bool resolved = false;
      if (cc != nullptr && !cc->empty()) {
        ar::Code* code = iv->code();
        ar::Function* fun = code ? code->function_or_null() : nullptr;
        if (fun != nullptr) {
          for (std::size_t i = 0; i < fun->num_parameters(); ++i) {
            if (fun->param(i) != iv) {
              continue;
            }
            auto* c = dyn_cast< ar::CallBase >(cc->call());
            auto* fpc = c ? dyn_cast_or_null< ar::FunctionPointerConstant >(
                                c->called())
                          : nullptr;
            if (c != nullptr && fpc != nullptr && fpc->function() == fun &&
                i < c->num_arguments()) {
              thread_func_val = c->argument(i);
              create_site = c; // wrapper call site distinguishes instances
              cc = cc->parent();
              resolved = true;
            }
            break; // iv is param i (resolved or not); stop scanning params
          }
        }
      }
      if (!resolved) {
        break;
      }
    }
    if (isa< ar::FunctionPointerConstant >(thread_func_val)) {
      auto fpc = cast< ar::FunctionPointerConstant >(thread_func_val);
      if (fpc->function()) {
        thread_func = fpc->function();
        thread_func_name = fpc->function()->name();
        spawn_resolved = true;
        eng.ctx().concurrent_env.register_thread_func(
            fpc->function(), create_site);
      } else {
        log::msg() << "[Concurrency] Discovered pthread_create with dynamic thread function"
                   << "\n";
      }
    } else {
      log::msg() << "[Concurrency] Discovered pthread_create with non-constant thread function"
                 << "\n";
    }
  }

  // Create-happens-before digest: from THIS point on, the parent
  // has spawned this thread — parent writes AFTER this point are
  // concurrent with the child; writes BEFORE it (digest not yet
  // containing the child) are HB-before everything in the child.
  if (spawn_resolved) {
    eng.lockset().add_spawned_thread(thread_func_name);
  } else {
    // Indirect spawn (non-constant / unresolvable thread function): we
    // cannot name the child, so the create-edge HB "the parent never
    // spawned the child" would be unsound (FN). Mark the spawned digest
    // TOP — "may have spawned ANY thread" — so the checker refuses the
    // create-edge skip for every child.
    eng.lockset().set_spawned_top();
  }

  // Thread-instance id of this create site (0 when the spawn was indirect /
  // the thread function could not be resolved to a constant). Stored into the
  // pthread_t handle below so pthread_join can read it back.
  std::uint64_t sid = 0;
  if (spawn_resolved) {
    sid = eng.ctx().concurrent_env.site_id(create_site);
    eng.lockset().add_spawned_instance(sid);
  }

  // Handle initialization: pthread_create writes the thread-instance id into
  // *handle (arg 0). The per-thread uninit domain does not see that write (it
  // happens "inside" the libc call), so the handle local would otherwise stay
  // "uninitialized" and a later pthread_join(t, ...) reads it as uninitialized
  // — bottoming out and killing the thread-entry discovery before the child's
  // stores are analysed (pthread/singleton-b.c regresses RACE->SAFE). Writing
  // the site id (or a nondet value for an indirect spawn) marks the pointee
  // initialized.
  {
    ar::Value* handle_arg = *call->arg_begin();
    const auto& handle_lit = eng.lit_factory().get(handle_arg);
    if (handle_lit.is_scalar()) {
      const auto& scalar_lit = handle_lit.scalar();
      if (scalar_lit.is_pointer_var()) {
        auto pts = eng.data().normal().pointer_to_points_to(scalar_lit.var());
        if (pts.is_set()) {
          for (MemoryLocation* loc : pts) {
            if (auto* lml = dyn_cast< LocalMemoryLocation >(loc)) {
              // Only SCALAR handles — an ARRAY of pthread_t (`id[i]`)
              // collapses to a variable-index deref whose points-to is TOP
              // (skipped above) or resolves to a cell (aggregate), which
              // `scalar_assign_nondet` must not touch.
              Variable* var = eng.var_factory().get_local(lml->local_var());
              if (!isa< CellVariable >(var)) {
                // Store the thread-instance id as the pthread_t VALUE (a
                // concrete i64) into the handle CELL, so a later pthread_join
                // reads it back as a singleton (⇒ the exact instance). A
                // CONCRETE value is what lets an unknown call (`foo(&t)`
                // clobbers it to ⊤) be distinguished from a freshly-created
                // handle (07-trivial-unknowntid FN). The handle is an
                // address-taken local, so its value lives in the MEMORY cell,
                // written via mem_write (the old scalar_assign_nondet only
                // touched the ADDRESS var, not the content).
                ar::Type* pointee = lml->local_var()->type()->pointee();
                if (sid != 0 && pointee->is_integer()) {
                  ar::IntegerType* ity = ar::cast< ar::IntegerType >(pointee);
                  auto size = core::MachineInt(
                      eng.data_layout().store_size_in_bytes(pointee),
                      eng.data_layout().pointers.bit_width, core::Unsigned);
                  eng.data().normal().mem_write(
                      var,
                      core::Literal< Variable*, MemoryLocation* >::machine_int(
                          core::MachineInt(static_cast< std::uint64_t >(sid),
                                           ity->bit_width(), ity->sign())),
                      size);
                } else {
                  eng.data().normal().scalar_assign_nondet(var);
                }
              }
            }
          }
        }
      }
    }
  }

  // IPA context binding: record the points-to set of the 4th argument
  // (the thread's `arg` actual) so the child's formal parameter can be
  // bound to it at entry (04-mutex_45-escape_rc.c: `&i` escapes main's
  // stack into t_fun's `void* arg`; without this, `(*p)++` is TOP).
  if (thread_func != nullptr && call->arg_begin() + 3 != call->arg_end()) {
    ar::Value* arg_val = *(call->arg_begin() + 3);
    const auto& arg_lit = eng.lit_factory().get(arg_val);
    if (arg_lit.is_scalar()) {
      const auto& scalar_lit = arg_lit.scalar();
      if (scalar_lit.is_pointer_var()) {
        auto pts = eng.data().normal().pointer_to_points_to(scalar_lit.var());
        auto offset =
            eng.data().normal().pointer_to_pointer(scalar_lit.var()).offset();
        eng.ctx().concurrent_env.record_spawn_arg(
            thread_func, pts, offset);
        // Passing a pointer as the thread `arg` PUBLISHES its heap nodes: the
        // child dereferences the same cell (`&is[i]` escapes to the thread),
        // so they must leave the fresh set (per-thread-array-init-race FN).
        unfresh_pointer_targets(eng, scalar_lit);
      }
    }
  }
}

} // end namespace analyzer
} // end namespace ikos
