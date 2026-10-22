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
#include <ikos/analyzer/analysis/bmc/bmc.hpp>
#include <ikos/analyzer/analysis/pair_exclusion.hpp>
#include <ikos/analyzer/analysis/region_ownership.hpp>
#include <ikos/analyzer/analysis/execution_engine/symbolic_index.hpp>
#include <ikos/analyzer/support/cast.hpp>
#include <ikos/analyzer/util/log.hpp>
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
// Keep every byte coefficient and constant; indices from separate threads
// never need to be equal, only each access and its own mutex must correspond.
struct AffinePointer {
  ar::Value* root = nullptr;
  core::ZNumber displacement{0};
  std::unordered_map< ar::InternalVariable*, core::ZNumber > coefficients;
};

bool acyclic_definition(ar::Statement* statement) {
  if (statement == nullptr || statement->parent() == nullptr) return false;
  auto* block = statement->parent();
  std::vector< ar::BasicBlock* > pending(block->successor_begin(), block->successor_end());
  std::unordered_set< ar::BasicBlock* > seen;
  while (!pending.empty()) {
    auto* next = pending.back();
    pending.pop_back();
    if (next == block || seen.size() >= 256) return false;
    if (!seen.insert(next).second) continue;
    pending.insert(pending.end(), next->successor_begin(), next->successor_end());
  }
  return true;
}

// Repeated extensions of one frozen value are equal. Keep the operation and
// result type: sign extension, zero extension and truncation are not synonyms.
ar::UnaryOperation* fixed_index_extension(ar::InternalVariable* variable) {
  auto* cast = dyn_cast_or_null< ar::UnaryOperation >(unique_def(variable));
  if (cast == nullptr ||
      (cast->op() != ar::UnaryOperation::SExt && cast->op() != ar::UnaryOperation::ZExt) ||
      !cast->operand()->type()->is_integer() || !variable->type()->is_integer())
    return nullptr;
  auto* input = dyn_cast< ar::InternalVariable >(cast->operand());
  if (input == nullptr || !acyclic_definition(unique_def(input)) ||
      !acyclic_definition(cast)) return nullptr;
  return cast;
}

bool equal_pointer_indices(const AffinePointer& left, const AffinePointer& right) {
  if (left.coefficients == right.coefficients) return true;
  if (left.coefficients.size() != right.coefficients.size()) return false;
  std::unordered_set< ar::InternalVariable* > matched;
  for (const auto& x : left.coefficients) {
    bool found = false;
    auto* xc = fixed_index_extension(x.first);
    for (const auto& y : right.coefficients) {
      if (matched.count(y.first) || x.second != y.second) continue;
      bool equal = x.first == y.first;
      if (!equal && xc != nullptr) {
        auto* yc = fixed_index_extension(y.first);
        equal = yc != nullptr && xc->op() == yc->op() &&
                xc->operand() == yc->operand() && x.first->type() == y.first->type();
      }
      if (equal) { matched.insert(y.first); found = true; break; }
    }
    if (!found) return false;
  }
  return true;
}

bool affine_pointer(ar::Value* pointer, AffinePointer& out) {
  for (unsigned depth = 0; depth < 32; ++depth) {
    if (!pointer->type()->is_pointer() || isa< ar::UndefinedConstant >(pointer) ||
        isa< ar::NullConstant >(pointer)) return false;
    auto* variable = dyn_cast< ar::InternalVariable >(pointer);
    if (variable == nullptr) { out.root = pointer; break; }
    auto* definition = unique_def(variable);
    if (auto* shift = dyn_cast_or_null< ar::PointerShift >(definition)) {
      for (auto i = shift->term_begin(); i != shift->term_end(); ++i) {
        auto term = *i;
        if (auto* constant = dyn_cast< ar::IntegerConstant >(term.second)) {
          out.displacement += term.first.to_z_number() * constant->value().to_z_number();
        } else if (auto* index = dyn_cast< ar::InternalVariable >(term.second)) {
          if (!index->type()->is_integer()) return false;
          out.coefficients[index] += term.first.to_z_number();
        } else return false;
      }
      pointer = shift->pointer();
    } else if (auto* assignment = dyn_cast_or_null< ar::Assignment >(definition)) {
      pointer = assignment->operand();
    } else if (auto* cast = dyn_cast_or_null< ar::UnaryOperation >(definition)) {
      if (cast->op() != ar::UnaryOperation::Bitcast) return false;
      pointer = cast->operand();
    } else { out.root = pointer; break; }
  }
  if (out.root == nullptr) return false;
  for (auto i = out.coefficients.begin(); i != out.coefficients.end();) {
    if (i->second == 0) i = out.coefficients.erase(i);
    else ++i;
  }
  return true;
}

struct PointerCongruence {
  ar::GlobalVariable* global = nullptr;
  core::ZNumber period;
  core::ZNumber residue{0};
};

