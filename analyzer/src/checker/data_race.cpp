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

#include <ikos/ar/semantic/pthread.hpp>
#include <ikos/analyzer/checker/data_race.hpp>
#include <ikos/analyzer/analysis/source_call_abi.hpp>
#include <ikos/analyzer/checker/race_rules.hpp>
#include <ikos/analyzer/checker/init_publication.hpp>
#include <ikos/analyzer/checker/first_publication.hpp>
#include <ikos/analyzer/checker/mutex_handover.hpp>
#include <ikos/analyzer/checker/guarded_publication.hpp>
#include <ikos/analyzer/checker/sc_protocol.hpp>
#include <ikos/analyzer/checker/self_publication.hpp>
#include <ikos/analyzer/checker/thread_private_heap.hpp>
#include <ikos/analyzer/checker/arena_reservation.hpp>
#include <ikos/analyzer/checker/array_slices.hpp>
#include <ikos/analyzer/checker/readonly_heap_reads.hpp>
#include <ikos/analyzer/checker/bucket_list_ownership.hpp>
#include <ikos/analyzer/checker/thread_lifecycle.hpp>
#include <ikos/analyzer/checker/binary_token.hpp>
#include <ikos/analyzer/checker/once_initialization.hpp>
#include <ikos/analyzer/checker/thread_completion.hpp>
#include <ikos/analyzer/analysis/execution_engine/pthread_initialization.hpp>
#include <ikos/analyzer/analysis/execution_engine/pthread_attributes.hpp>
#include <ikos/analyzer/analysis/execution_engine/pthread_key_effects.hpp>
#include <ikos/analyzer/analysis/execution_engine/stdio_effects.hpp>
#include <ikos/analyzer/analysis/runtime_stdio_frame.hpp>
#include <ikos/analyzer/analysis/pthread_initialization_frame.hpp>
#include <ikos/analyzer/analysis/posix_semaphore.hpp>
#include <ikos/analyzer/analysis/posix_barrier.hpp>
#include <ikos/analyzer/analysis/bmc/bmc.hpp>
#include <ikos/analyzer/analysis/bmc/unroll.hpp>
#include <ikos/analyzer/analysis/pair_exclusion.hpp>
#include <ikos/analyzer/analysis/region_ownership.hpp>
#include <ikos/analyzer/analysis/execution_engine/symbolic_index.hpp>
#include <ikos/analyzer/analysis/pointer/function.hpp>
#include <ikos/analyzer/support/cast.hpp>
#include <ikos/analyzer/util/log.hpp>
#include <ikos/analyzer/util/timer.hpp>
#include <ikos/analyzer/util/source_location.hpp>

