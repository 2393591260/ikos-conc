/*******************************************************************************
 *
 * \file
 * \brief Data race checker implementation
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
 * RECIPIENT'S USE OF THE SUBJECT SOFTWARE. RECIPIENT SHALL INDEMNIFY AND HOLD
 * HARMLESS THE UNITED STATES GOVERNMENT, ITS CONTRACTORS AND SUBCONTRACTORS,
 * AS WELL AS ANY PRIOR RECIPIENT, TO THE EXTENT PERMITTED BY LAW.
 * RECIPIENT'S SOLE REMEDY FOR ANY SUCH MATTER SHALL BE THE IMMEDIATE,
 * UNILATERAL TERMINATION OF THIS AGREEMENT.
 *
 ******************************************************************************/

#include <ikos/analyzer/checker/data_race.hpp>
#include <ikos/analyzer/analysis/execution_engine/symbolic_index.hpp>
#include <ikos/analyzer/support/cast.hpp>
#include <ikos/analyzer/util/log.hpp>
#include <ikos/analyzer/util/source_location.hpp>

#include <ikos/core/domain/concurrent_global_env.hpp>

#include <algorithm>
#include <map>
#include <sstream>
#include <vector>

namespace ikos {
namespace analyzer {

namespace {

/// \brief Canonical branch guard of a statement directly guarded by an
/// `if (guard)` branch: the base AR variable pointer (affine-peeled), or 0
/// when the statement is not guarded this way. Mirrors the lock-side branch
/// detection in numerical.hpp: the access sits in the TRUE branch, and the
/// sibling (false branch) carries the Comparison `guard == 0`.
std::uint64_t branch_guard_of(ar::Statement* stmt) {
  ar::BasicBlock* bb = stmt->parent();
  if (bb == nullptr || bb->num_predecessors() != 1) {
    return 0;
  }
  ar::BasicBlock* pred = *bb->predecessor_begin();
  if (pred->num_successors() != 2) {
    return 0;
  }
  auto sit = pred->successor_begin();
  ar::BasicBlock* s0 = *sit;
  ++sit;
  ar::BasicBlock* s1 = *sit;
  ar::BasicBlock* sibling = (s0 == bb) ? s1 : s0;
  const ar::Comparison* cmp = nullptr;
  bool sibling_has_call = false;
  for (ar::Statement* ss : *sibling) {
    if (auto* c = dyn_cast< ar::Comparison >(ss)) {
      cmp = c;
    } else if (isa< ar::CallBase >(ss)) {
      sibling_has_call = true;
    }
  }
  if (cmp == nullptr || sibling_has_call) {
    return 0;
  }
  auto* rc = dyn_cast< ar::IntegerConstant >(cmp->right());
  if (rc == nullptr || !rc->value().is_zero()) {
    return 0;
  }
  // The access is in the TRUE branch: it executes iff `guard != 0`. Only that
  // polarity (sibling `guard == 0`) can match a HELD_IF_NONZERO cond-lock.
  switch (cmp->predicate()) {
    case ar::Comparison::SIEQ:
    case ar::Comparison::UIEQ:
      break;
    case ar::Comparison::SINE:
    case ar::Comparison::UINE:
      return 0;
    default:
      return 0;
  }
  LinearIndex lin;
  if (resolve_linear_index(cmp->left(), lin) && lin.coeff == 1 &&
      lin.const_term == 0) {
    return lin.var_id;
  }
  return 0;
}

/// \brief Resolve a pointer to the stable_id of its underlying global, or 0
/// when it is not (transitively) rooted in a global (flat-array summarization).
/// Follows constant-offset PointerShifts and value-preserving casts.
inline std::uint64_t resolve_global_base(ar::Value* v, Context& ctx) {
  for (int depth = 0; depth < 16; ++depth) {
    if (auto* gv = dyn_cast< ar::GlobalVariable >(v)) {
      return core::IndexableTraits< MemoryLocation* >::index(
          ctx.mem_factory->get_global(gv));
    }
    auto* iv = dyn_cast< ar::InternalVariable >(v);
    if (iv == nullptr) {
      return 0;
    }
    ar::Statement* def = unique_def(iv);
    if (auto* ps = dyn_cast_or_null< ar::PointerShift >(def)) {
      v = ps->pointer();
    } else if (auto* un = dyn_cast_or_null< ar::UnaryOperation >(def)) {
      switch (un->op()) {
        case ar::UnaryOperation::Bitcast:
        case ar::UnaryOperation::PtrToUI:
        case ar::UnaryOperation::UIToPtr:
          v = un->operand();
          break;
        default:
          return 0;
      }
    } else {
      return 0;
    }
  }
  return 0;
}

/// \brief Classify the provenance of a ⊤ points-to access (phase 2 LIGHT
/// PROTOTYPE, region approximation — NOT a full SSA trace_root).
///
/// Follows PointerShift / Bitcast / Load one step at a time (no phi/formal
/// resolution, no multi-level cross-function walk). Returns:
/// - UnknownObject: the chain reaches a GLOBAL POINTER (`gv->type()->pointee()`
///   is a pointer) whose recorded value on the pointer blackboard is ⊤/absent
///   — the pointed-to heap identity is LOST (weaver `A[i]`, `A` set via a
///   helper). Such an access cannot be reliably analysed → UNKNOWN.
/// - KnownObject: the chain reaches a DynAlloc / stack local / global
///   value-type, or is unresolvable (phi/formal/nondet) — the object identity
///   is known (or conservatively treated as known), so the ⊤ is a compression
///   artefact (container_of / variable index) and the race must stay RACE.
TopProvenance classify_top_provenance(ar::Value* pointer, Context& ctx) {
  for (int depth = 0; depth < 16; ++depth) {
    if (auto* gv = dyn_cast< ar::GlobalVariable >(pointer)) {
      // Reached a global directly. Only a POINTER-typed global can lose its
      // pointed-to identity; an array/struct/int global's value is concrete.
      if (gv->type()->pointee()->is_pointer()) {
        auto* glob_loc = ctx.mem_factory->get_global(gv);
        core::PointsToSet< MemoryLocation* > pts =
            core::PointsToSet< MemoryLocation* >::empty();
        core::machine_int::Interval off =
            core::machine_int::Interval::bottom(64, core::Signed);
        auto& env = ctx.concurrent_env;
        if (!env.get_global_pointer(glob_loc, pts, off) || pts.is_top()) {
          return TopProvenance::UnknownObject;
        }
      }
      return TopProvenance::KnownObject;
    }
    auto* iv = dyn_cast< ar::InternalVariable >(pointer);
    if (iv == nullptr) {
      return TopProvenance::KnownObject; // local alloca / constant
    }
    ar::Statement* def = unique_def(iv);
    if (auto* ps = dyn_cast_or_null< ar::PointerShift >(def)) {
      pointer = ps->pointer();
    } else if (auto* un = dyn_cast_or_null< ar::UnaryOperation >(def)) {
      switch (un->op()) {
        case ar::UnaryOperation::Bitcast:
        case ar::UnaryOperation::PtrToUI:
        case ar::UnaryOperation::UIToPtr:
          pointer = un->operand();
          break;
        default:
          return TopProvenance::KnownObject;
      }
    } else if (auto* load = dyn_cast_or_null< ar::Load >(def)) {
      // Follow a load: `load @G` reads the global pointer G's VALUE (a
      // pointer). Continue the walk from the load's source operand — if that
      // reaches a pointer-typed global, the next iteration classifies it.
      pointer = load->operand();
    } else {
      return TopProvenance::KnownObject; // phi / formal / call / nondet
    }
  }
  return TopProvenance::KnownObject;
}

/// \brief True iff `v` is, transitively through assignments and value-preserving
/// casts, the result of a FRESH heap allocation — a direct call to
/// malloc/calloc/realloc, or a call to a function whose return value is itself a
/// fresh allocation (`new()`). Used by region_tainting_sound to tell a
/// slot-loading call (`lookup()`, which returns `slot[h].next`) from a fresh
/// allocator: only the former makes `p1->next = p2->next` a cross-slot splice
/// (09-regions_23-evilcollapse_rc.c).
inline bool is_fresh_alloc(ar::Value* v) {
  std::unordered_set< ar::Value* > seen;
  for (int depth = 0; depth < 16; ++depth) {
    auto* iv = dyn_cast< ar::InternalVariable >(v);
    if (iv == nullptr || !seen.insert(iv).second) {
      return false;
    }
    ar::Statement* def = unique_def(iv);
    if (def == nullptr) {
      return false; // phi/formal: not provably a fresh allocation
    }
    if (auto* as = dyn_cast< ar::Assignment >(def)) {
      v = as->operand();
      continue;
    }
    if (auto* un = dyn_cast< ar::UnaryOperation >(def)) {
      switch (un->op()) {
        case ar::UnaryOperation::Bitcast:
        case ar::UnaryOperation::PtrToUI:
        case ar::UnaryOperation::UIToPtr:
          v = un->operand();
          continue;
        default:
          return false;
      }
    }
    if (auto* call = dyn_cast< ar::CallBase >(def)) {
      auto* fpc =
          dyn_cast_or_null< ar::FunctionPointerConstant >(call->called());
      if (fpc == nullptr) {
        return false; // indirect call: not provably fresh
      }
      const std::string& name = fpc->function()->name();
      if (name == "malloc" || name == "calloc" || name == "realloc" ||
          name == "ar.libc.malloc" || name == "ar.libc.calloc" ||
          name == "ar.libc.realloc") {
        return true;
      }
      // A user function (`new`): fresh iff its return value is fresh.
      ar::Function* f = fpc->function();
      if (auto* body = f->body_or_null()) {
        for (ar::BasicBlock* bb : *body) {
          for (ar::Statement* st : *bb) {
            if (auto* rv = dyn_cast< ar::ReturnValue >(st)) {
              return rv->has_operand() && is_fresh_alloc(rv->operand());
            }
          }
        }
      }
      return false;
    }
    return false; // Load / PointerShift / other: not a fresh allocation
  }
  return false;
}

/// \brief Collect the flat-array slots a value derives from. Follows
/// container_of (constant-offset PointerShift), value-preserving casts, and
/// phi copies (Assignment). Loads are followed unconditionally: a GLOBAL
/// array load (`slot[i]`) yields its slot index, and a heap-node `->next`
/// load inherits its base pointer's region. The heap-node case is sound
/// because check_load/check_store only call region_of when
/// region_tainting_sound() holds — no cross-slot pointer aliasing
/// (09-regions_23-evilcollapse_rc.c) — so a node reached through slot[j]
/// provably stays in slot[j]'s list. An untraceable leaf (malloc/call
/// result) still sets `ambiguous`.
inline void region_collect(
    ar::Value* v,
    std::vector< core::ConcurrentGlobalEnv::SymbolicIndex >& out,
    std::unordered_set< ar::Value* >& visiting,
    bool& ambiguous,
    Context& ctx,
    CallContext* cc,
    std::int64_t acc = 0) {
  auto* iv = dyn_cast< ar::InternalVariable >(v);
  if (iv == nullptr) {
    return; // a global/constant directly: no array slot (not ambiguous)
  }
  if (!visiting.insert(iv).second) {
    return; // cycle: contributes nothing
  }
  ar::Code* code = iv->code();
  if (code != nullptr) {
    // Formal parameter (no def statement): resolve to the actual argument at
    // the current call site (09-regions_11-arraylist_nr.c: the inlined
    // `list_add(list, ...)` formal `list` is bound to `slot[i]` only in the
    // abstract domain, not syntactically). Unknown callee/argument ⇒ ambiguous.
    if (ar::Function* fun = code->function_or_null()) {
      for (std::size_t i = 0; i < fun->num_parameters(); ++i) {
        if (fun->param(i) != iv) {
          continue;
        }
        if (cc != nullptr && !cc->empty()) {
          auto* call = dyn_cast< ar::CallBase >(cc->call());
          auto* fpc = call ? dyn_cast_or_null< ar::FunctionPointerConstant >(
                                 call->called())
                           : nullptr;
          if (call != nullptr && fpc != nullptr && fpc->function() == fun &&
              i < call->num_arguments()) {
            region_collect(call->argument(i), out, visiting, ambiguous, ctx,
                           cc->parent(), acc);
          } else {
            ambiguous = true;
          }
        } else {
          ambiguous = true;
        }
        visiting.erase(iv);
        return;
      }
    }
    for (ar::BasicBlock* bb : *code) {
      for (ar::Statement* s : *bb) {
        if (s->result_or_null() != iv) {
          continue;
        }
        if (auto* ps = dyn_cast< ar::PointerShift >(s)) {
          std::uint64_t base_id = resolve_global_base(ps->pointer(), ctx);
          if (base_id != 0) {
            core::ConcurrentGlobalEnv::SymbolicIndex sym;
            if (symbolic_index_of_ps(ps, sym)) {
              // Fold in the element offsets peeled through to reach this root
              // (`entry[1].refs` advanced the slot by 1 — must match the lock
              // side, 06-symbeq_39-funloop_index_bad.c FN).
              sym.const_term += acc;
              out.push_back(sym); // region root: stop, do not propagate
              continue;
            }
          }
          if (base_id == 0) {
            // Polymorphic base pointer (`&s->field` where `s = x ? &A : &B`):
            // not rooted in a global. The instance identity IS the base
            // pointer itself — the SAME `s` the lock side keys on
            // (06-symbeq_11-equ_nr.c). Stop here instead of recursing into the
            // phi (which would under-approximate to {} and drop the identity).
            core::ConcurrentGlobalEnv::SymbolicIndex sym;
            if (symbolic_base_of(iv, sym)) {
              out.push_back(sym);
              continue;
            }
          }
          region_collect(ps->pointer(), out, visiting, ambiguous, ctx, cc,
                         acc + constant_element_offset(ps));
        } else if (auto* ld = dyn_cast< ar::Load >(s)) {
          // Follow the loaded-from address unconditionally. A GLOBAL array
          // pointer (`slot[i]`) yields its slot index; a heap-node `->next`
          // load inherits the region of its base pointer (`t` in
          // `p = t->next`, where `t = slot[j]`). The heap-node case is sound
          // because check_load/check_store only call region_of when
          // region_tainting_sound() holds — i.e. no cross-slot pointer
          // aliasing (09-regions_23-evilcollapse_rc.c) — so a node reached
          // through slot[j] provably stays in slot[j]'s list. If the operand
          // traces to a malloc/other untraceable leaf, the recursion sets
          // `ambiguous` deeper down, so no unsound single-slot region escapes.
          region_collect(ld->operand(), out, visiting, ambiguous, ctx, cc,
                         acc);
        } else if (auto* as = dyn_cast< ar::Assignment >(s)) {
          region_collect(as->operand(), out, visiting, ambiguous, ctx, cc, acc);
        } else if (auto* un = dyn_cast< ar::UnaryOperation >(s)) {
          switch (un->op()) {
            case ar::UnaryOperation::Bitcast:
            case ar::UnaryOperation::PtrToUI:
            case ar::UnaryOperation::UIToPtr:
              region_collect(un->operand(), out, visiting, ambiguous, ctx, cc,
                             acc);
              break;
            default:
              ambiguous = true;
          }
        } else if (auto* bin = dyn_cast< ar::BinaryOperation >(s)) {
          ar::Value* lhs = bin->left();
          ar::Value* rhs = bin->right();
          if (isa< ar::IntegerConstant >(rhs)) {
            region_collect(lhs, out, visiting, ambiguous, ctx, cc, acc);
          } else if (isa< ar::IntegerConstant >(lhs)) {
            region_collect(rhs, out, visiting, ambiguous, ctx, cc, acc);
          } else {
            ambiguous = true;
          }
        } else {
          ambiguous = true; // call / alloc / other untraceable pointer leaf
        }
      }
    }
  }
  visiting.erase(iv);
}

/// \brief Returns true iff `v` derives from exactly one flat-array slot (with
/// no ambiguous branch), filling `out` with that slot's SymbolicIndex.
inline bool region_of(ar::Value* v, Context& ctx, CallContext* cc,
                      core::ConcurrentGlobalEnv::SymbolicIndex& out) {
  std::vector< core::ConcurrentGlobalEnv::SymbolicIndex > regions;
  std::unordered_set< ar::Value* > visiting;
  bool ambiguous = false;
  region_collect(v, regions, visiting, ambiguous, ctx, cc);
  if (ambiguous || regions.empty()) {
    return false;
  }
  const auto& first = regions[0];
  for (const auto& r : regions) {
    if (r != first) {
      return false; // ambiguous: derives from multiple slots
    }
  }
  out = first;
  return true;
}

} // end anonymous namespace

DataRaceChecker::DataRaceChecker(Context& ctx) : Checker(ctx) {}

DataRaceChecker::~DataRaceChecker() {
  // Cross-slot heap-node sharing (12-arraycollapse_rc: `list_add(p, slot[j])`
  // then `list_add(p, slot[k])` stores one node into TWO slots) makes the
  // per-slot region/lock summarization unsound: a single region would match
  // the wrong slot's lock. Drop EVERY region (both the def-chain and the
  // forward-label ones) so self_locked cannot fire — the race must survive.
  if (!this->_dyn_alloc_ambiguous.empty()) {
    for (AccessRecord& rec : this->_accesses) {
      rec.has_region = false;
    }
  } else {
    // Backfill the forward region labels: WRITE accesses to a heap node that
    // was stored into a flat-array slot (recorded by check_store) get that
    // slot's region, so self_locked can match them against the lock's symbolic
    // index (11/13/17/19 race on `new`'s `p->datum = x`).
    for (AccessRecord& rec : this->_accesses) {
      if (rec.has_region || !rec.pts.is_set() || rec.pts.size() != 1) {
        continue;
      }
      auto* ml = *rec.pts.begin();
      if (!isa< DynAllocMemoryLocation >(ml)) {
        continue;
      }
      auto it = this->_dyn_alloc_region.find(
          core::IndexableTraits< MemoryLocation* >::index(ml));
      if (it != this->_dyn_alloc_region.end()) {
        rec.has_region = true;
        rec.region = it->second;
      }
    }
  }

  // Offline pairwise race detection across all logged accesses.
  //
  // Two accesses A and B form a potential race when:
  //   1. A.stmt != B.stmt                 (different statements)
  //   2. at least one is a Write          (W/W or W/R)
  //   3. A.pts and B.pts have non-empty intersection
  //   4. A.locks and B.locks have empty intersection
  //      (no common lock protecting the conflicting access)
  //
  // Happens-before short-circuit (VMCAI'26 + ESOP'23 style):
  //   - A and B from the SAME thread cannot race against each other, but
  //     ONLY if that thread function is "unique" (Thread Uniqueness
  //     abstraction: spawned exactly once across the program). The flag
  //     `is_unique_thread` is precomputed when the AccessRecord is logged,
  //     so this destructor never re-enters the global blackboard.
  //   - If A.joined_threads contains B.thread_id, then B finished before
  //     A started → HB(A, B) → no race.
  //   - Symmetrically for B.joined_threads contains A.thread_id.
  //
  // Aggregation strategy (IKOS-native): rather than emitting one
  // `_checks.insert(...)` per pair (and flooding the database), we
  // collect every detected race pair first, then group by the
  // conflicting memory location (the physical key around which races
  // occur) and emit a SINGLE aggregated report per group. The
  // `info` JsonDict therefore carries:
  //
  //   - "memory_location"        : human-readable name of the conflict
  //                                site (top-level, preserved for
  //                                downstream compatibility).
  //   - "access_a" / "access_b"  : the FIRST pair's side-info, kept as
  //                                a flat top-level field so any legacy
  //                                consumer that reads `info.access_a`
  //                                still gets a meaningful answer.
  //   - "conflicting_accesses"   : a JsonList of nested JsonDicts, one
  //                                per pair, each carrying the full
  //                                side-info for both A and B (so the
  //                                aggregated report is self-contained).
  //   - "num_pairs"              : integer count of pairs in this group.
  //   - "unique_lockset_intersection" : always "empty" here (otherwise
  //                                the pair would have been short-
  //                                circuited above); documented for
  //                                downstream parsers.
  //
  // Anchoring: each group's insert uses the Write statement of its
  // FIRST pair (preserving the previous IKOS behaviour for the
  // statement-level report) and the matching CallContext.

  // Per-pair metadata captured during the scan. We avoid allocating
  // JsonDicts here so the inner loop stays cheap; the group emits are
  // done once at the end.
  struct PendingRacePair {
    std::size_t i;
    std::size_t j;
    ar::Statement* report_stmt;
    CallContext* report_cc;
    std::string first_conflict_loc;
    JsonList conflicting_pts; // dump() of every cell in a.pts ∧ b.pts
    /// True when this pair rests on a TOP points-to (at least one side's
    /// points-to is ⊤), so the checker cannot DISTINGUISH which memory
    /// object conflicts. Such a pair is reported as `Result::Warning`
    /// ("unknown"), NOT `Result::Error` ("race") — the access is a model
    /// boundary, not a proven race. Soundness invariant: `Warning` here maps
    /// to SV-COMP UNKNOWN, NEVER to SAFE — it does not weaken the FN=0
    /// guarantee (see the demotion site below).
    bool is_unknown = false;
  };

  std::vector< PendingRacePair > all_pairs;

  // State the pairing precondition: two accesses on the SAME statement
  // from the same unique (single-instance) thread cannot race — skip
  // them. But a NON-unique thread function (spawned 2+ times) runs
  // concurrently with its own instances: instance A and instance B can
  // both be between the read and write of the very same statement
  // (10-synch_02-thread_nonunique.c: 10 spawns of `myglobal=42`).
  // Materialize that possibility by duplicating each such WRITE record
  // once, so the pairing loop below sees an (instance, instance) pair.
  // Unique-thread records are NOT duplicated — no self-race FPs.
  this->_accesses.reserve(this->_accesses.size() * 2);
  const std::size_t n_orig = this->_accesses.size();
  for (std::size_t i = 0; i < n_orig; ++i) {
    const AccessRecord& r = this->_accesses[i];
    if (r.kind == AccessKind::Write && !r.thread_id.empty() &&
        !r.is_unique_thread) {
      this->_accesses.push_back(r); // second concurrent instance
    }
  }

  if (!this->_creators_built) {
    this->build_thread_creators();
    this->_creators_built = true;
  }

  // Pre-compute, per access record, the set of thread entry functions whose
  // ALL instances this thread has joined at that point ("fully joined").
  // Instance-level join HB (28/29/30-join-array): joining tids[0] marks only
  // that instance; an access is HB-suppressed against another thread's access
  // only when EVERY instance of that thread function is joined — otherwise an
  // unjoined instance may still race. Indexed by the FINAL `_accesses` order
  // (after the non-unique write duplication above).
  std::vector< std::unordered_set< std::string > > joined_names(
      this->_accesses.size());
  {
    auto thread_funcs = this->_ctx.concurrent_env.get_all_thread_functions();
    for (std::size_t k = 0; k < this->_accesses.size(); ++k) {
      const auto& joined = this->_accesses[k].joined_threads;
      const auto& spawned_insts = this->_accesses[k].spawned_instances;
      bool spawned_top = this->_accesses[k].spawned_is_top;
      for (ar::Function* f : thread_funcs) {
        if (f == nullptr) {
          continue;
        }
        auto instances =
            this->_ctx.concurrent_env.get_func_instances(f->name());
        if (instances.empty()) {
          continue;
        }
        // "Not-yet-spawned" ordering is only meaningful when THIS thread is the
        // (direct) creator of f — a leaf thread that never spawns f must not be
        // treated as "ordered before f" merely because its spawn set is empty.
        auto cit = this->_thread_creators.find(f->name());
        bool is_creator =
            cit != this->_thread_creators.end() &&
            cit->second.count(this->_accesses[k].thread_id) != 0;
        bool all = true;
        for (std::uint64_t sid : instances) {
          bool joined_it = joined.count(sid) != 0;
          bool not_spawned =
              is_creator && !spawned_top && spawned_insts.count(sid) == 0;
          if (!joined_it && !not_spawned) {
            all = false;
            break;
          }
        }
        if (all) {
          joined_names[k].insert(f->name());
        }
      }
      // Joining the MAIN thread (pthread_self + pthread_join(mainid)): "main"
      // is not a spawned thread function, so it is not in the registry above.
      // Map the reserved main-thread id to the "main" name directly
      // (51-threadjoins/09-join-main.c).
      if (joined.count(core::kMainThreadSid) != 0) {
        joined_names[k].insert("main");
      }
    }
  }

  // Pre-pass: mark "private-init" writes (09-regions_02-list_nr.c). An
  // UNLOCKED write to a SINGLE DynAlloc that the same thread later accesses
  // while holding exactly ONE lock is an initialization preceding the
  // object's promotion to a locked/shared state. It happens-before any
  // locked read of that object by another thread: the promoting lock
  // serializes the two critical sections, and before promotion the object
  // is reachable only by its allocating thread (publication is itself a
  // locked write under that lock).
  for (std::size_t i = 0; i < this->_accesses.size(); ++i) {
    AccessRecord& w = this->_accesses[i];
    if (w.kind != AccessKind::Write || !w.locks.empty() ||
        w.thread_id.empty() || !w.pts.is_set() || w.pts.size() != 1 ||
        !isa< DynAllocMemoryLocation >(*w.pts.begin())) {
      continue;
    }
    MemoryLocation* o = *w.pts.begin();
    for (std::size_t j = i + 1; j < this->_accesses.size(); ++j) {
      const AccessRecord& l = this->_accesses[j];
      if (l.thread_id != w.thread_id || l.locks.size() != 1 ||
          !l.pts.is_set()) {
        continue;
      }
      bool touches = false;
      for (MemoryLocation* ml : l.pts) {
        if (ml == o) {
          touches = true;
          break;
        }
      }
      if (!touches) {
        continue;
      }
      w.private_init = true;
      w.promote_locks = l.locks;
      break;
    }
  }

  for (std::size_t i = 0; i < this->_accesses.size(); ++i) {
    const AccessRecord& a = this->_accesses[i];
    for (std::size_t j = i + 1; j < this->_accesses.size(); ++j) {
      const AccessRecord& b = this->_accesses[j];

      // Same statement: skip ONLY when both sides belong to the same
      // UNIQUE thread (single instance — one execution cannot race
      // itself). For non-unique threads the duplicates materialized
      // above represent distinct concurrent instances — do NOT skip.
      if (a.stmt == b.stmt) {
        if (a.thread_id == b.thread_id &&
            (a.is_unique_thread || b.is_unique_thread)) {
          continue;
        }
      }
      if (a.kind != AccessKind::Write && b.kind != AccessKind::Write) {
        continue;
      }

      // Happens-before: same thread AND that thread is unique ⇒ sequential.
      if (!a.thread_id.empty() && a.thread_id == b.thread_id &&
          a.is_unique_thread) {
        continue;
      }
      // Happens-before: A fully joined B (ALL instances of B's function) ⇒
      // B terminated before A's continuation. Instance-level: joining tids[0]
      // alone does NOT suppress tids[1]'s instance (28/29/30-join-array FN).
      if (!a.thread_id.empty() && !b.thread_id.empty() &&
          joined_names[i].count(b.thread_id)) {
        continue;
      }
      // Happens-before: B fully joined A ⇒ A terminated before B's
      // continuation.
      if (!a.thread_id.empty() && !b.thread_id.empty() &&
          joined_names[j].count(a.thread_id)) {
        continue;
      }
      // Happens-before (creation edge): when ONE side's thread was
      // created ONLY by the other side's thread, and the parent's
      // program-point digest does not list it, the parent's access
      // precedes the pthread_create that started the child — a parent
      // write before the create HB everything in the child
      // (04-mutex_36-trylock_nr.c: `end_time = ...` precedes both
      // creates). Tested SYMMETRICALLY: the record order in _accesses
      // follows checker visit order, so either side may play the
      // parent. The creator map is what makes an EMPTY digest
      // meaningful: without it, a thread that never spawns anything
      // would skip every cross-thread pair (classic two-thread races).
      if (!a.thread_id.empty() && !b.thread_id.empty() &&
          a.thread_id != b.thread_id) {
        // x happens-before y via a (possibly TRANSITIVE) create chain: x is a
        // direct-or-transitive creator of y, y has a unique DIRECT creator, and
        // x's access precedes x's own create on that chain (x has neither
        // spawned y nor any intermediate ancestor of y). The transitive leg is
        // main → t1_fun → t2_fun (10-synch_15-join_other_nr.c); the two
        // spawned-set guards are what keep it sound when x HAS already spawned
        // an intermediate ancestor.
        auto creates_before = [&](const AccessRecord& x,
                                  const AccessRecord& y) {
          auto dir = this->_thread_creators.find(y.thread_id);
          if (dir == this->_thread_creators.end() ||
              dir->second.size() != 1) {
            return false;
          }
          auto anc = this->_thread_ancestors.find(y.thread_id);
          if (anc == this->_thread_ancestors.end() ||
              !anc->second.count(x.thread_id)) {
            return false;
          }
          if (x.spawned_is_top || x.spawned_threads.count(y.thread_id)) {
            return false;
          }
          for (const std::string& s : x.spawned_threads) {
            if (anc->second.count(s)) {
              return false; // x already spawned an intermediate ancestor of y
            }
          }
          return true;
        };
        if (creates_before(a, b) || creates_before(b, a)) {
          continue;
        }
      }

      // Points-to intersection: ignore bottom (unreachable); TOP is the
      // identity of meet, so a TOP access conservatively pairs with every
      // other access (sound over-approximation, NOT masked to "nothing").
      if (a.pts.is_bottom() || b.pts.is_bottom()) {
        continue;
      }
      if (a.pts.meet(b.pts).is_empty()) {
        continue;
      }
      // C11 DRF: two ATOMIC accesses never race (atomicity is itself
      // synchronization). Exempt the pair regardless of locks/HB — this is a
      // direct corollary of the C11 data-race definition, so it only removes
      // false positives, never a real race.
      if (a.is_atomic && b.is_atomic) {
        continue;
      }
      // Happens-before (private-init publication, 09-regions_02-list_nr.c):
      // an unlocked initialization write precedes its thread's promotion of
      // the object under a single lock, so it happens-before a locked read
      // of the same object by another thread (the promoting lock serializes
      // the two critical sections).
      {
        auto hb_private_init = [](const AccessRecord& writer,
                                  const AccessRecord& reader) {
          if (!writer.private_init || writer.promote_locks.size() != 1 ||
              reader.locks.size() != 1 ||
              writer.promote_locks[0] != reader.locks[0]) {
            return false;
          }
          if (!reader.pts.is_set()) {
            return false; // TOP reader: the same-cell fact is not provable
          }
          std::uint64_t o = core::IndexableTraits< MemoryLocation* >::index(
              *writer.pts.begin());
          for (MemoryLocation* ml : reader.pts) {
            if (core::IndexableTraits< MemoryLocation* >::index(ml) == o) {
              return true;
            }
          }
          return false;
        };
        if (hb_private_init(a, b) || hb_private_init(b, a)) {
          continue;
        }
      }
      // Local self-consistency (flat-array + polymorphic-base FP): two
      // accesses to the SAME cell, each holding a lock that is provably on
      // ITS OWN instance, are mutually exclusive — "same cell ⟹ same
      // instance ⟹ same lock". Each side must be self-consistent on its own:
      //   (symbolic) a symbolic lock whose index/identity == the access's own
      //              region (09-regions_11-arraylist_nr.c / 06-symbeq_11-equ).
      //   (flat)     a field lock on the SAME object AND same struct instance
      //              as the access (concrete singleton only: rec.base != 0).
      // The "same cell" fact is the precise points-to meet above; "same cell
      // ⟹ same instance" is sound because region/base tainting is gated on no
      // cross-instance pointer aliasing.
      {
        auto& env = this->_ctx.concurrent_env;
        auto self_locked = [&](const AccessRecord& rec) {
          // Fast-path: the two match modes need, respectively, a symbolic
          // region (has_region) or a concrete singleton base (base != 0). With
          // neither, no lock can ever match — skip the per-lock
          // lock_symbolic_index / lock_instance lookups entirely. This is the
          // common case for heap-node accesses whose base is a malloc/call
          // result (region_collect leaves those region-less), and it
          // previously burned an env hash probe per held lock for nothing.
          if (!rec.has_region && rec.base == 0) {
            return false;
          }
          for (std::uint64_t key : rec.locks) {
            std::uint64_t base;
            core::ConcurrentGlobalEnv::SymbolicIndex sym;
            if (env.lock_symbolic_index(key, base, sym)) {
              if (rec.has_region && sym == rec.region) {
                return true; // symbolic lock on the access's own identity
              }
            } else if (rec.base != 0 && (key >> 32) == rec.base &&
                       env.lock_instance(key) == rec.instance) {
              return true; // flat field lock on the access's own instance
            }
          }
          return false;
        };
        if (self_locked(a) && self_locked(b)) {
          continue;
        }
      }
      // Field/element sensitivity on the DATA side: `data.x` and `data.y`
      // (or `data[3]` and `data[4]`) collapse to the same base location, but
      // their byte offsets are disjoint — provably distinct elements, so no
      // race. A top/imprecise offset overlaps everything (conservative).
      //
      // Skip the interval meet when either side is TOP: top overlaps any
      // interval, so `meet(...).is_bottom()` can never be true — the meet and
      // disjointness test would be pure wasted work. (Class B's `c.slot[j]`
      // accesses are exactly this top-offset case.)
      if (!a.offset.is_top() && !b.offset.is_top() &&
          a.offset.meet(b.offset).is_bottom()) {
        continue;
      }
      // Structural field disjointness under an OPAQUE base: when the abstract
      // offset is ⊤ (the base's points-to is ⊤, so `⊤ + field` collapsed to ⊤),
      // fall back to the compile-time field offset. Two DIFFERENT constant
      // offsets of the SAME struct type are disjoint bytes regardless of which
      // object the opaque base points to (04-mutex_78-type-array.c:
      // `getS()->field` vs `getS()->arr[1]`). The type must MATCH — fields of
      // different struct types are not comparable (their objects could overlap).
      if (a.structural_type != nullptr &&
          a.structural_type == b.structural_type &&
          a.structural_offset != b.structural_offset) {
        continue;
      }

      // Lockset intersection: a shared EXCLUSIVE lock (or an exclusive
      // lock meeting a read lock) makes the two accesses mutually
      // exclusive => no race. A shared READ-READ lock does NOT exclude —
      // pthread_rwlock_rdlock permits concurrent readers, so a write
      // performed under rdlock races (04-mutex_55-pt_rwlock_rr.c). A
      // pure read-read data pair was already skipped above.
      //
      // Each lock key packs (base stable_id << 32) | flat offset. A lock
      // protects an access iff it is on a DIFFERENT object (a global/whole
      // mutex — standard lockset semantics) OR on the SAME object AND SAME
      // struct instance (container_of region, via the side table). A field
      // lock on A[0].mutex must NOT protect A[1].datum
      // (06-symbeq_14-list_entry_rc.c: `s++` advances the access past the
      // locked instance).
      auto lock_protects = [this](std::uint64_t key,
                                  std::uint64_t base,
                                  std::uint64_t instance) {
        if ((key >> 32) != base) {
          return true; // different object → global lock
        }
        return this->_ctx.concurrent_env.lock_instance(key) ==
               instance;
      };
      auto& env = this->_ctx.concurrent_env;
      auto protects_both = [&](const std::vector< std::uint64_t >& x,
                               const std::vector< std::uint64_t >& y) {
        for (std::uint64_t lx : x) {
          std::uint64_t bx;
          core::ConcurrentGlobalEnv::SymbolicIndex sx;
          if (env.lock_symbolic_index(lx, bx, sx)) {
            continue; // symbolic lock: handled by the flat-array rule above
          }
          for (std::uint64_t ly : y) {
            std::uint64_t by;
            core::ConcurrentGlobalEnv::SymbolicIndex sy;
            if (env.lock_symbolic_index(ly, by, sy)) {
              continue;
            }
            if (lx == ly && lock_protects(lx, a.base, a.instance) &&
                lock_protects(ly, b.base, b.instance)) {
              return true;
            }
          }
        }
        return false;
      };
      if (protects_both(a.locks, b.locks) ||          // exclusive ∩ exclusive
          protects_both(a.locks, b.read_locks) ||     // exclusive ∩ read
          protects_both(a.read_locks, b.locks)) {     // read ∩ exclusive
        continue;
      }

      // Anchor the report to the Write (or to A if both are writes).
      ar::Statement* report_stmt =
          (a.kind == AccessKind::Write) ? a.stmt : b.stmt;
      CallContext* report_cc =
          (a.kind == AccessKind::Write) ? a.call_context : b.call_context;

      // Build the conflicting-pts JsonList (one entry per cell in
      // a.pts ∧ b.pts) and the human-readable location name.
      PendingRacePair pr;
      pr.i = i;
      pr.j = j;
      pr.report_stmt = report_stmt;
      pr.report_cc = report_cc;
      // Model-boundary demotion (phase 2, ⊤-provenance) is DISABLED. A ⊤
      // points-to on one side cannot distinguish "the ⊤ over-approximated an
      // UNRELATED object" (a false positive, e.g. weaver `A[i]` ⊤ vs `sum2`)
      // from "the ⊤ is a legitimate part of a REAL race" (e.g.
      // container_of/list_entry in 09-regions_05-ptra_rc, `A->next` written
      // under B_mutex and read under A_mutex). Demoting `is_top()` would
      // downgrade those real races to "unknown" (a soundness regression).
      // Until a PROVENANCE-aware criterion exists (⊤-heap vs ⊤-global vs
      // container_of base), no RAW is_top() pair is demoted.
      //
      // Model-boundary demotion (phase 3, mixed atomic/non-atomic) IS ENABLED.
      // C11 DRF only exempts ATOMIC-vs-ATOMIC pairs (the `continue` above). An
      // atomic access paired with a NON-atomic access to the same cell is a
      // model boundary: the atomic's synchronization semantics (acquire/
      // release memory order) is not modelled (theory §2.1.9), so the checker
      // cannot run a reliable may-race analysis on the pair. Report it as
      // "unknown" (Result::Warning), NEVER as a proven race and NEVER safe.
      //
      // Model-boundary demotion (phase 2 LIGHT PROTOTYPE) is DISABLED after
      // evaluation: demoting UnknownObject (global pointer value lost) turned
      // out UNSOUND — the same "global pointer + variable index" ⊤ underlies
      // BOTH a weaver FP (popl20-more-sum-array-hom, `A[i]` vs `sum2`) AND a
      // REAL race (popl20-more-parray-copy / per-thread-array-index-race,
      // where the pointer's heap identity is lost but three threads still race
      // on the same object). "Object identity lost" is a PRECISION issue, not
      // an unimplemented concurrency semantic, so it must stay RACE (may-race),
      // never UNKNOWN. The provenance classification (classify_top_provenance)
      // and the AccessRecord.provenance field are KEPT for a future sound
      // criterion, but the demotion condition below only uses phase 3.
      pr.is_unknown = (a.is_atomic != b.is_atomic);
      {
        PointsToSet common = a.pts.meet(b.pts);
        if (common.is_top()) {
          // TOP intersection (both sides are ⊤): "any cell" — no concrete
          // cells to dump; emit a marker instead of iterating a TopKind set.
          pr.first_conflict_loc = "\xe2\x8a\xa4"; // "⊤"
          pr.conflicting_pts.add("\xe2\x8a\xa4");
        } else {
          for (MemoryLocation* loc : common) {
            std::ostringstream oss;
            loc->dump(oss);
            if (pr.first_conflict_loc.empty()) {
              pr.first_conflict_loc = oss.str();
            }
            pr.conflicting_pts.add(oss.str());
          }
        }
      }
      all_pairs.push_back(std::move(pr));
    }
  }

  if (all_pairs.empty()) {
    return;
  }

  // Helper: encode a single side of a race pair as a JSON dict.
  // Captured by reference into the lambda; must outlive the lambda's
  // uses during group emission below.
  auto side_info = [](const AccessRecord& rec,
                      const JsonList& pts,
                      const std::unordered_set< std::string >& joined) {
    JsonDict side;
    side.put("thread_id", rec.thread_id);
    side.put("kind", static_cast< int >(rec.kind));
    JsonList lock_list;
    for (std::uint64_t l : rec.locks) {
      lock_list.add("0x" + std::to_string(l));
    }
    side.put("locks", lock_list);
    JsonList joined_list;
    for (const std::string& t : joined) {
      joined_list.add(t);
    }
    side.put("joined_threads", joined_list);
    side.put("unique_thread", rec.is_unique_thread);
    // Source location of THIS access (for the SV-COMP violation witness:
    // the two target waypoints must carry physical file:line:column of the
    // two conflicting accesses). Omitted when the statement has no frontend.
    if (auto loc = source_location(rec.stmt)) {
      side.put("line", static_cast< int >(loc.line()));
      side.put("column", static_cast< int >(loc.column()));
      side.put("file", loc.path().filename().string());
    }
    side.put("points_to", pts);
    return side;
  };

  // Group pairs by memory_location (the physical site of the race).
  // The location name is the lexicographic key — the dump() format is
  // deterministic across runs of the same IR (memory locations are
  // addressed by their llvm.global / local slot). Using the first cell
  // of `pts` as the key keeps the grouping stable even when a single
  // pair references multiple addresses.
  std::map< std::string, std::vector< std::size_t > > groups;
  for (std::size_t k = 0; k < all_pairs.size(); ++k) {
    const std::string& key =
        all_pairs[k].first_conflict_loc.empty() ? std::string("unknown")
                                                 : all_pairs[k]
                                                       .first_conflict_loc;
    groups[key].push_back(k);
  }

  // pthread_create call sites, for the violation witness's thread
  // registration (function_enter waypoints). Emitted once and reused in
  // every group's info so downstream parsers can read it from any report.
  JsonList thread_creations;
  for (const ThreadCreation& tc : this->_thread_creations) {
    JsonDict d;
    if (auto loc = source_location(tc.stmt)) {
      d.put("line", static_cast< int >(loc.line()));
      d.put("column", static_cast< int >(loc.column()));
      d.put("file", loc.path().filename().string());
    }
    d.put("thread_function", tc.child);
    ar::Code* code = tc.stmt->parent()->code();
    if (code != nullptr && code->is_function_body()) {
      d.put("creator", code->function()->name());
    }
    thread_creations.add(d);
  }

  // Emit one aggregated report per group.
  for (auto& kv : groups) {
    const std::vector< std::size_t >& pair_indices = kv.second;
    if (pair_indices.empty()) {
      continue;
    }

    // Anchor on the FIRST pair (Write of A or B).
    const PendingRacePair& first = all_pairs[pair_indices.front()];
    const AccessRecord& a0 = this->_accesses[first.i];
    const AccessRecord& b0 = this->_accesses[first.j];

    // A group is "unknown" only when EVERY pair in it is a model boundary
    // (⊤ points-to). If any pair is a proven (concrete) race, the group stays
    // a race — a proven race is never downgraded to unknown.
    bool group_is_unknown = true;
    for (std::size_t pk : pair_indices) {
      if (!all_pairs[pk].is_unknown) {
        group_is_unknown = false;
        break;
      }
    }

    // Build the nested list of all conflicting_accesses.
    JsonList conflicting_accesses;
    std::size_t pair_id = 0;
    for (std::size_t pk : pair_indices) {
      const PendingRacePair& pr = all_pairs[pk];
      const AccessRecord& ai = this->_accesses[pr.i];
      const AccessRecord& bj = this->_accesses[pr.j];

      JsonDict pair_dict;
      pair_dict.put("pair_id", static_cast< int >(pair_id));
      pair_dict.put("access_a", side_info(ai, pr.conflicting_pts,
                                          joined_names[pr.i]));
      pair_dict.put("access_b", side_info(bj, pr.conflicting_pts,
                                          joined_names[pr.j]));
      conflicting_accesses.add(pair_dict);
      ++pair_id;
    }

    // The aggregated info dict. Keeps the legacy flat top-level
    // `access_a`/`access_b` pointing at the first pair so any
    // downstream parser that reads `info.access_a` keeps working; the
    // new `conflicting_accesses` carries the full list for consumers
    // that want the complete picture.
    JsonDict info{
        {"memory_location",
         first.first_conflict_loc.empty() ? std::string("unknown")
                                          : first.first_conflict_loc},
        {"access_a", side_info(a0, first.conflicting_pts,
                               joined_names[first.i])},
        {"access_b", side_info(b0, first.conflicting_pts,
                               joined_names[first.j])},
        {"conflicting_accesses", conflicting_accesses},
        {"num_pairs", static_cast< int >(pair_indices.size())},
        {"unique_lockset_intersection", "empty"},
    };
    info.put("thread_creations", thread_creations);

    if (group_is_unknown) {
      info.put("verdict", std::string("unknown"));
      info.put("unknown_reason",
               std::string("model boundary (atomic mixed or unknown-object ⊤ provenance)"));
      info.put("ref_doc",
               std::string("ikos-race-theory-mapping.md §2.1.2/§2.1.9 / §3.2 (缺口 #3 与 provenance 原型)"));
    }

    this->_checks.insert(CheckKind::DataRace,
                         CheckerName::DataRace,
                         group_is_unknown ? Result::Warning : Result::Error,
                         first.report_stmt,
                         first.report_cc,
                         /* operands = */ {},
                         /* info = */ info);
    // No terminal output: the aggregated info is the report. Downstream
    // tools (ikos-report, ikos-view) render it from the database.
  }
}

CheckerName DataRaceChecker::name() const {
  return CheckerName::DataRace;
}

const char* DataRaceChecker::description() const {
  return "Data race checker";
}

void DataRaceChecker::check(ar::Statement* stmt,
                            const value::AbstractDomain& inv,
                            CallContext* call_context) {
  if (inv.first().is_normal_flow_bottom()) {
    // Statement is unreachable
    return;
  }

  if (auto load = dyn_cast< ar::Load >(stmt)) {
    this->check_load(load, inv, call_context);
  } else if (auto store = dyn_cast< ar::Store >(stmt)) {
    this->check_store(store, inv, call_context);
  } else if (auto call = dyn_cast< ar::CallBase >(stmt)) {
    this->check_extern_call_effects(call, inv, call_context);
  }
}

DataRaceChecker::PointsToSet DataRaceChecker::resolve_points_to(
    ar::Value* pointer, const value::AbstractDomain& inv) const {
  const ScalarLit& ptr = this->_lit_factory.get_scalar(pointer);

  if (ptr.is_undefined() ||
      (ptr.is_pointer_var() &&
       inv.first().normal().uninit_is_uninitialized(ptr.var()))) {
    return PointsToSet::bottom();
  }

  if (!ptr.is_pointer_var()) {
    return PointsToSet::bottom();
  }

  // Initialize global / function-pointer bases for accurate points-to
  if (auto gv = dyn_cast< ar::GlobalVariable >(pointer)) {
    Variable* v = this->_ctx.var_factory->get_global(gv);
    MemoryLocation* addr = this->_ctx.mem_factory->get_global(gv);
    value::AbstractDomain tmp = inv;
    tmp.first().normal().pointer_assign(v, addr, core::Nullity::non_null());
    return tmp.first().normal().pointer_to_points_to(ptr.var());
  }
  if (auto fpc = dyn_cast< ar::FunctionPointerConstant >(pointer)) {
    Variable* v = this->_ctx.var_factory->get_function_ptr(fpc->function());
    MemoryLocation* addr =
        this->_ctx.mem_factory->get_function(fpc->function());
    value::AbstractDomain tmp = inv;
    tmp.first().normal().pointer_assign(v, addr, core::Nullity::non_null());
    return tmp.first().normal().pointer_to_points_to(ptr.var());
  }
  // A direct local alloca (`$x = allocate`) always points to its own stack
  // cell — a ⊤ points-to here is a leftover from pthread_create's handle init,
  // which calls scalar_assign_nondet on the POINTER (it meant to mark the
  // pointee initialized). Pinning the alloca back to its cell is a SOUND
  // refinement: a stack slot never aliases a global/heap cell, and any
  // cross-thread visibility is tracked separately via `_escaped_locs`. Without
  // this, a pthread_join(t) handle read carries ⊤ and pairs with every shared
  // write (weaver FP flood). Assignments (`p = &global`) are InternalVariables,
  // not LocalVariables, so they are untouched.
  if (auto lv = dyn_cast< ar::LocalVariable >(pointer)) {
    Variable* v = this->_ctx.var_factory->get_local(lv);
    MemoryLocation* addr = this->_ctx.mem_factory->get_local(lv);
    value::AbstractDomain tmp = inv;
    tmp.first().normal().pointer_assign(v, addr, core::Nullity::non_null());
    return tmp.first().normal().pointer_to_points_to(ptr.var());
  }
  // A pointer DERIVED from a local alloca via PointerShift (`&arr[i]`, the
  // pthread_t handle GEP in `pthread_join(&t_ids[i])`) provably points INTO
  // the thread-private stack: its base is a LocalVariable. With a VARIABLE
  // loop index `i` over a 10000-element array the GEP's points-to collapses
  // to ⊤, so `touches_shared_memory` conservatively records it as shared and
  // the destructor's meet (⊤ is the identity) pairs it against EVERY shared
  // write — the 28-race_reach_* FP flood (main's handle read "races" t_fun's
  // @global write). Pin the GEP result to the local's cell: sound (a stack
  // slot never aliases global/heap; cross-thread visibility is tracked via
  // `_escaped_locs`), and it makes the meet with a global cell empty.
  if (auto iv = dyn_cast< ar::InternalVariable >(pointer)) {
    ar::Value* base = pointershift_base(pointer);
    if (base != nullptr) {
      if (auto lv = dyn_cast< ar::LocalVariable >(base)) {
        // Directly name the local's cell — no pointer_assign round-trip, whose
        // type check (`%arr[i]` is `elem*`, the local is the whole array)
        // asserts. The base is a stack alloca, so the access is thread-private
        // regardless of the (variable) index.
        return PointsToSet{this->_ctx.mem_factory->get_local(lv)};
      }
    }
  }

  return inv.first().normal().pointer_to_points_to(ptr.var());
}

/// \brief Constant STRUCTURAL byte offset of a pointer's PointerShift chain
/// (the field offset within the base struct/array) and the base pointee type.
///
/// Used as a FALLBACK when the abstract offset is ⊤ (opaque base): the
/// abstract domain loses the field offset because the base's points-to is ⊤
/// (⊤ + 0 = ⊤), but two DIFFERENT fields of the SAME struct are provably
/// disjoint regardless of which object the opaque base points to
/// (04-mutex_78-type-array.c: `getS()->field` vs `getS()->arr[1]`).
///
/// Returns false (leaving `type` null) when any PointerShift term has a
/// VARIABLE operand (a variable array index / pointer arithmetic) — the offset
/// is then not a compile-time constant, and no disjointness may be claimed.
static bool structural_field_of(ar::Value* ptr, std::int64_t& out,
                                ar::Type*& type) {
  std::int64_t acc = 0;
  for (int depth = 0; depth < 8; ++depth) {
    auto* iv = dyn_cast< ar::InternalVariable >(ptr);
    if (iv == nullptr) {
      break; // GlobalVariable / constant leaf: base reached
    }
    ar::Statement* def = unique_def(iv);
    auto* ps = dyn_cast_or_null< ar::PointerShift >(def);
    if (ps == nullptr) {
      break; // call / load / phi: base reached
    }
    for (std::size_t i = 0; i < ps->num_terms(); ++i) {
      auto term = ps->term(i);
      auto* c = dyn_cast< ar::IntegerConstant >(term.second);
      if (c == nullptr) {
        return false; // variable index → not a constant field offset
      }
      acc += term.first.to_z_number().to< std::int64_t >() *
             c->value().to_z_number().to< std::int64_t >();
    }
    ptr = ps->pointer(); // peel to the base pointer
  }
  type = (ptr != nullptr && isa< ar::PointerType >(ptr->type()))
             ? cast< ar::PointerType >(ptr->type())->pointee()
             : nullptr;
  if (type == nullptr) {
    return false;
  }
  out = acc;
  return true;
}

core::machine_int::Interval DataRaceChecker::resolve_offset(
    ar::Value* pointer, const value::AbstractDomain& inv) const {
  const ScalarLit& ptr = this->_lit_factory.get_scalar(pointer);
  if (ptr.is_pointer_var() && !ptr.is_undefined() &&
      !inv.first().normal().uninit_is_uninitialized(ptr.var())) {
    auto off = inv.first().normal().pointer_to_pointer(ptr.var()).offset();
    return off;
  }
  return core::machine_int::Interval::top(64, core::Signed);
}

std::uint64_t DataRaceChecker::instance_base_offset(
    ar::Value* pointer, const value::AbstractDomain& inv) const {
  auto flat_of = [&](ar::Value* v) -> std::uint64_t {
    const ScalarLit& lit = this->_lit_factory.get_scalar(v);
    if (lit.is_pointer_var() && !lit.is_undefined() &&
        !inv.first().normal().uninit_is_uninitialized(lit.var())) {
      auto off = inv.first().normal().pointer_to_pointer(lit.var()).offset();
      if (!off.is_top() && !off.is_bottom() && off.lb() == off.ub()) {
        return off.lb().to_z_number().to< std::uint64_t >();
      }
    }
    return 0;
  };
  ar::Value* base = pointershift_base(pointer);
  if (base != nullptr) {
    // ARRAY element (the PointerShift base is the array, pointee is an
    // ArrayType). ikos-pp flattens `m[i].field` into one PointerShift, so the
    // flat offset is the FIELD offset — round DOWN to the element boundary so
    // a field shares its element base with its siblings (06-symbeq_05-
    // funloop_hard2.c: refs_mutex@248 → 240 == refs@240). STRUCT field (the
    // base is a struct) — the base's offset is the struct instance base.
    auto* bt = dyn_cast< ar::PointerType >(base->type());
    if (bt != nullptr && isa< ar::ArrayType >(bt->pointee())) {
      std::uint64_t off = flat_of(pointer);
      auto* at = cast< ar::ArrayType >(bt->pointee());
      std::uint64_t elem_size =
          this->_ctx.bundle->data_layout()
              .alloc_size_in_bytes(at->element_type())
              .to< std::uint64_t >();
      return (elem_size != 0) ? off - (off % elem_size) : off;
    }
    return flat_of(base); // struct field
  }
  return flat_of(pointer); // whole object
}

std::vector< std::uint64_t > DataRaceChecker::snapshot_locks(
    const value::AbstractDomain& inv, std::uint64_t access_guard) const {
  // `lockset_held_locks()` already returns {} for ⊤/⊥, so no early-return
  // is needed — and it MUST NOT early-return on `lockset_is_top()`, because
  // that predicate only inspects the MUTEX digest and would skip the
  // conditional-lock resolution below.
  std::unordered_set< std::uint64_t > held =
      inv.second().held_locks();
  // Resolve conditional locks. HELD_IF_ZERO (trylock / `if (!i) lock`): the
  // key is the guard Variable* index, held iff its interval is the singleton
  // {0} — trylock is the 04-mutex_35-trylock_rc.c FN fix. HELD_IF_NONZERO
  // (`if (i) lock`): the key is the canonical (affine-peeled) base AR var
  // pointer, held iff it SSA-identity-matches the access's own branch guard
  // (the access executes iff its guard != 0) — 04-mutex_07-ps_nr /
  // 04-mutex_17-ps_add1_nr.c FP fix.
  for (const auto& kv : inv.second().cond_locks()) {
    if (kv.second.polarity == core::lockset::CondPolarity::HELD_IF_ZERO) {
      Variable* guard = reinterpret_cast< Variable* >(kv.first);
      auto single = inv.first().normal().int_to_interval(guard).singleton();
      if (single && single->is_zero()) {
        held.insert(kv.second.lock_addr);
      }
    } else if (access_guard != 0 && kv.first == access_guard) {
      held.insert(kv.second.lock_addr);
    }
  }
  return {held.begin(), held.end()};
}

std::vector< std::uint64_t > DataRaceChecker::snapshot_read_locks(
    const value::AbstractDomain& inv) const {
  std::unordered_set< std::uint64_t > held =
      inv.second().held_read_locks();
  return {held.begin(), held.end()};
}

std::unordered_set< std::uint64_t > DataRaceChecker::snapshot_joined_threads(
    const value::AbstractDomain& inv) const {
  return inv.second().get_joined_threads();
}

std::unordered_set< std::string > DataRaceChecker::snapshot_spawned_threads(
    const value::AbstractDomain& inv) const {
  // MAY-spawned digest (separate from the MUST joined digest since the
  // create-edge HB needs "possibly spawned", which a MUST set — emptied at a
  // conditional-create merge — cannot express; race-1_3b-join.c FN).
  return inv.second().get_spawned_threads();
}

std::unordered_set< std::uint64_t > DataRaceChecker::snapshot_spawned_instances(
    const value::AbstractDomain& inv) const {
  return inv.second().get_spawned_instances();
}

bool DataRaceChecker::snapshot_spawned_is_top(
    const value::AbstractDomain& inv) const {
  return inv.second().spawned_is_top();
}

void DataRaceChecker::build_thread_creators() {
  // Static scan of pthread_create call sites: child entry name -> set of
  // creator entry names. Mirrors the engine-side detection
  // (NumericalExecutionEngine's pthread_create handler): direct call via
  // FunctionPointerConstant named "pthread_create", thread function given
  // as a constant in argument slot 2. Creators are identified by entry
  // name — the same identity `current_thread_id` assigns to access
  // records. Creation inside a non-entry helper is skipped (creator name
  // would not match any record's tid): precision loss, never unsoundness.
  for (auto fit = this->_ctx.bundle->function_begin(),
            fend = this->_ctx.bundle->function_end();
       fit != fend;
       ++fit) {
    ar::Function* fun = *fit;
    if (!fun->is_definition()) {
      continue;
    }

    std::string creator;
    if (this->_ctx.concurrent_env.is_thread_entry(fun)) {
      creator = fun->name();
    } else if (fun->name() == "main") {
      creator = "main";
    } else {
      continue;
    }

    ar::Code* body = fun->body_or_null();
    if (body == nullptr) {
      continue;
    }
    for (auto bit = body->begin(), bend = body->end(); bit != bend; ++bit) {
      ar::BasicBlock* bb = *bit;
      for (auto sit = bb->begin(), send = bb->end(); sit != send; ++sit) {
        ar::Statement* stmt = *sit;
        ar::CallBase* call = dyn_cast< ar::CallBase >(stmt);
        if (call == nullptr) {
          continue;
        }
        ar::FunctionPointerConstant* cst =
            dyn_cast< ar::FunctionPointerConstant >(call->called());
        if (cst == nullptr || cst->function() == nullptr ||
            cst->function()->name() != "pthread_create") {
          continue;
        }
        if (call->arg_begin() + 2 == call->arg_end()) {
          continue;
        }
        ar::FunctionPointerConstant* tfc =
            dyn_cast< ar::FunctionPointerConstant >(*(call->arg_begin() + 2));
        if (tfc == nullptr || tfc->function() == nullptr) {
          continue;
        }
        this->_thread_creators[tfc->function()->name()].insert(creator);
        this->_thread_creations.push_back(
            ThreadCreation{stmt, tfc->function()->name()});
      }
    }
  }

  // Deterministic source order: the k-th creation (by line/column) assigns
  // witness thread_id k, matching the SV-COMP thread-registration semantics.
  std::sort(this->_thread_creations.begin(), this->_thread_creations.end(),
            [](const ThreadCreation& a, const ThreadCreation& b) {
              SourceLocation la = source_location(a.stmt);
              SourceLocation lb = source_location(b.stmt);
              if (la && lb) {
                if (la.line() != lb.line()) {
                  return la.line() < lb.line();
                }
                if (la.column() != lb.column()) {
                  return la.column() < lb.column();
                }
              }
              return a.child < b.child;
            });

  // Transitive closure: if A creates B and B creates C, then A transitively
  // creates C — A's pre-create writes happen-before C's writes
  // (10-synch_15-join_other_nr.c: main → t1_fun → t2_fun). Kept in a SEPARATE
  // map so the HB test below still reads the DIRECT set (`_thread_creators`)
  // for its "unique direct creator" requirement.
  this->_thread_ancestors = this->_thread_creators;
  bool changed = true;
  while (changed) {
    changed = false;
    const auto snapshot = this->_thread_ancestors; // avoid rehash invalidation
    for (const auto& kv : snapshot) {
      for (const std::string& anc : kv.second) {
        auto it = snapshot.find(anc);
        if (it == snapshot.end()) {
          continue;
        }
        for (const std::string& grand : it->second) {
          if (this->_thread_ancestors[kv.first].insert(grand).second) {
            changed = true;
          }
        }
      }
    }
  }
}

std::string DataRaceChecker::current_thread_id(ar::Statement* stmt,
                                               CallContext* call_context) {
  // Deterministic, flow-sensitive thread identity: walk the call-context
  // chain UP to its root - the caller function of the OUTERMOST call
  // statement is the thread entry (main / t_fun), not the syntactic
  // function containing the statement. Probing showed the old
  // syntactic-function rule collapsed accesses flowing through the same
  // helper called from different threads (munge called from both main
  // and t_fun both got tid="munge"), falsely short-circuiting their
  // race pairs as "same thread".
  const CallContext* cc = call_context;
  const CallContext* outermost = nullptr;
  while (cc != nullptr && cc->has_parent()) {
    outermost = cc; // deepest non-entry level seen so far
    cc = cc->parent();
  }
  // The outermost call statement's containing function IS the thread
  // entry (its own context is the empty/entry context).
  if (outermost != nullptr) {
    if (auto* fn = outermost->call()->code()->function_or_null()) {
      return fn->name();
    }
  }
  // No call chain (statements in the entry function body itself, e.g.
  // main): fall back to the syntactic function.
  if (stmt != nullptr) {
    if (auto* code = stmt->code()) {
      if (auto* fn = code->function_or_null()) {
        return fn->name();
      }
    }
  }
  return {};
}

bool DataRaceChecker::touches_shared_memory(const PointsToSet& pts) const {
  // Race detection is only meaningful for cells that could be visible across
  // threads. Stack-local addresses are thread-private by construction, so
  // ignore them — this also keeps `_accesses` from blowing up on large
  // programs that touch many locals per statement.
  // ⭐ Top/Bottom guard: a top points-to set (unknown target, e.g. a `void*`
  // thread arg, container_of arithmetic, escaped pointer) may denote any
  // cell — conservatively shared. A bottom set denotes nothing. Without
  // this guard, `for (addr : pts)` calls begin() on a TopKind/BottomKind
  // set and trips `assert(_kind == SetKind)` (points_to_set.hpp:128).
  if (pts.is_bottom()) {
    return false;
  }
  if (pts.is_top()) {
    return true;
  }
  for (MemoryLocation* addr : pts) {
    if (auto* gml = dyn_cast< GlobalMemoryLocation >(addr)) {
      // Thread-local storage (`__thread` / `_Thread_local`): each thread has
      // its own copy, so the cell can never race across threads — UNLESS its
      // address has escaped to another thread (passed as a pthread_create
      // arg, 04-mutex_83-thread-local-storage-escape.c), in which case the
      // dereferenced copy is shared and must race. The escape is tracked in
      // the same `_escaped_locs` set used for stack locals below.
      if (gml->global_var()->is_thread_local() &&
          !this->_ctx.concurrent_env.is_escaped(gml)) {
        continue;
      }
      return true;
    }
    if (isa< DynAllocMemoryLocation >(addr)) {
      return true;
    }
    // A stack local is normally thread-private, but passing `&i` as a
    // pthread_create arg ESCAPES it — the child dereferences the same cell
    // (04-mutex_45-escape_rc.c). Treat escaped locals as shared.
    if (isa< LocalMemoryLocation >(addr) &&
        this->_ctx.concurrent_env.is_escaped(addr)) {
      return true;
    }
  }
  return false;
}

bool DataRaceChecker::region_tainting_sound() {
  if (this->_region_sound_computed) {
    return this->_region_sound;
  }
  this->_region_sound_computed = true;
  // Fallback iff some store writes a POINTER that was itself LOADED from a
  // slot — a cross-slot pointer alias that would make "load from slot[k] ⟹
  // region k" an unsound under-approximation. `slot[j] = new(...)` (fresh
  // allocation) has a call-result RHS, not a load, so it does not trip this.
  //
  // Two store TARGET forms are cross-slot hazards:
  //   (a) a GLOBAL slot:  `slot[k] = slot[j]`            (element-to-element)
  //   (b) a HEAP FIELD:   `p1->next = p2->next`          (list splice)
  //       — 09-regions_23-evilcollapse_rc.c pulls nodes out of two slots via
  //       `lookup(1)`/`lookup(2)` and splices one list onto the other. The
  //       loaded pointer leaves its home slot through a heap `->next` field,
  //       so the per-slot summarization is no longer sound. This is form (b),
  //       which the old scan MISSED (it only looked at global stores), giving
  //       a false "sound" verdict on 23.
  for (auto fit = this->_ctx.bundle->function_begin(),
            fend = this->_ctx.bundle->function_end();
       fit != fend;
       ++fit) {
    ar::Function* fun = *fit;
    if (!fun->is_definition()) {
      continue;
    }
    ar::Code* body = fun->body_or_null();
    if (body == nullptr) {
      continue;
    }
    for (ar::BasicBlock* bb : *body) {
      for (ar::Statement* stmt : *bb) {
        auto* s = dyn_cast< ar::Store >(stmt);
        if (s == nullptr) {
          continue;
        }
        // Only a store of a POINTER value can create a cross-slot alias.
        auto* pt = dyn_cast< ar::PointerType >(s->pointer()->type());
        if (pt == nullptr || !isa< ar::PointerType >(pt->pointee())) {
          continue; // not storing a pointer
        }
        auto* rv = dyn_cast< ar::InternalVariable >(s->value());
        if (rv == nullptr) {
          continue; // constant / global RHS: not an alias
        }
        ar::Statement* rvd = unique_def(rv);
        if (rvd == nullptr || !isa< ar::Load >(rvd)) {
          continue; // fresh allocation (call result), phi, or copy: no alias
        }

        // Form (a): a global slot store carrying a loaded pointer.
        if (resolve_global_base(s->pointer(), this->_ctx) != 0) {
          this->_region_sound = false; // cross-slot pointer alias
          return false;
        }

        // Form (b): a heap-field store carrying a loaded pointer. The store
        // target is a PointerShift result (a `.field` / `[i]` access) whose
        // base is NOT rooted in a global — a global base would have been
        // caught by form (a) above. This is the 23-evilcollapse list splice:
        // a pointer loaded out of one slot's list is written into a node field
        // reached from another slot, merging the two slots.
        //
        // Require the field's BASE to be a SLOT-DERIVED pointer (a load, or a
        // call to a non-allocator that loads — `lookup()` returns `slot[h].next`).
        // Otherwise the store merely inserts a loaded node into a list reached
        // through a FRESH node (`list_add`'s `node->next = temp`, where `node` is
        // a call/malloc result not yet in any slot) — that keeps everything in
        // one slot and is NOT a cross-slot splice. 11/13/17/19 (arraylist
        // etc.) must stay region-sound so their `slot[i]` accesses get a
        // region; only a loaded-to-loaded splice like `p1->next = p2->next`
        // trips this.
        auto* piv = dyn_cast< ar::InternalVariable >(s->pointer());
        if (piv != nullptr) {
          ar::Statement* pvd = unique_def(piv);
          auto* ps = dyn_cast_or_null< ar::PointerShift >(pvd);
          if (ps != nullptr) {
            auto* base_iv = dyn_cast< ar::InternalVariable >(ps->pointer());
            if (base_iv != nullptr) {
              ar::Statement* bvd = unique_def(base_iv);
              if (bvd != nullptr && isa< ar::Load >(bvd)) {
                this->_region_sound = false; // heap-field splice
                return false;
              }
              if (bvd != nullptr && isa< ar::CallBase >(bvd) &&
                  !is_fresh_alloc(base_iv)) {
                // `p1->next = p2->next` where `p1 = lookup(1)` (a call that
                // returns a slot-loaded pointer, not a fresh malloc): the node
                // leaves its home slot through a heap field, so the per-slot
                // summarization is unsound (09-regions_23-evilcollapse_rc.c).
                this->_region_sound = false; // heap-field splice via call
                return false;
              }
            }
          }
        }
      }
    }
  }
  this->_region_sound = true;
  return true;
}

void DataRaceChecker::check_extern_call_effects(
    ar::CallBase* call,
    const value::AbstractDomain& inv,
    CallContext* call_context) {
  // Determine whether the callee has no analyzable body (extern).
  ar::Value* called = call->called();
  bool extern_call = false;
  if (auto* cst = dyn_cast< ar::FunctionPointerConstant >(called)) {
    extern_call = cst->function()->is_declaration();
  } else if (auto* ptr = dyn_cast< ar::InternalVariable >(called)) {
    Variable* pv = this->_ctx.var_factory->get_internal(ptr);
    PointsToSet cpts = inv.first().normal().pointer_to_points_to(pv);
    if (cpts.is_top()) {
      extern_call = true; // unknown target: conservatively extern
    } else if (!cpts.is_bottom() && cpts.is_set() && cpts.size() > 0) {
      extern_call = true;
      for (MemoryLocation* loc : cpts) {
        auto* fm = dyn_cast< FunctionMemoryLocation >(loc);
        if (fm == nullptr || fm->function()->is_definition()) {
          extern_call = false; // at least one analyzable body exists
          break;
        }
      }
    }
  } else {
    return; // undefined / null / asm / data: no side-effect synthesis
  }
  if (!extern_call) {
    return;
  }

  // Exclude the pthread lock primitives: their pointer argument is the
  // MUTEX ITSELF, not shared data — synthesizing a Write on it (with an
  // empty lockset, since the record is taken before/after the abstract
  // lock manipulation) fabricates "unprotected write on the mutex"
  // pairs between threads (04-mutex_02-simple_nr.c FP).
  std::string callee_nm;
  if (auto* cst = dyn_cast< ar::FunctionPointerConstant >(called)) {
    callee_nm = cst->function()->name();
  } else if (auto* ptr = dyn_cast< ar::InternalVariable >(called)) {
    Variable* pv = this->_ctx.var_factory->get_internal(ptr);
    PointsToSet cpts = inv.first().normal().pointer_to_points_to(pv);
    if (!cpts.is_top() && !cpts.is_bottom() && cpts.is_set()) {
      for (MemoryLocation* loc : cpts) {
        if (auto* fm = dyn_cast< FunctionMemoryLocation >(loc)) {
          callee_nm = fm->function()->name();
          break;
        }
      }
    }
  }
  // Matches both "pthread_mutex_lock" and the AR intrinsic form
  // "ar.pthread.mutex.lock" written by ikos-pp.
  if (callee_nm.find("pthread.mutex") != std::string::npos ||
      callee_nm.find("pthread_mutex") != std::string::npos ||
      callee_nm.find("pthread_rwlock") != std::string::npos ||
      callee_nm.find("pthread.spin") != std::string::npos ||
      callee_nm.find("pthread_barrier") != std::string::npos ||
      // pthread_create is fully modelled by exec_pthread_create: its only
      // side effect is storing the thread id into the *pthread_t handle
      // (a thread-private stack cell, filtered by touches_shared_memory).
      // The trailing `void *arg` is PASSED to the child (read, not written),
      // so synthesizing a Write through it fabricates a "main writes the
      // shared object it hands to the thread" race — the child's own access
      // to that object races, main's pthread_create call does not
      // (06-symbeq_26/27/33-symb_lockfuns FP).
      callee_nm.find("pthread_create") != std::string::npos ||
      // pthread_once(&once, init): the once-control (arg0) is pthread runtime
      // state, not shared data — synthesizing a Write on it fabricates a
      // cross-thread race on the once-control itself (87-once/* FP). The
      // callback's own accesses are handled by exec_pthread_once.
      callee_nm.find("pthread_once") != std::string::npos ||
      // pthread_cond_wait/signal/broadcast/init/destroy: their pointer args
      // are the CONDITION-VARIABLE and MUTEX objects, not shared data.
      // They are already modeled by exec_pthread_cond_wait/signal; the
      // write-synthesis fallback here fabricates cross-thread races on the
      // cond/mutex object itself (pthread/sync01.c FP: `pthread_cond_wait`
      // and `pthread_cond_signal` "race" on `&empty`/`&full`/`&m`).
      callee_nm.find("pthread_cond") != std::string::npos ||
      // POSIX semaphores (sem_wait/sem_post/sem_init/...): their pointer
      // argument is the semaphore OBJECT, not shared data — synthesizing a
      // Write on it fabricates cross-thread races on the semaphore itself
      // (semaphore-posix.c). sem_getvalue's int* output is a thread-local
      // (filtered by touches_shared_memory), so excluding the whole family is
      // sound.
      callee_nm.rfind("sem_", 0) == 0) {
    return; // modeled sync primitive: args are lock/thread ids, not data
  }

  // Synthesize ONE Write access per pointer argument: the library may
  // read or write through it (scanf(&x), fgets(buf, ...), ...). The
  // record carries the same thread identity and lockset as an explicit
  // store at this program point would.
  std::string tid = this->current_thread_id(call, call_context);
  bool unique = this->_ctx.concurrent_env.is_thread_unique(tid);
  auto joined = this->snapshot_joined_threads(inv);
  auto spawned = this->snapshot_spawned_threads(inv);
  auto spawned_instances = this->snapshot_spawned_instances(inv);
  bool spawned_is_top = this->snapshot_spawned_is_top(inv);
  auto locks = this->snapshot_locks(inv);
  auto read_locks = this->snapshot_read_locks(inv);
  for (auto it = call->arg_begin(), et = call->arg_end(); it != et; ++it) {
    ar::Value* arg = *it;
    PointsToSet pts = this->resolve_points_to(arg, inv);
    // A bottom set has no target; a concrete-but-thread-private set is not
    // shared. A TOP set (unknown target — e.g. an opaque pointer returned by
    // an extern, or a struct assignment lowered to llvm.memcpy) is
    // conservatively SHARED: the library may write through it to any cell, so
    // we must record the access (04-mutex_90/91/92-distribute-fields FN was
    // caused by skipping it and silently dropping the write). A ⊤ set is
    // recorded as a SINGLE ⊤ Write below, never iterated (a TopKind set has
    // no begin()).
    if (pts.is_bottom() ||
        (pts.is_set() && !this->touches_shared_memory(pts))) {
      continue;
    }
    // Read-only memory exclusion (concrete sets only): string literals and
    // other compiler-generated constant globals (LLVM `.str`, `.str.N`,
    // `__PRETTY_FUNCTION__`, ...) are IMMUTABLE by construction — a
    // library call receiving them (printf format strings!) cannot
    // write through them. Synthesizing a Write on such nodes produced
    // cross-thread "races" on format strings (04-mutex_36-trylock_nr.c
    // FP: every printf pair raced on its own .str). A ⊤ set is never a
    // string literal, so it skips this check and is recorded.
    if (pts.is_set()) {
      bool all_const = true;
      bool any = false;
      for (MemoryLocation* ml : pts) {
        if (auto* gml = dyn_cast< GlobalMemoryLocation >(ml)) {
          any = true;
          const std::string& n = gml->global_var()->name();
          // Compiler-generated read-only constant? LLVM string literals are
          // `.str` / `.str.N`; clang names `__PRETTY_FUNCTION__` and `__func__`
          // string literals as `__PRETTY_FUNCTION__.<fn>` / `__func__.<fn>`
          // (observed as `__PRETTY_FUNCTION__.reach_error`). These are all
          // immutable — a library call cannot write through them.
          bool is_const =
              (!n.empty() && n[0] == '.') ||
              n.rfind("__PRETTY_FUNCTION__.", 0) == 0 ||
              n.rfind("__func__.", 0) == 0;
          if (!is_const) {
            all_const = false; // user global: writable
          }
        } else {
          any = true;
          all_const = false; // heap/stack: writable
        }
      }
      if (any && all_const) {
        continue; // constant globals only: no Write synthesis
      }
    }
    std::uint64_t base = 0;
    if (pts.is_set() && pts.size() == 1) {
      base = core::IndexableTraits< MemoryLocation* >::index(*pts.begin());
    }
    std::uint64_t instance = this->instance_base_offset(arg, inv);
    bool has_region = false;
    core::ConcurrentGlobalEnv::SymbolicIndex region;
    if (this->region_tainting_sound()) {
      has_region = region_of(arg, this->_ctx, call_context, region);
    }
    this->_accesses.push_back(
        AccessRecord{call,
                     call_context,
                     AccessKind::Write,
                     std::move(pts),
                     locks,
                     read_locks,
                     this->resolve_offset(arg, inv),
                     base,
                     instance,
                     has_region,
                     region,
                     tid,
                     unique,
                     joined,
                     spawned,
                     spawned_instances,
                     spawned_is_top});
  }

  // Non-thread-safe libc functions: they read-modify-write a process-wide
  // internal buffer WITHOUT any lock, so two concurrent calls race on it
  // (04-mutex_94-thread-unsafe_fun_rc.c). Synthesize a Write to the hidden
  // libc-state location so the checker pairs such calls — mirrors Goblint's
  // `ThreadUnsafe` library attribute. The raw C name covers the unmapped
  // extern form; "ar.libc.*" covers the intrinsic-mapped form.
  // NOTE: only rand/srand are white-listed. Extending this to the full
  // Goblint ThreadUnsafe set (strtok/strerror/localtime/...) introduced an FP
  // (04-mutex_36-trylock_nr.c's strerror-on-abort-path) without fixing any FN:
  // Goblint's ThreadUnsafe is gated on may-happen-in-parallel, whereas a naive
  // "always synthesize" is too coarse. Revisit only with an MHP-precise gate.
  if (callee_nm == "rand" || callee_nm == "srand" ||
      callee_nm == "ar.libc.rand" || callee_nm == "ar.libc.srand") {
    MemoryLocation* libc_state = this->_ctx.mem_factory->get_libc_state();
    PointsToSet state_pts{libc_state};
    this->_accesses.push_back(
        AccessRecord{call,
                     call_context,
                     AccessKind::Write,
                     std::move(state_pts),
                     locks,
                     read_locks,
                     core::machine_int::Interval::top(64, core::Signed),
                     core::IndexableTraits< MemoryLocation* >::index(libc_state),
                     /* instance = */ 0,
                     /* has_region = */ false,
                     core::ConcurrentGlobalEnv::SymbolicIndex{},
                     tid,
                     unique,
                     joined,
                     spawned,
                     spawned_instances,
                     spawned_is_top});
  }

  // mem* functions read their scalar (non-pointer) arguments too: memset's
  // value argument, memcpy/memmove's size argument, etc. When such an
  // argument is a direct global variable, the library reads that global —
  // synthesize a Read so it pairs with a concurrent write
  // (04-mutex_72-memset_arg_rc.c). White-listed to mem* to avoid the FP
  // explosion of treating every extern's scalar args as reads.
  if (callee_nm == "memset" || callee_nm == "ar.memset" ||
      callee_nm == "memcpy" || callee_nm == "ar.memcpy" ||
      callee_nm == "memmove" || callee_nm == "ar.memmove") {
    for (auto it = call->arg_begin(), et = call->arg_end(); it != et; ++it) {
      if (auto* gv = dyn_cast< ar::GlobalVariable >(*it)) {
        GlobalMemoryLocation* gml = this->_ctx.mem_factory->get_global(gv);
        PointsToSet read_pts{gml};
        this->_accesses.push_back(
            AccessRecord{call,
                         call_context,
                         AccessKind::Read,
                         std::move(read_pts),
                         locks,
                         read_locks,
                         core::machine_int::Interval::top(64, core::Signed),
                         core::IndexableTraits< MemoryLocation* >::index(gml),
                         /* instance = */ 0,
                         /* has_region = */ false,
                         core::ConcurrentGlobalEnv::SymbolicIndex{},
                         tid,
                         unique,
                         joined,
                         spawned,
                         spawned_instances,
                         spawned_is_top});
      }
    }
  }
}

void DataRaceChecker::check_load(ar::Load* load,
                                 const value::AbstractDomain& inv,
                                 CallContext* call_context) {
  PointsToSet pts = this->resolve_points_to(load->operand(), inv);
  if (!this->touches_shared_memory(pts)) {
    return;
  }
  std::string tid = this->current_thread_id(load, call_context);
  // Precompute the Thread Uniqueness flag so the destructor never re-enters
  // the global blackboard for every race pair.
  bool unique = this->_ctx.concurrent_env.is_thread_unique(tid);
  std::uint64_t base = 0;
  if (pts.is_set() && pts.size() == 1) {
    base = core::IndexableTraits< MemoryLocation* >::index(*pts.begin());
  }
  std::uint64_t instance = this->instance_base_offset(load->operand(), inv);
  bool has_region = false;
  core::ConcurrentGlobalEnv::SymbolicIndex region;
  if (this->region_tainting_sound()) {
    has_region = region_of(load->operand(), this->_ctx, call_context, region);
  }
  TopProvenance provenance = TopProvenance::Concrete;
  if (pts.is_top()) {
    provenance = classify_top_provenance(load->operand(), this->_ctx);
  }
  core::machine_int::Interval off = this->resolve_offset(load->operand(), inv);
  std::int64_t soff = 0;
  ar::Type* stype = nullptr;
  if (off.is_top()) {
    structural_field_of(load->operand(), soff, stype);
  }
  this->_accesses.push_back(
      AccessRecord{load,
                   call_context,
                   AccessKind::Read,
                   std::move(pts),
                   this->snapshot_locks(inv, branch_guard_of(load)),
                   this->snapshot_read_locks(inv),
                   off,
                   base,
                   instance,
                   has_region,
                   region,
                   std::move(tid),
                   unique,
                   this->snapshot_joined_threads(inv),
                   this->snapshot_spawned_threads(inv),
                   this->snapshot_spawned_instances(inv),
                   this->snapshot_spawned_is_top(inv),
                   load->is_atomic(),
                   provenance,
                   /*private_init=*/false,
                   /*promote_locks=*/{},
                   soff,
                   stype});
}

void DataRaceChecker::check_store(ar::Store* store,
                                  const value::AbstractDomain& inv,
                                  CallContext* call_context) {
  PointsToSet pts = this->resolve_points_to(store->pointer(), inv);
  if (!this->touches_shared_memory(pts)) {
    return;
  }
  std::string tid = this->current_thread_id(store, call_context);
  // Precompute the Thread Uniqueness flag so the destructor never re-enters
  // the global blackboard for every race pair.
  bool unique = this->_ctx.concurrent_env.is_thread_unique(tid);
  std::uint64_t base = 0;
  if (pts.is_set() && pts.size() == 1) {
    base = core::IndexableTraits< MemoryLocation* >::index(*pts.begin());
  }
  std::uint64_t instance = this->instance_base_offset(store->pointer(), inv);
  bool has_region = false;
  core::ConcurrentGlobalEnv::SymbolicIndex region;
  if (this->region_tainting_sound()) {
    has_region = region_of(store->pointer(), this->_ctx, call_context, region);
  }
  // Forward region label: when a heap node (DynAlloc) is STORED into a
  // flat-array slot, record `node -> slot region` so the destructor can fill
  // the region of the node's WRITE accesses (`new`'s `p->datum = x`), which
  // the def-chain region_collect cannot recover (11/13/17/19). The store
  // lives in the caller after the allocating function returns the node.
  if (this->region_tainting_sound()) {
    PointsToSet vpts = this->resolve_points_to(store->value(), inv);
    if (vpts.is_set() && vpts.size() == 1) {
      auto* ml = *vpts.begin();
      if (isa< DynAllocMemoryLocation >(ml)) {
        core::ConcurrentGlobalEnv::SymbolicIndex sr;
        if (region_of(store->pointer(), this->_ctx, call_context, sr)) {
          std::uint64_t idx =
              core::IndexableTraits< MemoryLocation* >::index(ml);
          auto it = this->_dyn_alloc_region.find(idx);
          if (it == this->_dyn_alloc_region.end() &&
              !this->_dyn_alloc_ambiguous.count(idx)) {
            this->_dyn_alloc_region[idx] = sr;
          } else if (it != this->_dyn_alloc_region.end() &&
                     it->second != sr) {
            // Stored into TWO different slots: region not unique. Drop the
            // label — a single region would match the wrong slot's lock
            // (12-arraycollapse_rc must stay a race).
            this->_dyn_alloc_region.erase(it);
            this->_dyn_alloc_ambiguous.insert(idx);
          }
        }
      }
    }
  }
  TopProvenance provenance = TopProvenance::Concrete;
  if (pts.is_top()) {
    provenance = classify_top_provenance(store->pointer(), this->_ctx);
  }
  core::machine_int::Interval off = this->resolve_offset(store->pointer(), inv);
  std::int64_t soff = 0;
  ar::Type* stype = nullptr;
  if (off.is_top()) {
    structural_field_of(store->pointer(), soff, stype);
  }
  this->_accesses.push_back(
      AccessRecord{store,
                   call_context,
                   AccessKind::Write,
                   std::move(pts),
                   this->snapshot_locks(inv, branch_guard_of(store)),
                   this->snapshot_read_locks(inv),
                   off,
                   base,
                   instance,
                   has_region,
                   region,
                   std::move(tid),
                   unique,
                   this->snapshot_joined_threads(inv),
                   this->snapshot_spawned_threads(inv),
                   this->snapshot_spawned_instances(inv),
                   this->snapshot_spawned_is_top(inv),
                   store->is_atomic(),
                   provenance,
                   /*private_init=*/false,
                   /*promote_locks=*/{},
                   soff,
                   stype});
}

} // end namespace analyzer
} // end namespace ikos