// Resolve only the address congruence through exact actual/formal bindings.
// Each unknown term independently weakens the modulus, so no equality between
// caller/callee index values or distinct dynamic invocations is assumed.
boost::optional< PointerCongruence > pointer_congruence(
    ar::Value* pointer, CallContext* context, std::uint64_t pointer_bits) {
  PointerCongruence out{nullptr, core::ZNumber(1) << pointer_bits, core::ZNumber(0)};
  for (unsigned depth = 0; depth < 64; ++depth) {
    if (!pointer->type()->is_pointer()) return boost::none;
    if (auto* global = dyn_cast< ar::GlobalVariable >(pointer)) {
      if (global->is_thread_local()) return boost::none;
      out.global = global;
      return out;
    }
    auto* variable = dyn_cast< ar::InternalVariable >(pointer);
    if (variable == nullptr) return boost::none;
    auto* definition = unique_def(variable);
    if (auto* shift = dyn_cast_or_null< ar::PointerShift >(definition)) {
      for (auto i = shift->term_begin(); i != shift->term_end(); ++i) {
        auto term = *i;
        if (auto* constant = dyn_cast< ar::IntegerConstant >(term.second)) {
          out.residue += term.first.to_z_number() * constant->value().to_z_number();
        } else if (isa< ar::InternalVariable >(term.second) &&
                   term.second->type()->is_integer()) {
          out.period = core::gcd(out.period, term.first.to_z_number());
        } else return boost::none;
      }
      pointer = shift->pointer();
    } else if (auto* assignment = dyn_cast_or_null< ar::Assignment >(definition)) {
      pointer = assignment->operand();
    } else if (auto* cast = dyn_cast_or_null< ar::UnaryOperation >(definition)) {
      if (cast->op() != ar::UnaryOperation::Bitcast) return boost::none;
      pointer = cast->operand();
    } else {
      auto* function = variable->code()->function_or_null();
      if (definition != nullptr || function == nullptr || context == nullptr ||
          context->empty()) return boost::none;
      auto* call = context->call();
      auto* target = dyn_cast< ar::FunctionPointerConstant >(call->called());
      if (!isa< ar::Call >(call) || target == nullptr ||
          target->function() != function) return boost::none;
      // unique_def also returns null for multiple assignments, which are not
      // an immutable formal parameter binding.
      for (auto* block : *variable->code()) for (auto* statement : *block)
        if (statement->result_or_null() == variable) return boost::none;
      std::size_t i = 0;
      while (i < function->num_parameters() && function->param(i) != variable) ++i;
      if (i == function->num_parameters() || i >= call->num_arguments()) return boost::none;
      pointer = call->argument(i);
      context = context->parent();
    }
  }
  return boost::none;
}

struct RelativeMutex {
  core::ZNumber delta;
  ar::GlobalVariable* global;
  core::ZNumber period;
  core::ZNumber residue;
};