#include <ikos/core/domain/concurrent_global_env.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace ikos {
namespace analyzer {

namespace {

// A fresh source nondet and its pure SSA expressions are independent of shared
// memory and of the caller's context. Loads, parameters and phi are excluded.
bool bmc_local_integer(ar::Value* value, unsigned depth = 0) {
  if (depth >= 32 || !value->type()->is_integer()) return false;
  if (isa< ar::IntegerConstant >(value)) return true;
  auto* variable = dyn_cast< ar::InternalVariable >(value);
  if (variable == nullptr) return false;
  auto* definition = bmc::unique_def_bmc(variable);
  if (definition == nullptr) return false;
  if (auto* call = dyn_cast< ar::CallBase >(definition)) return bmc::source_nondet(call);
  if (!isa< ar::Assignment >(definition) && !isa< ar::UnaryOperation >(definition) &&
      !isa< ar::BinaryOperation >(definition)) return false;
  for (auto i = definition->op_begin(); i != definition->op_end(); ++i)
    if (!bmc_local_integer(*i, depth + 1)) return false;
  return true;
}

bool bmc_shared_independent_control(ar::Code* code) {
  for (auto* block : *code) {
    for (auto* statement : *block) {
      if (bmc::abstract_initializer(statement)) continue;
      if (auto* comparison = dyn_cast< ar::Comparison >(statement)) {
        if (!bmc_local_integer(comparison->left()) || !bmc_local_integer(comparison->right())) return false;
      } else if (auto* call = dyn_cast< ar::CallBase >(statement)) {
        if (bmc::source_nondet(call)) continue;
        if (bmc::is_assume_call(call) && call->num_arguments() == 1 && bmc_local_integer(call->argument(0))) continue;
        // A callee may hide a shared-dependent assume or branch refinement.
        return false;
      }
    }
  }
  return true;
}

/// \brief Canonical branch guard of a statement directly guarded by an
/// `if (guard)` branch: a fixed, zero-equivalent AR variable identity, or 0
/// when the statement is not guarded this way. Mirrors the lock-side branch
/// detection in numerical.hpp: the access sits in the TRUE branch, and the
/// sibling (false branch) carries the Comparison `guard == 0`.
std::uint64_t branch_guard_of(ar::Statement* stmt, Context& ctx) {
  auto* function = stmt->code()->function_or_null();
  if (function == nullptr ||
      !ctx.concurrent_env.function_executes_at_most_once(function)) return 0;
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
  return nonzero_guard_identity(cmp->left());
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
        auto pointer_value = core::ConcurrentGlobalEnv::PointerValue::top(64, core::Unsigned);
        auto& env = ctx.concurrent_env;
        if (!env.get_global_pointer(glob_loc, pointer_value) ||
            pointer_value.points_to().is_top()) {
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

/// \brief Collect symbolic slots through shifts, casts, copies and load bases.
/// Callers use a syntactic alias gate; it is not a complete non-escape proof.
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
          // Propagate the load address's region, subject to the caller's gate.
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

DataRaceChecker::DataRaceChecker(Context& ctx) : Checker(ctx) {
  {
    ScopeTimerDatabase timer(ctx.output_db->times,
                             "ikos-analyzer.prepare.region-ownership-inventory");
    this->_ownership.reset(new RegionOwnership(ctx));
  }
  this->_argument_windows = ctx.thread_argument_windows.get();
  this->_lifecycle_observations.reset(new ThreadLifecycleObservations(ctx));
  const char* configured = std::getenv("IKOS_AI_PAIR_EXCLUSION");
  const std::string mode = configured ? configured : "";
  this->_pair_exclusion_enabled = mode == "1" || mode == "native";
  this->_pair_exclusion_solver = mode == "1";
}

DataRaceChecker::~DataRaceChecker() = default;

void DataRaceChecker::finalize() {
  if (this->_phase != Phase::Collecting) {
    throw std::logic_error("data-race checks finalized more than once");
  }
  this->_phase = Phase::Finalizing;
  try {
    this->finalize_checks();
    this->_phase = Phase::Complete;
  } catch (...) {
    this->_phase = Phase::Failed;
    throw;
  }
}

void DataRaceChecker::finalize_checks() {
  ScopeTimerDatabase timer(this->_ctx.output_db->times,
                           "ikos-analyzer.race.finalize");
  {
    ScopeTimerDatabase owner_timer(this->_ctx.output_db->times,
                                   "ikos-analyzer.prepare.region-ownership");
    this->_ownership->run();
  }
  Timer preparation;
  preparation.start();
  ReadonlyHeapReads readonly_heap_reads(this->_ctx);
  preparation.stop();
  this->_ctx.output_db->times.insert("ikos-analyzer.prepare.readonly-heap",
                                     preparation.elapsed().count());
  preparation.start();
  BucketListOwnership bucket_list_ownership(this->_ctx, this->_accesses);
  preparation.stop();
  this->_ctx.output_db->times.insert("ikos-analyzer.prepare.bucket-ownership",
                                     preparation.elapsed().count());
  this->check_effect_coverage(readonly_heap_reads, bucket_list_ownership);
  this->refresh_coverage();
  if (this->_coverage.state != RaceCoverageSummary::State::Checked) {
    this->publish_result("UNKNOWN");
    return;
  }
  this->prepare_access_regions();
  JoinedNames joined_names;
  auto all_pairs = this->collect_race_pairs(readonly_heap_reads,
                                          bucket_list_ownership, joined_names);
  this->_remaining_pairs = all_pairs.size();
  for (const auto& pair : all_pairs) {
    if (pair.is_unknown)
      this->add_coverage_obligation("mixed_atomic", pair.report_stmt,
                                    "mixed atomic/non-atomic accesses");
  }
  this->refresh_coverage();
  if (this->_coverage.state == RaceCoverageSummary::State::Checked &&
      (all_pairs.empty() || this->_coverage.safety_only == 0)) {
    this->search_race_candidates(all_pairs, joined_names);
  }
  // Search and auxiliary consumers cannot publish against a stale checkpoint.
  this->refresh_coverage();
  const bool publish = this->_coverage.state == RaceCoverageSummary::State::Checked &&
                       this->_coverage.safety_only == 0;
  bool confirmed = false;
  if (publish) {
    for (const auto& report : this->_pending_reports) {
      this->_checks.insert(CheckKind::DataRace, CheckerName::DataRace,
                           report.confirmed ? Result::Error : Result::Warning,
                           report.statement, report.context, {}, report.info);
      confirmed |= report.confirmed;
    }
  }
  this->publish_result(confirmed ? "FALSE" :
      this->_coverage.state == RaceCoverageSummary::State::Checked && all_pairs.empty()
          ? "SAFE" : "UNKNOWN");
}

void DataRaceChecker::add_coverage_obligation(const std::string& source,
                                             ar::Statement* statement,
                                             const std::string& reason) {
  for (const auto& obligation : this->_coverage.obligations)
    if (obligation.source == source && obligation.statement == statement &&
        obligation.reason == reason) return;
  this->_coverage.obligations.push_back({source, statement, reason, ""});
}

void DataRaceChecker::refresh_coverage() {
  for (auto* call : this->_ctx.concurrent_env.unmodeled_thread_creations())
    this->add_coverage_obligation("thread_entry", call, "thread entry body not covered");
  for (const auto& effect : this->_ctx.concurrent_env.unmodeled_effects())
    this->add_coverage_obligation("effect", effect.first, effect.second);
  for (const auto& reason : this->_ctx.race_entry_obligations)
    this->add_coverage_obligation("required_entry", nullptr, reason);
  if (this->_ctx.concurrent_env.is_dirty())
    this->add_coverage_obligation("stability", nullptr,
                                 "shared summaries changed after convergence");
  if (!this->_ctx.race_analysis_complete)
    this->add_coverage_obligation("completion", nullptr, "analysis not complete");
  this->_coverage.unresolved = 0;
  this->_coverage.safety_only = 0;
  for (const auto& obligation : this->_coverage.obligations) {
    if (obligation.contract.empty()) ++this->_coverage.unresolved;
    else ++this->_coverage.safety_only;
  }
  this->_coverage.state = this->_coverage.unresolved != 0
      ? RaceCoverageSummary::State::Blocked : RaceCoverageSummary::State::Checked;
}

void DataRaceChecker::publish_result(const char* verdict) {
  JsonList obligations;
  for (const auto& obligation : this->_coverage.obligations) {
    JsonDict item{{"source", obligation.source}, {"reason", obligation.reason},
                  {"contract", obligation.contract},
                  {"safety_only", !obligation.contract.empty()}};
    if (obligation.statement != nullptr) {
      std::ostringstream statement;
      obligation.statement->dump(statement);
      item.put("statement", statement.str());
      if (auto loc = source_location(obligation.statement)) {
        item.put("file", loc.path().string());
        item.put("line", loc.line());
        item.put("column", loc.column());
      }
    }
    obligations.add(item);
  }
  this->_ctx.output_db->settings.insert("race_result", JsonDict{
      {"schema", 1}, {"policy", "strict-coverage-v1"},
      {"analysis_complete", this->_ctx.race_analysis_complete},
      {"checks_complete", true},
      {"coverage", this->_coverage.state == RaceCoverageSummary::State::Checked
                       ? "Checked" : "Blocked"},
      {"obligation_count", this->_coverage.obligations.size()},
      {"unresolved_count", this->_coverage.unresolved},
      {"safety_only_count", this->_coverage.safety_only},
      {"obligations", obligations}, {"verdict", verdict},
      {"remaining_pairs", this->_remaining_pairs},
      {"search_performed", this->_search_performed}});
}

void DataRaceChecker::check_effect_coverage(
    ReadonlyHeapReads& readonly_heap_reads,
    BucketListOwnership& bucket_list_ownership) {
  this->refresh_coverage();
  for (ar::CallBase* call : this->_ctx.concurrent_env.unmodeled_thread_creations()) {
    this->_checks.insert(
        CheckKind::UnknownFunctionCallPointer, CheckerName::DataRace,
        Result::Warning, call, this->_ctx.call_context_factory->get_empty(),
        {call->argument(2)},
        {{"unknown_reason", "thread entry body not covered"}});
  }

  for (const auto& effect : this->_ctx.concurrent_env.unmodeled_effects()) {
    auto evaluate_effect = [&]() -> boost::optional< CoverageDecision > {
      if (effect.second == source_call_abi_reason()) return boost::none;
      if (effect.second == "synthetic pthread handle representation not modeled" &&
          readonly_heap_reads.certifies(effect.first)) {
        auto proof = readonly_heap_reads.info();
        proof.put("safe_reason", "bounded immutable heap read cannot consume a pthread handle");
        return CoverageDecision{race_rules::Id::CoverageReadonlyHeap,
                                effect.first, effect.second, std::move(proof)};
      }
      if (bucket_list_ownership.certifies(effect.first, effect.second)) {
        auto proof = bucket_list_ownership.info();
        proof.put("safe_reason", "complete graph source and fully accounted byte effects");
        return CoverageDecision{race_rules::Id::CoverageBucketOwnership,
                                effect.first, effect.second, std::move(proof)};
      }
      return boost::none;
    };
    if (auto proof = evaluate_effect()) {
      this->consume_coverage_proof(*proof);
      continue;
    }
    ar::Statement* anchor = effect.first;
    JsonDict info{{"unknown_reason", effect.second}};
    if (!anchor->code()->is_function_body()) {
      // The report schema requires a function statement; a global initializer
      // can still invalidate coverage even when no access pair is collected.
      info.put("effect_origin", "global initialization");
      for (auto* entry : this->_ctx.opts.entry_points) {
        if (!entry->is_definition()) continue;
        for (auto* block : *entry->body()) {
          for (auto* statement : *block) {
            if (statement->has_frontend()) { anchor = statement; break; }
          }
          if (anchor->code()->is_function_body()) break;
        }
        if (anchor->code()->is_function_body()) break;
      }
    }
    if (anchor->code()->is_function_body())
      this->_checks.insert(
          CheckKind::DataRace, CheckerName::DataRace, Result::Warning,
          anchor, this->_ctx.call_context_factory->get_empty(), {}, info);
  }

}

void DataRaceChecker::consume_coverage_proof(const CoverageDecision& proof) {
  if (this->_phase != Phase::Finalizing || proof.effect == nullptr ||
      proof.reason == source_call_abi_reason() ||
      !race_rules::permits_effect_coverage(proof.rule)) {
    throw std::logic_error("invalid effect coverage proof consumption");
  }
  bool registered = false;
  for (const auto& effect : this->_ctx.concurrent_env.unmodeled_effects())
    registered |= effect.first == proof.effect && effect.second == proof.reason;
  if (!registered) throw std::logic_error("coverage proof has no matching obligation");
  for (auto& obligation : this->_coverage.obligations)
    if (obligation.source == "effect" && obligation.statement == proof.effect &&
        obligation.reason == proof.reason)
      obligation.contract = race_rules::contract(proof.rule).id;
  if (!proof.effect->code()->is_function_body()) return;
  this->_checks.insert(CheckKind::DataRace, CheckerName::DataRace, Result::Ok,
                       proof.effect, this->_ctx.call_context_factory->get_empty(),
                       {}, proof.report);
}

void DataRaceChecker::prepare_access_regions() {
  // Temporary probe (env IKOS_BMC_DUMP): dump the BMC events (memory + mutex
  // sync) for every function, verifying the event extraction on real code.
  // Remove once the BMC module is wired in.
  if (getenv("IKOS_BMC_DUMP")) {
    for (auto fit = this->_ctx.bundle->function_begin(),
              fend = this->_ctx.bundle->function_end();
         fit != fend; ++fit) {
      ar::Function* fun = *fit;
      if (!fun->is_definition()) {
        continue;
      }
      ar::Code* body = fun->body_or_null();
      if (body == nullptr) {
        continue;
      }
      std::cerr << "[BMC-DUMP] " << fun->name() << "\n";
      for (auto bit = body->begin(), bend = body->end(); bit != bend; ++bit) {
        for (auto sit = (*bit)->begin(), send = (*bit)->end(); sit != send;
             ++sit) {
          ar::Statement* s = *sit;
          bmc::BmcEvent ev(nullptr, bmc::EvKind::Read);
          if (!bmc::classify_event(s, ev)) {
            continue;
          }
          const char* k = "?";
          switch (ev.kind) {
            case bmc::EvKind::Read: k = "READ"; break;
            case bmc::EvKind::Write: k = "WRITE"; break;
            case bmc::EvKind::Lock: k = "LOCK"; break;
            case bmc::EvKind::Unlock: k = "UNLOCK"; break;
            default: break;
          }
          std::cerr << "    " << k;
          if (auto loc = source_location(s)) {
            std::cerr << " @" << loc.line();
          }
          std::cerr << "\n";
        }
      }
    }
  }

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

}

void DataRaceChecker::consume_pair_proof(std::size_t i, std::size_t j,
                                        const PairDecision& decision) {
  if (this->_phase != Phase::Finalizing || i >= this->_accesses.size() ||
      j >= this->_accesses.size() || !decision.excluded() ||
      !race_rules::permits_pair_exclusion(decision.rule)) {
    throw std::logic_error("invalid race pair proof consumption");
  }
  const auto& a = this->_accesses[i];
  ++this->_pair_rule_counts[static_cast< std::size_t >(decision.rule)];
  if (decision.report) {
    this->_checks.insert(CheckKind::DataRace, CheckerName::DataRace, Result::Ok,
                         a.stmt, a.call_context, {}, *decision.report);
  }
}


void DataRaceChecker::report_race_group(const PendingRacePair& selected,
                                      bool group_is_unknown,
                                      bool bmc_confirmed,
                                      const JsonDict& info) {
  this->_pending_reports.push_back({selected.report_stmt, selected.report_cc,
                                     !group_is_unknown && bmc_confirmed, info});
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
  if (this->_phase != Phase::Collecting) {
    throw std::logic_error("data-race access recorded after finalization");
  }

  if (inv.first().is_normal_flow_bottom()) {
    // Statement is unreachable
    return;
  }

  if (unsupported_source_function_abi(stmt->code()->function_or_null())) {
    const std::string reason = source_call_abi_reason();
    this->add_coverage_obligation("source_abi", stmt, reason);
    this->_checks.insert(CheckKind::DataRace, CheckerName::DataRace,
                         Result::Warning, stmt, call_context, {},
                         {{"unknown_reason", reason}});
    return;
  }

  this->_ownership->observe(stmt, inv, call_context);
  this->_lifecycle_observations->observe(stmt, inv, call_context);
  if (this->_argument_windows)
    this->_argument_windows->observe(stmt, inv, call_context);

  if (auto* operation = dyn_cast< ar::BinaryOperation >(stmt)) {
    if (this->_ctx.opts.demote_race_to_unknown && getenv("IKOS_BMC_RECOVER") != nullptr && operation->has_no_wrap()) {
      using IntInterval = core::machine_int::Interval;
      using ZInterval = core::numeric::Interval< core::ZNumber >;
      using ZBound = core::ZBound;
      bool proven = false;
      auto* type = dyn_cast< ar::IntegerType >(operation->result()->type());
      auto code = stmt->code();
      auto inserted = this->_bmc_local_control.emplace(code, false);
      if (inserted.second) inserted.first->second = bmc_shared_independent_control(code);
      if (race_rules::permits_execution_premise(race_rules::Id::BmcLocalInteger) &&
          race_rules::permits_execution_premise(race_rules::Id::BmcLocalControl) &&
          type != nullptr && inserted.first->second && bmc_local_integer(operation->left()) && bmc_local_integer(operation->right())) {
        auto interval = [&](ar::Value* value) {
          const auto& literal = this->_lit_factory.get_scalar(value);
          if (literal.is_machine_int()) return IntInterval(literal.machine_int()).to_z_interval();
          if (literal.is_machine_int_var() && !inv.first().normal().uninit_is_uninitialized(literal.var()))
            return inv.first().normal().int_to_interval(literal.var()).to_z_interval();
          return ZInterval::bottom();
        };
        auto left = interval(operation->left()), right = interval(operation->right());
        auto result = ZInterval::bottom();
        switch (operation->op()) {
          case ar::BinaryOperation::SAdd: case ar::BinaryOperation::UAdd: result = left + right; break;
          case ar::BinaryOperation::SSub: case ar::BinaryOperation::USub: result = left - right; break;
          case ar::BinaryOperation::SMul: case ar::BinaryOperation::UMul: result = left * right; break;
          default: break;
        }
        proven = !result.is_bottom() &&
                 result.lb() >= ZBound(MachineInt::min(type->bit_width(), type->sign()).to_z_number()) &&
                 result.ub() <= ZBound(MachineInt::max(type->bit_width(), type->sign()).to_z_number());
      }
      auto proof = this->_bmc_no_wrap.emplace(operation, proven);
      if (!proof.second) proof.first->second = proof.first->second && proven;
    }
  }

  if (auto load = dyn_cast< ar::Load >(stmt)) {
    this->check_load(load, inv, call_context);
  } else if (auto store = dyn_cast< ar::Store >(stmt)) {
    this->check_store(store, inv, call_context);
  } else if (auto call = dyn_cast< ar::CallBase >(stmt)) {
    if (this->_ctx.pthread_initialization_frame()) {
      auto* requirement = this->_ctx.pthread_initialization_frame()->array_access(call);
      if (requirement) {
        const auto& index = this->_lit_factory.get_scalar(requirement->index);
        auto* index_type = dyn_cast< ar::IntegerType >(requirement->index->type());
        auto bounds = core::machine_int::Interval::top(
            index_type ? index_type->bit_width() : 1,
            index_type ? index_type->sign() : Signed);
        if (index.is_machine_int()) bounds = core::machine_int::Interval(index.machine_int());
        else if (index.is_machine_int_var() &&
                 inv.first().normal().uninit_is_initialized(index.var()))
          bounds = inv.first().normal().int_to_interval(index.var());
        auto points = this->resolve_points_to(call->argument(0), inv);
        const auto& pointer = this->_lit_factory.get_scalar(call->argument(0));
        bool proven = index_type && pointer.is_pointer_var() &&
            inv.first().normal().nullity_is_non_null(pointer.var()) &&
            points.is_set() && points.size() == 1 &&
            *points.begin() == this->_ctx.mem_factory->get_global(requirement->global) &&
            !bounds.is_bottom() && !bounds.is_top() &&
            bounds.lb().to_z_number() >= 0 && bounds.ub().to_z_number() < requirement->count;
        if (!proven) {
          const std::string reason = "mutex array consumer does not establish an initialized leaf";
          this->add_coverage_obligation("mutex_array", call, reason);
          this->_checks.insert(CheckKind::DataRace, CheckerName::DataRace,
                               Result::Warning, call, call_context, {},
                               {{"unknown_reason", reason}});
        }
      }
    }
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
  // the final pair meet (⊤ is the identity) pairs it against EVERY shared
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

boost::optional< core::machine_int::Congruence >
DataRaceChecker::resolve_offset_congruence(
    ar::Value* pointer, const value::AbstractDomain& inv) const {
  if (!this->_pair_exclusion_enabled) return boost::none;
  const ScalarLit& ptr = this->_lit_factory.get_scalar(pointer);
  if (!ptr.is_pointer_var() || ptr.is_undefined() ||
      !inv.first().normal().uninit_is_initialized(ptr.var())) return boost::none;
  return inv.first().normal().pointer_offset_to_congruence(ptr.var());
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
  // (`if (i) lock`): the key is a fixed, zero-equivalent AR value identity,
  // including same-width constant add/sub cancellation. The access's branch
  // must prove the same identity (and hence the same nonzero predicate).
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

/// 解析 pthread_create 的 start_routine（arg2）到线程函数：沿 value-preserving
/// cast 解开。签名不匹配的 `(void*(*)(void*)) myPartOfCalc` 会包一层 Bitcast，
/// 直接 dyn_cast 到 FunctionPointerConstant 会失败 → create-edge HB 失效
/// （pthread-numerical-integration 的 numberOfIntervals 误判）。与
/// concurrent_semantics.hpp exec_pthread_create 的解析对齐。
static ar::Function* resolve_thread_function(ar::Value* v) {
  for (int depth = 0; depth < 8; ++depth) {
    if (auto* fpc = dyn_cast< ar::FunctionPointerConstant >(v)) {
      return fpc->function();
    }
    auto* iv = dyn_cast< ar::InternalVariable >(v);
    if (iv == nullptr) {
      return nullptr;
    }
    ar::Statement* def = bmc::unique_def_bmc(iv);
    if (auto* un = dyn_cast_or_null< ar::UnaryOperation >(def)) {
      if (un->op() == ar::UnaryOperation::Bitcast) {
        v = un->operand();
        continue;
      }
    }
    return nullptr;
  }
  return nullptr;
}

void DataRaceChecker::build_thread_creators() {
  // Static scan of pthread_create call sites: child entry name -> set of
  // creator entry names. Mirrors the engine-side detection
  // (NumericalExecutionEngine's pthread_create handler): direct call via
  // FunctionPointerConstant named "pthread_create", thread function given
  // as a constant in argument slot 2. Creators are identified by entry
  // name — the same identity `current_thread_id` assigns to access
  // records. A create inside a non-entry helper is attributed to the entry
  // functions that (transitively) call the helper, so a parent write before
  // the call still happens-before the child (race-1_1/1_2/1_3-join.c: the
  // create is in `module_init`, reached only from `main`).
  //
  // Set of all thread-entry names (potential creators): "main" plus every
  // thread function discovered during analysis. A helper whose address is
  // taken gets the WHOLE set as its creator set — a non-unique (TOP) set that
  // disables the create-edge HB for that child, keeping the uniqueness claim
  // sound when the helper may be reached through an indirect call we can't see.
  std::unordered_set< std::string > all_entries;
  all_entries.insert("main");
  for (ar::Function* tf : this->_ctx.concurrent_env.get_all_thread_functions()) {
    if (tf != nullptr) {
      all_entries.insert(tf->name());
    }
  }

  // Reverse DIRECT call graph (callee name -> direct callers) and the set of
  // functions whose address is taken (a FunctionPointerConstant used anywhere
  // other than as a direct-call target: stored, passed, or in a global
  // initializer). A function-pointer constant as a call ARGUMENT (pthread
  // create's start_routine) is an address-taken use, not a direct call.
  std::unordered_map< std::string, std::unordered_set< std::string > > callers;
  std::unordered_set< std::string > address_taken;
  auto scan_operands = [&](ar::Code* body, const std::string& owner) {
    for (auto bit = body->begin(), bend = body->end(); bit != bend; ++bit) {
      ar::BasicBlock* bb = *bit;
      for (auto sit = bb->begin(), send = bb->end(); sit != send; ++sit) {
        ar::Statement* stmt = *sit;
        if (auto* call = dyn_cast< ar::CallBase >(stmt)) {
          if (auto* cst =
                  dyn_cast< ar::FunctionPointerConstant >(call->called())) {
            if (cst->function() != nullptr) {
              callers[cst->function()->name()].insert(owner);
            }
          }
          for (auto it = call->arg_begin(), et = call->arg_end(); it != et;
               ++it) {
            if (auto* fpc = dyn_cast< ar::FunctionPointerConstant >(*it)) {
              if (fpc->function() != nullptr) {
                address_taken.insert(fpc->function()->name());
              }
            }
          }
        } else {
          for (auto it = stmt->op_begin(), et = stmt->op_end(); it != et;
               ++it) {
            if (auto* fpc = dyn_cast< ar::FunctionPointerConstant >(*it)) {
              if (fpc->function() != nullptr) {
                address_taken.insert(fpc->function()->name());
              }
            }
          }
        }
      }
    }
  };
  for (auto fit = this->_ctx.bundle->function_begin(),
            fend = this->_ctx.bundle->function_end();
       fit != fend;
       ++fit) {
    ar::Function* fun = *fit;
    if (!fun->is_definition()) {
      continue;
    }
    if (ar::Code* body = fun->body_or_null()) {
      scan_operands(body, fun->name());
    }
  }
  // Global function-pointer initializers (e.g. `void (*fp)(void) = helper;`)
  // mark the helper address-taken: it may be called from any thread entry.
  for (auto git = this->_ctx.bundle->global_begin(),
            gend = this->_ctx.bundle->global_end();
       git != gend;
       ++git) {
    ar::Code* init = (*git)->initializer_or_null();
    if (init == nullptr) {
      continue;
    }
    for (auto bit = init->begin(), bend = init->end(); bit != bend; ++bit) {
      ar::BasicBlock* bb = *bit;
      for (auto sit = bb->begin(), send = bb->end(); sit != send; ++sit) {
        ar::Statement* stmt = *sit;
        for (auto it = stmt->op_begin(), et = stmt->op_end(); it != et; ++it) {
          if (auto* fpc = dyn_cast< ar::FunctionPointerConstant >(*it)) {
            if (fpc->function() != nullptr) {
              address_taken.insert(fpc->function()->name());
            }
          }
        }
      }
    }
  }

  // Attribute each pthread_create's child to its possible creators.
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
            !ar::is_pthread_create(cst->function())) {
          continue;
        }
        if (call->arg_begin() + 2 == call->arg_end()) {
          continue;
        }
        ar::Function* tfunc =
            resolve_thread_function(*(call->arg_begin() + 2));
        if (tfunc == nullptr) {
          continue;
        }
        const std::string child = tfunc->name();

        if (fun->name() == "main") {
          this->_thread_creators[child].insert("main");
        } else if (this->_ctx.concurrent_env.is_thread_entry(fun)) {
          this->_thread_creators[child].insert(fun->name());
        } else if (address_taken.count(fun->name()) != 0) {
          // Helper reachable through an unknown (indirect) call: any entry may
          // be the creator -> non-unique (TOP) creator set.
          for (const std::string& e : all_entries) {
            this->_thread_creators[child].insert(e);
          }
        } else {
          // Helper reachable only through direct calls: the creator set is the
          // entries that transitively call it (reverse BFS over direct callers).
          std::vector< std::string > work{fun->name()};
          std::unordered_set< std::string > seen{fun->name()};
          while (!work.empty()) {
            std::string cur = work.back();
            work.pop_back();
            auto cit = callers.find(cur);
            if (cit == callers.end()) {
              continue;
            }
            for (const std::string& caller : cit->second) {
              if (all_entries.count(caller) != 0) {
                this->_thread_creators[child].insert(caller);
              } else if (seen.insert(caller).second) {
                work.push_back(caller);
              }
            }
          }
        }

        this->_thread_creations.push_back(ThreadCreation{stmt, child});
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
      // Fresh allocation alone does not prove absence of transitive publication.
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

void DataRaceChecker::consume_library_effect(
    race_rules::Id rule, ar::CallBase* call, CallContext* context,
    boost::optional< JsonDict > report) {
  if (this->_phase != Phase::Collecting || call == nullptr ||
      !race_rules::permits_library_effect(rule)) {
    throw std::logic_error("invalid library effect proof consumption");
  }
  if (report) {
    this->_checks.insert(CheckKind::DataRace, CheckerName::DataRace, Result::Ok,
                         call, context, {}, *report);
  }
}

void DataRaceChecker::check_extern_call_effects(
    ar::CallBase* call,
    const value::AbstractDomain& inv,
    CallContext* call_context) {
  // An unsupported source ABI is a separate, non-dischargeable obligation.
  auto reject_abi = [&]() {
    const std::string reason = source_call_abi_reason();
    this->add_coverage_obligation("source_abi", call, reason);
    this->_checks.insert(CheckKind::DataRace, CheckerName::DataRace,
                         Result::Warning, call, call_context, {},
                         {{"unknown_reason", reason}});
  };
  if (unsupported_source_call_abi(call)) {
    reject_abi();
    return;
  }
  // Determine whether the callee has no analyzable body (extern).
  ar::Value* called = call->called();
  bool extern_call = false;
  auto thread_unsafe = [](ar::Function* function) {
    return function->is_declaration() &&
           (function->name() == "rand" || function->name() == "srand" ||
            function->name() == "ar.libc.rand" || function->name() == "ar.libc.srand");
  };
  bool may_thread_unsafe = false;
  if (auto* cst = dyn_cast< ar::FunctionPointerConstant >(called)) {
    if (unsupported_source_call_abi(call, cst->function())) {
      reject_abi();
      return;
    }
    extern_call = cst->function()->is_declaration();
    may_thread_unsafe = thread_unsafe(cst->function());
  } else if (auto* ptr = dyn_cast< ar::InternalVariable >(called)) {
    Variable* pv = this->_ctx.var_factory->get_internal(ptr);
    PointsToSet cpts = inv.first().normal().pointer_to_points_to(pv);
    if (cpts.is_top()) {
      extern_call = true; // unknown target: conservatively extern
    } else if (!cpts.is_bottom() && cpts.is_set() && cpts.size() > 0) {
      extern_call = false;
      for (MemoryLocation* loc : cpts) {
        auto* fm = dyn_cast< FunctionMemoryLocation >(loc);
        if (fm == nullptr || fm->function()->is_declaration()) {
          extern_call = true; // any unmodeled target may access user memory
        }
        if (fm && unsupported_source_call_abi(call, fm->function())) {
          reject_abi();
          return;
        }
        if (fm != nullptr) may_thread_unsafe |= thread_unsafe(fm->function());
      }
    }
  } else {
    return; // undefined / null / asm / data: no side-effect synthesis
  }
  if (!extern_call) {
    return;
  }
  if (const auto* effect = closed_pthread_attribute_effect(call, this->_ctx)) {
    if (closed_pthread_attribute_range(*effect, call, inv.first(), this->_ctx)) {
      this->consume_library_effect(race_rules::Id::EffectPthreadAttributes, call, call_context,
          JsonDict{{"safe_reason", "closed private default pthread attribute byte effects"},
                   {"pthread_attributes_contract", "closed-private-default-pthread-attributes-v1"}});
      return;
    }
    // Monotone, call-scoped debt: a later successful visit cannot erase a mismatch.
    this->add_coverage_obligation("pthread_attributes", call,
        "private pthread attribute current range is not established");
  }
  if (this->_ctx.posix_barrier() && this->_ctx.posix_barrier()->effect(call)) {
    this->consume_library_effect(race_rules::Id::EffectBarrier, call, call_context,
        JsonDict{{"safe_reason", "sealed opaque GNU barrier control storage"},
                 {"barrier_effect_contract", "closed-gnu-barrier-effects-v1"}});
    return;
  }
  if (this->_ctx.posix_semaphore() && this->_ctx.posix_semaphore()->effect(call)) {
    this->consume_library_effect(race_rules::Id::EffectSemaphore, call, call_context,
        JsonDict{{"safe_reason", "sealed opaque GNU semaphore control storage"},
                 {"semaphore_effect_contract", "closed-gnu-semaphore-effects-v1"}});
    return;
  }
  // Reuse the verified literal/integer-only summary, not a stdio name family:
  // buffer destinations, %n and pointer conversions can access shared data.
  if (bmc::constant_output_call(call)) {
    this->consume_library_effect(race_rules::Id::EffectConstantOutput,
                                 call, call_context);
    return;
  }

  auto scan = scalar_scan_effect(call);
  if (scan && !scan->implicit_state_closed) scan = boost::none;
  if (scan) {
    this->consume_library_effect(race_rules::Id::EffectScalarScan, call, call_context,
        JsonDict{{"safe_reason", "exact scalar scanf application-memory effects"},
                 {"stdio_scan_contract", "scalar-percent-d-output-and-errno-v1"}});
  }

  const auto key_effect = pthread_key_effect(call, this->_ctx);
  if (key_effect == PthreadKeyEffect::Get || key_effect == PthreadKeyEffect::Set) {
    this->consume_library_effect(race_rules::Id::EffectTlsSlot, call, call_context);
    return; // Runtime TLS slots, not memory pointed to by the stored value.
  }
  if (key_effect == PthreadKeyEffect::Create &&
      !isa< ar::NullConstant >(call->argument(1))) {
    auto* callback = cast< ar::FunctionPointerConstant >(call->argument(1))->function();
    this->consume_library_effect(race_rules::Id::EffectTlsDestructor, call, call_context,
        JsonDict{{"safe_reason", "complete TLS destructor has no application-memory or synchronization effects"},
                 {"tls_destructor_contract", "race-only-closed-memory-free-callback-v1"},
                 {"callback", callback->name()}});
  }

  auto* exact = bmc::exact_callee(call);
  const std::string callee_nm = exact != nullptr ? exact->name() : "";
  const auto* byte_effect = this->_ctx.pthread_initialization_frame()
      ? this->_ctx.pthread_initialization_frame()->effect(call) : nullptr;
  const bool initialization = byte_effect || certified_pthread_initialization(call, this->_ctx);
  if (byte_effect) {
    this->consume_library_effect(race_rules::Id::EffectPthreadInitialization, call, call_context,
        JsonDict{{"safe_reason", "closed pthread initialization byte effects"},
                 {"pthread_initialization_contract", byte_effect->complete_prefix
                     ? "finite-global-array-runtime-byte-frame-v1"
                     : "constant-heap-runtime-byte-frame-v1"}});
  }
  unsigned ignored_arguments = 0;
  if (exact != nullptr && exact->is_declaration()) {
    // Preserve the existing lifetime/stack handling separately from user buffers.
    // Lifetime conflicts still need their own property and object model audit.
    if (exact->intrinsic_id() == ar::Intrinsic::LibcFree ||
        exact->intrinsic_id() == ar::Intrinsic::StackSave ||
        exact->intrinsic_id() == ar::Intrinsic::StackRestore) {
      this->consume_library_effect(race_rules::Id::EffectLifetimeBoundary,
                                   call, call_context);
      return;
    }

    // Only runtime-managed sync slots are exempt. Attribute/timeout inputs and
    // caller-owned outputs remain conservative accesses; names alone are no gate.
    ignored_arguments = synchronization_ignored_arguments(call, exact);
    if (ignored_arguments)
      this->consume_library_effect(race_rules::Id::EffectSyncArguments,
                                   call, call_context);
  }

  if (initialization) {
    this->consume_library_effect(race_rules::Id::EffectInitializationArguments,
                                 call, call_context);
    ignored_arguments = 0;
  }
  if (key_effect == PthreadKeyEffect::Create) {
    this->consume_library_effect(race_rules::Id::EffectTlsCreateOutput,
                                 call, call_context);
    ignored_arguments = 2;
  }
  const auto& frame = this->_ctx.runtime_stdio_frame_certificate();
  if (frame && frame->omits_stderr_write(call, call_context)) {
    ignored_arguments |= 1u; // Only fprintf's actual runtime stream argument.
    auto info = frame->info(call);
    info.put("safe_reason", "runtime stderr is outside the complete application effect frame");
    this->consume_library_effect(race_rules::Id::EffectRuntimeStdio,
                                 call, call_context, std::move(info));
  }

  // Default to a Write per pointer argument; an exact strcpy source is Read.
  // Other libraries may read or write through it (scanf(&x), fgets, ...). The
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
  std::size_t argument_index = 0;
  for (auto it = call->arg_begin(), et = call->arg_end(); it != et; ++it, ++argument_index) {
    if (argument_index < 4 && (ignored_arguments & (1u << argument_index)) != 0) continue;
    if (scan && argument_index == 1) continue; // Verified immutable format.
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
          auto* global = gml->global_var();
          // Symbol spelling is not an immutability proof (C asm aliases).
          bool is_const = global->has_frontend() &&
                          global->frontend< llvm::GlobalVariable >()->isConstant();
          if (!is_const) {
            all_const = false; // user global: writable
          }
        } else {
          any = true;
          all_const = false; // heap/stack: writable
        }
      }
      if (any && all_const) {
        this->consume_library_effect(race_rules::Id::EffectImmutableArguments,
                                     call, call_context);
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
                     (scan && argument_index == 0) ||
                     (exact != nullptr && argument_index == 1 &&
                             (exact->intrinsic_id() == ar::Intrinsic::LibcStrcpy ||
                              exact->intrinsic_id() == ar::Intrinsic::LibcStrcpyCheck ||
                              (initialization && callee_nm == "pthread_mutex_init")))
                         ? AccessKind::Read
                         : AccessKind::Write,
                     std::move(pts),
                     locks,
                     read_locks,
                     this->resolve_offset(arg, inv),
                     /* width = */ scan ? (argument_index == 2 ? 4u : 0u) :
                         key_effect == PthreadKeyEffect::Create ? 4u : byte_effect
                         ? (argument_index == 0 ? byte_effect->write_bytes : byte_effect->read_bytes)
                         : initialization
                         ? (argument_index == 0 && callee_nm == "pthread_mutex_init"
                                ? (this->_ctx.bundle->data_layout().pointers.bit_width == 32 ? 24u : 40u) : 4u)
                         : 0u,
                     base,
                     instance,
                     has_region,
                     region,
                     tid,
                     unique,
                     joined,
                     spawned,
                     spawned_instances,
                     spawned_is_top,
                     inv.second().get_live_children(),
                     inv.second().live_children_is_top()});
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
  if (may_thread_unsafe) {
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
                     /* width = */ 0,
                     core::IndexableTraits< MemoryLocation* >::index(libc_state),
                     /* instance = */ 0,
                     /* has_region = */ false,
                     core::ConcurrentGlobalEnv::SymbolicIndex{},
                     tid,
                     unique,
                     joined,
                     spawned,
                     spawned_instances,
                     spawned_is_top,
                     inv.second().get_live_children(),
                     inv.second().live_children_is_top()});
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
                         /* width = */ 0,
                         core::IndexableTraits< MemoryLocation* >::index(gml),
                         /* instance = */ 0,
                         /* has_region = */ false,
                         core::ConcurrentGlobalEnv::SymbolicIndex{},
                         tid,
                         unique,
                         joined,
                         spawned,
                         spawned_instances,
                         spawned_is_top,
                         inv.second().get_live_children(),
                         inv.second().live_children_is_top()});
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
  // Precompute the Thread Uniqueness flag so the finalization never re-enters
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
  std::uint64_t width = 0;
  if (load->result() != nullptr) {
    width = this->_ctx.bundle->data_layout()
                .store_size_in_bytes(load->result()->type())
                .to< std::uint64_t >();
  }
  this->_accesses.push_back(
      AccessRecord{load,
                   call_context,
                   AccessKind::Read,
                   std::move(pts),
                   this->snapshot_locks(inv, branch_guard_of(load, this->_ctx)),
                   this->snapshot_read_locks(inv),
                   off,
                   width,
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
                   inv.second().get_live_children(),
                   inv.second().live_children_is_top(),
                   load->is_atomic(),
                   provenance,
                   this->resolve_offset_congruence(load->operand(), inv),
                   this->_argument_windows
                       ? this->_argument_windows->window(load->operand(), inv,
                           this->current_thread_id(load, call_context))
                       : boost::none});
}

void DataRaceChecker::check_store(ar::Store* store,
                                  const value::AbstractDomain& inv,
                                  CallContext* call_context) {
  PointsToSet pts = this->resolve_points_to(store->pointer(), inv);
  if (!this->touches_shared_memory(pts)) {
    return;
  }
  std::string tid = this->current_thread_id(store, call_context);
  // Precompute the Thread Uniqueness flag so the finalization never re-enters
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
  // flat-array slot, record `node -> slot region` so finalization can fill
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
  std::uint64_t width = this->_ctx.bundle->data_layout()
                            .store_size_in_bytes(store->value()->type())
                            .to< std::uint64_t >();
  this->_accesses.push_back(
      AccessRecord{store,
                   call_context,
                   AccessKind::Write,
                   std::move(pts),
                   this->snapshot_locks(inv, branch_guard_of(store, this->_ctx)),
                   this->snapshot_read_locks(inv),
                   off,
                   width,
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
                   inv.second().get_live_children(),
                   inv.second().live_children_is_top(),
                   store->is_atomic(),
                   provenance,
                   this->resolve_offset_congruence(store->pointer(), inv),
                   this->_argument_windows
                       ? this->_argument_windows->window(store->pointer(), inv,
                           this->current_thread_id(store, call_context))
                       : boost::none});
}

} // end namespace analyzer
} // end namespace ikos