// Prove every CFG route to this access retains a mutex at access+delta.
// Rebinding, any intervening call (including unlock), and an uncovered cycle
// invalidate the certificate. No equality between different thread locals is used.
boost::optional< RelativeMutex > relative_mutex(const AccessRecord& access,
                                               std::uint64_t pointer_bits) {
  ar::Value* pointer = nullptr;
  if (auto* load = dyn_cast< ar::Load >(access.stmt)) pointer = load->operand();
  if (auto* store = dyn_cast< ar::Store >(access.stmt)) pointer = store->pointer();
  if (pointer == nullptr) return boost::none;
  AffinePointer data;
  if (!affine_pointer(pointer, data)) return boost::none;
  unsigned budget = 256;
  std::function< boost::optional< core::ZNumber >(
      const AffinePointer&, ar::BasicBlock*, ar::Statement*, CallContext*, unsigned) > prove;
  prove = [&](const AffinePointer& data, ar::BasicBlock* start,
              ar::Statement* initial_stop, CallContext* context, unsigned depth)
      -> boost::optional< core::ZNumber > {
    if (start == nullptr || depth >= 16) return boost::none;
    auto* function = start->code()->function_or_null();
    if (function == nullptr || (context != nullptr && context->contains(function)))
      return boost::none;
    std::unordered_set< ar::BasicBlock* > active;
    std::unordered_map< ar::BasicBlock*, boost::optional< core::ZNumber > > cache;
    std::function< boost::optional< core::ZNumber >(ar::BasicBlock*, ar::Statement*) > visit;
    visit = [&](ar::BasicBlock* block, ar::Statement* stop)
        -> boost::optional< core::ZNumber > {
      if (block == nullptr || budget == 0 || !active.insert(block).second) return boost::none;
      if (stop == nullptr) {
        auto old = cache.find(block);
        if (old != cache.end()) { active.erase(block); return old->second; }
      }
      auto scan = [&]() -> boost::optional< core::ZNumber > {
        auto end = stop ? std::find(block->begin(), block->end(), stop) : block->end();
        if (stop && end == block->end()) return boost::none;
        while (end != block->begin()) {
          if (budget == 0) return boost::none;
          --budget;
          auto* statement = *--end;
          auto* result = statement->result_or_null();
          auto* index = dyn_cast_or_null< ar::InternalVariable >(result);
          if (result == data.root || (index != nullptr && data.coefficients.count(index) &&
                                     fixed_index_extension(index) == nullptr))
            return boost::none;
          if (auto* call = dyn_cast< ar::CallBase >(statement)) {
            auto* target = dyn_cast< ar::FunctionPointerConstant >(call->called());
            if (!isa< ar::Call >(call) || target == nullptr ||
                !target->function()->is_declaration() ||
                target->function()->intrinsic_id() != ar::Intrinsic::PthreadMutexLock ||
                call->num_arguments() != 1) return boost::none;
            AffinePointer mutex;
            if (!affine_pointer(call->argument(0), mutex) || mutex.root != data.root ||
                !equal_pointer_indices(mutex, data)) return boost::none;
            return mutex.displacement - data.displacement;
          }
        }
        if (block->num_predecessors() == 0) {
          // Enter the exact ordinary caller only after checking the callee's
          // whole prefix. This never carries a parent's lock into a new thread.
          if (block != function->body()->entry_block() || context == nullptr ||
              context->empty() || !data.coefficients.empty()) return boost::none;
          auto* call = context->call();
          auto* target = dyn_cast< ar::FunctionPointerConstant >(call->called());
          if (!isa< ar::Call >(call) || target == nullptr || target->function() != function)
            return boost::none;
          auto* formal = dyn_cast< ar::InternalVariable >(data.root);
          if (formal == nullptr || formal->code() != function->body()) return boost::none;
          // A rebound parameter is not its caller's incoming actual value.
          for (auto* bb : *function->body()) for (auto* statement : *bb)
            if (statement->result_or_null() == formal) return boost::none;
          std::size_t i = 0;
          while (i < function->num_parameters() && function->param(i) != formal) ++i;
          if (i == function->num_parameters() || i >= call->num_arguments()) return boost::none;
          AffinePointer caller_data;
          if (!affine_pointer(call->argument(i), caller_data)) return boost::none;
          caller_data.displacement += data.displacement;
          return prove(caller_data, call->parent(), call, context->parent(), depth + 1);
        }
        boost::optional< core::ZNumber > delta;
        for (auto p = block->predecessor_begin(); p != block->predecessor_end(); ++p) {
          auto before = visit(*p, nullptr);
          if (!before || (delta && *delta != *before)) return boost::none;
          delta = before;
        }
        return delta;
      };
      auto result = scan();
      active.erase(block);
      if (stop == nullptr) cache.emplace(block, result);
      return result;
    };
    return visit(start, initial_stop);
  };
  auto delta = prove(data, access.stmt->parent(), access.stmt, access.call_context, 0);
  if (!delta) return boost::none;
  // PointerShift arithmetic is modulo the pointer width. Including 2^bits
  // preserves congruence even when byte coefficient arithmetic wraps.
  core::ZNumber period = core::ZNumber(1) << pointer_bits;
  for (const auto& term : data.coefficients) period = core::gcd(period, term.second);
  auto* global = dyn_cast< ar::GlobalVariable >(data.root);
  auto residue = data.displacement;
  if (global == nullptr) {
    if (auto origin = pointer_congruence(pointer, access.call_context, pointer_bits)) {
      global = origin->global;
      period = origin->period;
      residue = origin->residue;
    }
  }
  return RelativeMutex{*delta, global, period, residue};
}

} // namespace

std::vector< DataRaceChecker::PendingRacePair > DataRaceChecker::collect_race_pairs(
    ReadonlyHeapReads& readonly_heap_reads,
    BucketListOwnership& bucket_list_ownership,
    JoinedNames& joined_names) {
  // Offline pairwise race detection across all logged accesses.
  //
  // Two accesses A and B form a potential race when:
  //   1. A and B may be distinct concurrent dynamic instances
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
  //     so this finalization never re-enters the global blackboard.
  //   - If A has joined every instance represented by B.thread_id, then
  //     B terminated before A's continuation: HB(B, A).
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


  std::vector< PendingRacePair > all_pairs;
  unsigned exclusion_queries = 0, exclusion_native = 0, exclusion_smt = 0;
  unsigned exclusion_sat = 0, exclusion_unknown = 0, exclusion_invalid = 0;
  unsigned exclusion_eligible = 0, exclusion_budget = 0, exclusion_records = 0;
  std::uint64_t exclusion_us = 0;
  std::ofstream exclusion_report;
  if (this->_pair_exclusion_enabled) {
    if (const char* path = std::getenv("IKOS_AI_PAIR_EXCLUSION_REPORT")) {
      exclusion_report.open(path, std::ios::app);
      if (!exclusion_report) log::warning("Cannot open AI pair-exclusion evidence file");
    }
  }
  auto record_exclusion = [&](std::size_t i, std::size_t j,
                               const char* engine,
                               const pair_exclusion::Result& result) {
    if (!exclusion_report.is_open() || !exclusion_report || exclusion_records++ >= 64) return;
    auto fact = [&](std::size_t index) {
      const auto& a = this->_accesses[index];
      JsonDict side;
      side.put("access_id", index);
      side.put("thread", a.thread_id);
      side.put("width", std::to_string(a.width));
      side.put("offset_bits", a.offset.bit_width());
      side.put("offset_sign", a.offset.sign() == core::Signed ? "signed" : "unsigned");
      side.put("bottom", a.offset.is_bottom() || a.offset_congruence->is_bottom());
      if (!a.offset.is_bottom()) {
        side.put("lo", a.offset.lb().to_z_number().str());
        side.put("hi", a.offset.ub().to_z_number().str());
      }
      if (!a.offset_congruence->is_bottom()) {
        side.put("modulus", a.offset_congruence->modulus().str());
        side.put("residue", a.offset_congruence->residue().str());
      }
      if (auto location = source_location(a.stmt)) {
        side.put("file", location.path().string());
        side.put("line", static_cast< int >(location.line()));
        side.put("column", static_cast< int >(location.column()));
      }
      return side;
    };
    const char* status = "UNKNOWN";
    switch (result.status) {
      case pair_exclusion::Status::Unsat: status = "UNSAT"; break;
      case pair_exclusion::Status::Sat: status = "SAT"; break;
      case pair_exclusion::Status::Unknown: break;
      case pair_exclusion::Status::InvalidFacts: status = "INVALID_FACTS"; break;
    }
    JsonDict row;
    row.put("contract", "stable-ai-offset-byte-overlap-v1");
    row.put("engine", engine);
    row.put("status", status);
    row.put("pointer_bits", this->_ctx.bundle->data_layout().pointers.bit_width);
    std::ostringstream object;
    (*this->_accesses[i].pts.begin())->dump(object);
    row.put("object", object.str());
    row.put("a", fact(i));
    row.put("b", fact(j));
    row.put("reason", result.reason);
    row.put("elapsed_us", result.elapsed_us);
    if (!result.smtlib.empty()) row.put("smtlib", result.smtlib);
    if (result.model) {
      JsonDict model;
      model.put("start_a", result.model->start_a.str());
      model.put("start_b", result.model->start_b.str());
      model.put("byte_a", result.model->byte_a.str());
      model.put("byte_b", result.model->byte_b.str());
      model.put("address", result.model->address.str());
      row.put("abstract_model", model);
    }
    exclusion_report << row.str() << '\n';
  };
  std::unordered_map< ar::Statement*,
      std::unordered_map< CallContext*, boost::optional< RelativeMutex > > > relative_mutexes;
  auto mutex_delta = [&](const AccessRecord& access) {
    auto& contexts = relative_mutexes[access.stmt];
    auto old = contexts.find(access.call_context);
    if (old == contexts.end())
      old = contexts.emplace(access.call_context, relative_mutex(
          access, this->_ctx.bundle->data_layout().pointers.bit_width)).first;
    return old->second;
  };

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
  joined_names.resize(this->_accesses.size());
  std::vector< std::unordered_set< std::string > > inactive_names(this->_accesses.size());
  ConstantThreadCompletion completion(this->_ctx);
  // Wrapper-resolved start routines are absent from this syntactic creator
  // map. Require coverage of every registered site before using its uniqueness.
  std::unordered_set< std::uint64_t > covered_create_sites;
  for (const auto& creation : this->_thread_creations) {
    if (auto* call = dyn_cast< ar::CallBase >(creation.stmt))
      covered_create_sites.insert(this->_ctx.concurrent_env.site_id(call));
  }
  std::unordered_set< std::string > single_instance_threads{"main"};
  {
    auto thread_funcs = this->_ctx.concurrent_env.get_all_thread_functions();
    for (ar::Function* f : thread_funcs) {
      if (f != nullptr &&
          this->_ctx.concurrent_env.thread_instances_are_static(f->name()) &&
          this->_ctx.concurrent_env.get_func_instances(f->name()).size() == 1) {
        single_instance_threads.insert(f->name());
      }
    }
    for (std::size_t k = 0; k < this->_accesses.size(); ++k) {
      const auto& joined = this->_accesses[k].joined_threads;
      inactive_names[k] = completion.inactive_before(this->_accesses[k]);
      for (ar::Function* f : thread_funcs) {
        if (f == nullptr) {
          continue;
        }
        if (!this->_ctx.concurrent_env.thread_instances_are_static(f->name())) continue;
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
        const auto& access = this->_accesses[k];
        bool inactive = is_creator && cit->second.size() == 1 &&
                        single_instance_threads.count(access.thread_id) &&
                        !access.live_children_is_top;
        for (auto sid : instances)
          inactive &= covered_create_sites.count(sid) &&
                      !access.live_children.count(sid);
        if (inactive) inactive_names[k].insert(f->name());
        bool all = true;
        for (std::uint64_t sid : instances) {
          // Another creator's instance is absent from this thread's spawn
          // history even while live. Only actual completion proves a join;
          // before-create and inactive-child proofs have separate contracts.
          if (joined.count(sid) == 0) {
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

  // Legacy private_init annotations do not prove freshness or publication;
  // their exclusion rule remains disabled. InitPublication proves its own HB.
  InitPublication init_publication(this->_ctx, this->_accesses);
  FirstPublication first_publication(this->_ctx, this->_accesses);
  ThreadPrivateHeap private_heap(this->_ctx, this->_accesses);
  BinaryTokenOwnership binary_token(this->_ctx, this->_accesses);
  ArenaReservation arena_reservation(this->_ctx, this->_accesses, binary_token);
  ArraySlices array_slices(this->_ctx);
  ThreadLifecycle thread_lifecycle(this->_ctx);
  OnceInitialization once_initialization(this->_ctx, this->_accesses, binary_token);
  MutexHandover mutex_handover(this->_ctx);
  GuardedPublication guarded_publication(this->_ctx);
  ScProtocol sc_protocol(this->_ctx);
  SelfPublication self_publication(this->_ctx);
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

  auto evaluate_pair = [&](std::size_t i, std::size_t j) -> PairDecision {
    const AccessRecord& a = this->_accesses[i];
    const AccessRecord& b = this->_accesses[j];
      // Same statement: skip ONLY when both sides belong to the same
      // UNIQUE thread (single instance — one execution cannot race
      // itself). For non-unique threads the duplicates materialized
      // above represent distinct concurrent instances — do NOT skip.
      if (a.stmt == b.stmt) {
        if (a.thread_id == b.thread_id &&
            (a.is_unique_thread || b.is_unique_thread)) {
          return PairDecision::exclude(race_rules::Id::SameUniqueStatement);
        }
      }
      if (a.kind != AccessKind::Write && b.kind != AccessKind::Write) {
        return PairDecision::exclude(race_rules::Id::ReadRead);
      }

      // Happens-before: same thread AND that thread is unique ⇒ sequential.
      if (!a.thread_id.empty() && a.thread_id == b.thread_id &&
          a.is_unique_thread) {
        return PairDecision::exclude(race_rules::Id::SameUniqueThread);
      }
      if (this->_ctx.concurrent_env.same_closed_once(a.thread_id, b.thread_id)) {
        return PairDecision::exclude(race_rules::Id::SameClosedOnce);
      }
      // No direct child can be live: this is before create OR after join,
      // never a directional/transitive HB edge or a claim about grandchildren.
      if (inactive_names[i].count(b.thread_id) ||
          inactive_names[j].count(a.thread_id)) return PairDecision::exclude(race_rules::Id::InactiveChild);
      // Happens-before: A fully joined B (ALL instances of B's function) ⇒
      // B terminated before A's continuation. Instance-level: joining tids[0]
      // alone does NOT suppress tids[1]'s instance (28/29/30-join-array FN).
      if (!a.thread_id.empty() && !b.thread_id.empty() &&
          joined_names[i].count(b.thread_id)) {
        return PairDecision::exclude(race_rules::Id::JoinedRight);
      }
      // Happens-before: B fully joined A ⇒ A terminated before B's
      // continuation.
      if (!a.thread_id.empty() && !b.thread_id.empty() &&
          joined_names[j].count(a.thread_id)) {
        return PairDecision::exclude(race_rules::Id::JoinedLeft);
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
          // Even a terminated instance of x may have left y running.
          if (!single_instance_threads.count(x.thread_id)) {
            return false;
          }
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
          return PairDecision::exclude(race_rules::Id::CreateBefore);
        }
      }

      // Points-to intersection: ignore bottom (unreachable); TOP is the
      // identity of meet, so a TOP access conservatively pairs with every
      // other access (sound over-approximation, NOT masked to "nothing").
      if (a.pts.is_bottom() || b.pts.is_bottom()) {
        return PairDecision::exclude(race_rules::Id::UnreachablePoints);
      }
      if (a.pts.meet(b.pts).is_empty()) {
        return PairDecision::exclude(race_rules::Id::DisjointPoints);
      }
      // C11 DRF: two ATOMIC accesses never race (atomicity is itself
      // synchronization). Exempt the pair regardless of locks/HB — this is a
      // direct corollary of the C11 data-race definition, so it only removes
      // false positives, never a real race.
      if (a.is_atomic && b.is_atomic) {
        return PairDecision::exclude(race_rules::Id::AtomicAtomic);
      }
      if (init_publication.ordered(a, b) || init_publication.ordered(b, a)) {
        return PairDecision::exclude(race_rules::Id::InitPublication);
      }
      if (private_heap.excludes(a, b)) {
        JsonDict proof{{"safe_reason", "unexported live worker allocations"},
                       {"private_heap_contract", private_heap.contract(a)}};
        if (auto location = source_location(a.stmt)) proof.put("access_a_line", location.line());
        if (auto location = source_location(b.stmt)) proof.put("access_b_line", location.line());
        return PairDecision::exclude(race_rules::Id::PrivateHeap, proof);
      }
      if (binary_token.excludes(a, b)) {
        auto proof = binary_token.info(a, b);
        if (auto location = source_location(a.stmt)) proof.put("access_a_line", location.line());
        if (auto location = source_location(b.stmt)) proof.put("access_b_line", location.line());
        return PairDecision::exclude(race_rules::Id::BinaryToken, proof);
      }
      if (arena_reservation.excludes(a, b)) {
        auto proof = arena_reservation.info(a);
        if (auto location = source_location(a.stmt)) proof.put("access_a_line", location.line());
        if (auto location = source_location(b.stmt)) proof.put("access_b_line", location.line());
        return PairDecision::exclude(race_rules::Id::ArenaReservation, proof);
      }
      if (once_initialization.ordered(a, b)) {
        auto proof = once_initialization.info(a, b);
        if (auto location = source_location(a.stmt)) proof.put("access_a_line", location.line());
        if (auto location = source_location(b.stmt)) proof.put("access_b_line", location.line());
        return PairDecision::exclude(race_rules::Id::OnceInitialization, proof);
      }
      if (mutex_handover.ordered(a, b)) {
        auto proof = mutex_handover.info(a, b);
        if (auto location = source_location(a.stmt)) proof.put("access_a_line", location.line());
        if (auto location = source_location(b.stmt)) proof.put("access_b_line", location.line());
        return PairDecision::exclude(race_rules::Id::MutexHandover, proof);
      }
      if (thread_lifecycle.excludes(a, b)) {
        return PairDecision::exclude(race_rules::Id::ThreadLifecycle, thread_lifecycle.info(a, b));
      }
      if (guarded_publication.ordered(a, b)) {
        auto proof = guarded_publication.info(a, b);
        if (auto location = source_location(a.stmt)) proof.put("access_a_line", location.line());
        if (auto location = source_location(b.stmt)) proof.put("access_b_line", location.line());
        return PairDecision::exclude(race_rules::Id::GuardedPublication, proof);
      }
      if (sc_protocol.ordered(a, b)) {
        auto proof = sc_protocol.info(a, b);
        if (auto location = source_location(a.stmt)) proof.put("access_a_line", location.line());
        if (auto location = source_location(b.stmt)) proof.put("access_b_line", location.line());
        return PairDecision::exclude(race_rules::Id::ScProtocol, proof);
      }
      if (self_publication.ordered(a, b)) {
        auto proof = self_publication.info(a, b);
        if (auto location = source_location(a.stmt)) proof.put("access_a_line", location.line());
        if (auto location = source_location(b.stmt)) proof.put("access_b_line", location.line());
        return PairDecision::exclude(race_rules::Id::SelfPublication, proof);
      }
      if (first_publication.ordered(a, b)) {
        auto proof = first_publication.info();
        proof.put("safe_reason", "initialization before first mutex publication");
        if (auto location = source_location(a.stmt)) proof.put("access_a_line", location.line());
        if (auto location = source_location(b.stmt)) proof.put("access_b_line", location.line());
        return PairDecision::exclude(race_rules::Id::FirstPublication, proof);
      }
      // Happens-before (private-init publication, 09-regions_02-list_nr.c):
      // DISABLED (kEnablePrivateInit = false). The rule only proves "the same
      // thread later accesses this DynAlloc while holding exactly one lock" —
      // it does NOT prove the object was still un-published at the unlocked
      // write. published-before-init.c (main publishes `p` through a global
      // BEFORE spawning threads, so thread A's unlocked `p->v=1` races thread
      // B's locked write) is a real race this rule wrongly suppressed → FN,
      // violating FN=0. Re-enable only with (a) an escape/freshness proof that
      // the object is not yet reachable by other threads at the write, and
      // (b) an explicit publication edge (init write → promoting unlock →
      // reader lock → reader access). See is_heap_node_fresh /
      // touches_shared_memory as candidate sources for (a).
      constexpr bool kEnablePrivateInit = false;
      if (kEnablePrivateInit) {
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
          return PairDecision::exclude(race_rules::Id::DisabledPrivateInit);
        }
      }
      // Field/element sensitivity on the DATA side, width-aware: `data.x` and
      // `data.y` (or `data[3]` and `data[4]`) collapse to the same base
      // location, but their byte RANGES are disjoint — provably distinct, no
      // race. A top/imprecise offset overlaps everything (conservative), and
      // an unknown width (0) is treated as "may overlap" (no skip). The width
      // extends each start interval to a byte range, so `x` (u32 at [0])
      // overlaps `((u8*)&x)[1]` (u8 at [1]) — overlapping-widths.c FN.
      // Upper-bound + width is exact (ZNumber, no machine-integer wrap).
      if (!a.offset.is_top() && !b.offset.is_top() && a.width != 0 &&
          b.width != 0) {
        ZNumber a_hi = a.offset.ub().to_z_number();
        ZNumber b_hi = b.offset.ub().to_z_number();
        if (a_hi + a.width <= b.offset.lb().to_z_number() ||
            b_hi + b.width <= a.offset.lb().to_z_number()) {
          return PairDecision::exclude(race_rules::Id::DisjointBytes);
        }
      }
      // Unknown base displacement cannot be replaced by a relative field
      // offset: equal pointee types do not establish a common object origin.

      // Lockset intersection: a shared EXCLUSIVE lock (or an exclusive
      // lock meeting a read lock) makes the two accesses mutually
      // exclusive => no race. A shared READ-READ lock does NOT exclude —
      // pthread_rwlock_rdlock permits concurrent readers, so a write
      // performed under rdlock races (04-mutex_55-pt_rwlock_rr.c). A
      // pure read-read data pair was already skipped above.
      //
      // Equal concrete MUST mutex keys identify the same actual lock;
      // mutual exclusion covers every memory object, independent of data
      // region. Symbolic keys still require a separate equality proof.
      auto& env = this->_ctx.concurrent_env;
      auto protects_both = [&](const std::vector< std::uint64_t >& x,
                               const std::vector< std::uint64_t >& y) {
        for (std::uint64_t lx : x) {
          std::uint64_t bx;
          core::ConcurrentGlobalEnv::SymbolicIndex sx;
          if (env.lock_symbolic_index(lx, bx, sx)) {
            // A per-access region match does not prove that both executions
            // hold the same mutex. Keep symbolic pairs until that is proved.
            continue;
          }
          for (std::uint64_t ly : y) {
            std::uint64_t by;
            core::ConcurrentGlobalEnv::SymbolicIndex sy;
            if (env.lock_symbolic_index(ly, by, sy)) {
              continue;
            }
            if (lx == ly) {
              return true;
            }
          }
        }
        return false;
      };
      if (protects_both(a.locks, b.locks) ||          // exclusive ∩ exclusive
          protects_both(a.locks, b.read_locks) ||     // exclusive ∩ read
          protects_both(a.read_locks, b.locks)) {     // read ∩ exclusive
        return PairDecision::exclude(race_rules::Id::CommonMutex);
      }

      // This proof only covers live heap objects, including every disjunct on
      // both sides. Absolute-address/TOP mixtures retain their old candidates.
      auto heap_only = [](const PointsToSet& points) {
        if (!points.is_set() || points.is_empty()) return false;
        for (auto* object : points) {
          if (!isa< DynAllocMemoryLocation >(object)) return false;
        }
        return true;
      };
      // Actual heap overlap implies equal immutable owners, hence the same
      // corresponding concrete mutex. This is not a non-nullness assertion.
      auto owned_conflicts = [&]() {
        if (!heap_only(a.pts) || !heap_only(b.pts)) return false;
        const auto common = a.pts.meet(b.pts);
        if (common.is_empty()) return false;
        for (auto* object : common)
          if (!this->_ownership->excludes(a.stmt, a.call_context,
                                          b.stmt, b.call_context, object))
            return false;
        return true;
      };
      if (owned_conflicts()) {
        auto proof = this->_ownership->info();
        proof.put("safe_reason", "indexed ownership with corresponding mutexes");
        if (auto location = source_location(a.stmt)) {
          proof.put("access_a_line", location.line());
        }
        if (auto location = source_location(b.stmt)) {
          proof.put("access_b_line", location.line());
        }
        return PairDecision::exclude(race_rules::Id::RegionOwnership, proof);
      }

      if (bucket_list_ownership.excludes(a, b)) {
        auto proof = bucket_list_ownership.info();
        proof.put("safe_reason", "complete graph facts exclude conflicting byte accesses");
        return PairDecision::exclude(race_rules::Id::BucketOwnership, proof);
      }

      if (readonly_heap_reads.excludes(a, b)) {
        auto proof = readonly_heap_reads.info();
        proof.put("safe_reason", "immutable heap read is disjoint from direct scalar global storage");
        return PairDecision::exclude(race_rules::Id::ReadonlyHeap, proof);
      }

      if (array_slices.excludes(a, b)) {
        auto proof = array_slices.info();
        proof.put("safe_reason",
                  "disjoint inductive byte ranges of one stable array base");
        return PairDecision::exclude(race_rules::Id::ArraySlices, proof);
      }

      // Corresponding mutexes exclude conflicts only if overlap implies
      // equal starts. Equal exact offsets suffice; matching byte congruences
      // also suffice when both widths fit in the common modulus.
      if (a.pts.is_set() && b.pts.is_set() && a.width != 0 && b.width != 0) {
        auto common = a.pts.meet(b.pts);
        bool concrete = !common.is_empty();
        for (auto* location : common) {
          auto* global = dyn_cast< GlobalMemoryLocation >(location);
          if (global == nullptr || global->global_var()->is_thread_local()) {
            concrete = false;
            break;
          }
        }
        if (concrete) {
          auto left = mutex_delta(a), right = mutex_delta(b);
          if (left && right && left->delta == right->delta) {
            auto a_start = a.offset.singleton(), b_start = b.offset.singleton();
            if (a_start && b_start && a_start->to_z_number() == b_start->to_z_number())
              return PairDecision::exclude(race_rules::Id::CorrespondingMutexExact);
            if (left->global != nullptr && left->global == right->global &&
                a.pts.size() == 1 && b.pts.size() == 1 && common.size() == 1 &&
                cast< GlobalMemoryLocation >(*common.begin())->global_var() == left->global) {
              auto period = core::gcd(left->period, right->period);
              // The modulus divides the address-space size: even wrapping
              // byte ranges cannot overlap at distinct congruent starts.
              if (core::ZNumber(a.width) <= period && core::ZNumber(b.width) <= period &&
                  ((left->residue - right->residue) % period) == 0) return PairDecision::exclude(race_rules::Id::CorrespondingMutexCongruence);
            }
          }
        }
      }

      // This query covers all offsets in the two independent stable AI facts;
      // it is not a bounded-path BMC query. Unknown object/width/fact keeps may.
      if (this->_pair_exclusion_enabled && a.offset_congruence && b.offset_congruence &&
          a.width != 0 && b.width != 0 && a.pts.is_set() && b.pts.is_set() &&
          a.pts.size() == 1 && b.pts.size() == 1 && *a.pts.begin() == *b.pts.begin()) {
        auto* object = dyn_cast< GlobalMemoryLocation >(*a.pts.begin());
        if (object != nullptr && !object->global_var()->is_thread_local()) {
          ++exclusion_eligible;
          pair_exclusion::AccessFacts left{a.offset, *a.offset_congruence, a.width};
          pair_exclusion::AccessFacts right{b.offset, *b.offset_congruence, b.width};
          auto bits = this->_ctx.bundle->data_layout().pointers.bit_width;
          if (pair_exclusion::native_excludes(left, right, bits)) {
            ++exclusion_native;
            pair_exclusion::Result proof;
            proof.status = pair_exclusion::Status::Unsat;
            proof.reason = "byte difference excluded by common modular residue";
            record_exclusion(i, j, "native-congruence", proof);
            return PairDecision::exclude(race_rules::Id::OffsetCongruence);
          }
          if (this->_pair_exclusion_solver) {
            // Per-checker research budget; exhausting it cannot remove a pair.
            if (exclusion_queries < 8 && exclusion_us < 200000) {
              ++exclusion_queries;
              auto result = pair_exclusion::check(left, right, bits, 20,
                                                  exclusion_report.is_open() && bool(exclusion_report));
              exclusion_us += result.elapsed_us;
              record_exclusion(i, j, "smt", result);
              if (result.status == pair_exclusion::Status::Unsat) {
                ++exclusion_smt;
                return PairDecision::exclude(race_rules::Id::OffsetSmt);
              }
              if (result.status == pair_exclusion::Status::Sat) ++exclusion_sat;
              else if (result.status == pair_exclusion::Status::InvalidFacts)
                ++exclusion_invalid;
              else ++exclusion_unknown;
            } else ++exclusion_budget;
          }
        }
      }

    return PairDecision{};
  };
  for (std::size_t i = 0; i < this->_accesses.size(); ++i) {
    const AccessRecord& a = this->_accesses[i];
    for (std::size_t j = i + 1; j < this->_accesses.size(); ++j) {
      const AccessRecord& b = this->_accesses[j];
      const auto decision = evaluate_pair(i, j);
      if (decision.excluded()) {
        this->consume_pair_proof(i, j, decision);
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

  if (const char* path = std::getenv("IKOS_RACE_RULE_REPORT")) {
    std::ofstream output(path, std::ios::app);
    if (!output) log::warning("Cannot open race rule report");
    JsonDict counts;
    for (std::size_t id = 1; id < this->_pair_rule_counts.size(); ++id) {
      if (this->_pair_rule_counts[id])
        counts.put(race_rules::contract(static_cast< race_rules::Id >(id)).id,
                   this->_pair_rule_counts[id]);
    }
    output << JsonDict{{"kind", "pair-rule-summary"}, {"counts", counts},
                       {"accesses", this->_accesses.size()},
                       {"remaining_pairs", all_pairs.size()}}.str() << '\n';
  }
  if (exclusion_report.is_open() && exclusion_report) {
    JsonDict summary;
    summary.put("contract", "stable-ai-offset-byte-overlap-v1");
    summary.put("kind", "summary");
    summary.put("eligible", exclusion_eligible);
    summary.put("native_excluded", exclusion_native);
    summary.put("solver_queries", exclusion_queries);
    summary.put("smt_excluded", exclusion_smt);
    summary.put("abstract_sat", exclusion_sat);
    summary.put("unknown", exclusion_unknown);
    summary.put("invalid_facts", exclusion_invalid);
    summary.put("budget_skipped", exclusion_budget);
    summary.put("query_elapsed_us", exclusion_us);
    summary.put("record_limit", 64);
    exclusion_report << summary.str() << '\n';
  }

  return all_pairs;
}

} // namespace analyzer
} // namespace ikos
