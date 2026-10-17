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
#include <ikos/analyzer/checker/init_publication.hpp>
#include <ikos/analyzer/checker/first_publication.hpp>
#include <ikos/analyzer/checker/mutex_handover.hpp>
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
#include <ikos/analyzer/analysis/execution_engine/pthread_key_effects.hpp>
#include <ikos/analyzer/analysis/execution_engine/stdio_effects.hpp>
#include <ikos/analyzer/analysis/runtime_stdio_frame.hpp>
#include <ikos/analyzer/analysis/bmc/bmc.hpp>
#include <ikos/analyzer/analysis/bmc/unroll.hpp>
#include <ikos/analyzer/analysis/pair_exclusion.hpp>
#include <ikos/analyzer/analysis/region_ownership.hpp>
#include <ikos/analyzer/analysis/execution_engine/symbolic_index.hpp>
#include <ikos/analyzer/analysis/pointer/function.hpp>
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
  this->_ownership.reset(new RegionOwnership(ctx));
  const char* configured = std::getenv("IKOS_AI_PAIR_EXCLUSION");
  const std::string mode = configured ? configured : "";
  this->_pair_exclusion_enabled = mode == "1" || mode == "native";
  this->_pair_exclusion_solver = mode == "1";
}

DataRaceChecker::~DataRaceChecker() {
  this->_ownership->run();
  ReadonlyHeapReads readonly_heap_reads(this->_ctx);
  BucketListOwnership bucket_list_ownership(this->_ctx);
  bool synthetic_handle_unsupported = false;
  for (ar::CallBase* call : this->_ctx.concurrent_env.unmodeled_thread_creations()) {
    this->_checks.insert(
        CheckKind::UnknownFunctionCallPointer, CheckerName::DataRace,
        Result::Warning, call, this->_ctx.call_context_factory->get_empty(),
        {call->argument(2)},
        {{"unknown_reason", "thread entry body not covered"}});
  }

  for (const auto& effect : this->_ctx.concurrent_env.unmodeled_effects()) {
    // The source-only SAFE proof does not relax BMC's representation gate.
    synthetic_handle_unsupported |=
        effect.second == "synthetic pthread handle representation not modeled";
    if (effect.second == "synthetic pthread handle representation not modeled" &&
        readonly_heap_reads.certifies(effect.first)) {
      auto proof = readonly_heap_reads.info();
      proof.put("safe_reason", "bounded immutable heap read cannot consume a pthread handle");
      this->_checks.insert(CheckKind::DataRace, CheckerName::DataRace, Result::Ok,
                           effect.first, this->_ctx.call_context_factory->get_empty(), {}, proof);
      continue;
    }
    if (bucket_list_ownership.certifies(effect.first, effect.second)) {
      auto proof = bucket_list_ownership.info();
      proof.put("safe_reason", "complete bucket graph source and exact protected byte effects");
      this->_checks.insert(CheckKind::DataRace, CheckerName::DataRace, Result::Ok,
                           effect.first, this->_ctx.call_context_factory->get_empty(), {}, proof);
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
    this->_checks.insert(
        CheckKind::DataRace, CheckerName::DataRace, Result::Warning,
        anchor, this->_ctx.call_context_factory->get_empty(), {}, info);
  }

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
  //     so this destructor never re-enters the global blackboard.
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
  std::vector< std::unordered_set< std::string > > joined_names(
      this->_accesses.size());
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
      const auto& spawned_insts = this->_accesses[k].spawned_instances;
      bool spawned_top = this->_accesses[k].spawned_is_top;
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
      // No direct child can be live: this is before create OR after join,
      // never a directional/transitive HB edge or a claim about grandchildren.
      if (inactive_names[i].count(b.thread_id) ||
          inactive_names[j].count(a.thread_id)) continue;
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
      if (init_publication.ordered(a, b) || init_publication.ordered(b, a)) {
        continue;
      }
      if (private_heap.excludes(a, b)) {
        JsonDict proof{{"safe_reason", "unexported live worker allocations"},
                       {"private_heap_contract", private_heap.contract(a)}};
        if (auto location = source_location(a.stmt)) proof.put("access_a_line", location.line());
        if (auto location = source_location(b.stmt)) proof.put("access_b_line", location.line());
        this->_checks.insert(CheckKind::DataRace, CheckerName::DataRace, Result::Ok,
                             a.stmt, a.call_context, {}, proof);
        continue;
      }
      if (binary_token.excludes(a, b)) {
        auto proof = binary_token.info(a, b);
        if (auto location = source_location(a.stmt)) proof.put("access_a_line", location.line());
        if (auto location = source_location(b.stmt)) proof.put("access_b_line", location.line());
        this->_checks.insert(CheckKind::DataRace, CheckerName::DataRace, Result::Ok,
                             a.stmt, a.call_context, {}, proof);
        continue;
      }
      if (arena_reservation.excludes(a, b)) {
        auto proof = arena_reservation.info(a);
        if (auto location = source_location(a.stmt)) proof.put("access_a_line", location.line());
        if (auto location = source_location(b.stmt)) proof.put("access_b_line", location.line());
        this->_checks.insert(CheckKind::DataRace, CheckerName::DataRace, Result::Ok,
                             a.stmt, a.call_context, {}, proof);
        continue;
      }
      if (once_initialization.ordered(a, b)) {
        auto proof = once_initialization.info(a, b);
        if (auto location = source_location(a.stmt)) proof.put("access_a_line", location.line());
        if (auto location = source_location(b.stmt)) proof.put("access_b_line", location.line());
        this->_checks.insert(CheckKind::DataRace, CheckerName::DataRace, Result::Ok,
                             a.stmt, a.call_context, {}, proof);
        continue;
      }
      if (mutex_handover.ordered(a, b)) {
        auto proof = mutex_handover.info(a, b);
        if (auto location = source_location(a.stmt)) proof.put("access_a_line", location.line());
        if (auto location = source_location(b.stmt)) proof.put("access_b_line", location.line());
        this->_checks.insert(CheckKind::DataRace, CheckerName::DataRace, Result::Ok,
                             a.stmt, a.call_context, {}, proof);
        continue;
      }
      if (thread_lifecycle.excludes(a, b)) {
        this->_checks.insert(CheckKind::DataRace, CheckerName::DataRace, Result::Ok,
                             a.stmt, a.call_context, {}, thread_lifecycle.info(a, b));
        continue;
      }
      if (sc_protocol.ordered(a, b)) {
        auto proof = sc_protocol.info(a, b);
        if (auto location = source_location(a.stmt)) proof.put("access_a_line", location.line());
        if (auto location = source_location(b.stmt)) proof.put("access_b_line", location.line());
        this->_checks.insert(CheckKind::DataRace, CheckerName::DataRace, Result::Ok,
                             a.stmt, a.call_context, {}, proof);
        continue;
      }
      if (self_publication.ordered(a, b)) {
        auto proof = self_publication.info(a, b);
        if (auto location = source_location(a.stmt)) proof.put("access_a_line", location.line());
        if (auto location = source_location(b.stmt)) proof.put("access_b_line", location.line());
        this->_checks.insert(CheckKind::DataRace, CheckerName::DataRace, Result::Ok,
                             a.stmt, a.call_context, {}, proof);
        continue;
      }
      if (first_publication.ordered(a, b)) {
        auto proof = first_publication.info();
        proof.put("safe_reason", "initialization before first mutex publication");
        if (auto location = source_location(a.stmt)) proof.put("access_a_line", location.line());
        if (auto location = source_location(b.stmt)) proof.put("access_b_line", location.line());
        this->_checks.insert(CheckKind::DataRace, CheckerName::DataRace, Result::Ok,
                             a.stmt, a.call_context, {}, proof);
        continue;
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
          continue;
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
          continue;
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
        continue;
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
        this->_checks.insert(CheckKind::DataRace, CheckerName::DataRace, Result::Ok,
                             a.stmt, a.call_context, {}, proof);
        continue;
      }

      if (bucket_list_ownership.excludes(a, b)) {
        auto proof = bucket_list_ownership.info();
        proof.put("safe_reason", "closed bucket membership with corresponding normal mutex");
        this->_checks.insert(CheckKind::DataRace, CheckerName::DataRace, Result::Ok,
                             a.stmt, a.call_context, {}, proof);
        continue;
      }

      if (readonly_heap_reads.excludes(a, b)) {
        auto proof = readonly_heap_reads.info();
        proof.put("safe_reason", "immutable heap read is disjoint from direct scalar global storage");
        this->_checks.insert(CheckKind::DataRace, CheckerName::DataRace, Result::Ok,
                             a.stmt, a.call_context, {}, proof);
        continue;
      }

      if (array_slices.excludes(a, b)) {
        auto proof = array_slices.info();
        proof.put("safe_reason",
                  "disjoint inductive byte ranges of one stable array base");
        this->_checks.insert(CheckKind::DataRace, CheckerName::DataRace, Result::Ok,
                             a.stmt, a.call_context, {}, proof);
        continue;
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
              continue;
            if (left->global != nullptr && left->global == right->global &&
                a.pts.size() == 1 && b.pts.size() == 1 && common.size() == 1 &&
                cast< GlobalMemoryLocation >(*common.begin())->global_var() == left->global) {
              auto period = core::gcd(left->period, right->period);
              // The modulus divides the address-space size: even wrapping
              // byte ranges cannot overlap at distinct congruent starts.
              if (core::ZNumber(a.width) <= period && core::ZNumber(b.width) <= period &&
                  ((left->residue - right->residue) % period) == 0) continue;
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
            continue;
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
                continue;
              }
              if (result.status == pair_exclusion::Status::Sat) ++exclusion_sat;
              else if (result.status == pair_exclusion::Status::InvalidFacts)
                ++exclusion_invalid;
              else ++exclusion_unknown;
            } else ++exclusion_budget;
          }
        }
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
  auto creation_info = [&](const std::vector< ar::Statement* >* selected) {
    JsonList list;
    auto append = [&](const ThreadCreation& tc) {
      JsonDict item;
      if (auto loc = source_location(tc.stmt)) {
        item.put("line", static_cast< int >(loc.line()));
        item.put("column", static_cast< int >(loc.column()));
        item.put("file", loc.path().filename().string());
      }
      item.put("thread_function", tc.child);
      ar::Code* code = tc.stmt->parent()->code();
      if (code != nullptr && code->is_function_body()) item.put("creator", code->function()->name());
      list.add(item);
    };
    if (selected == nullptr) {
      for (const auto& creation : this->_thread_creations) append(creation);
    } else {
      // Preserve execution order and dynamic repetitions, not source order.
      for (auto* stmt : *selected) {
        for (const auto& creation : this->_thread_creations) {
          if (creation.stmt == stmt) {append(creation); break;}
        }
      }
    }
    return list;
  };

  // Every concrete prefix must discharge its own synchronization semantics.

  const bool bmc_enabled = this->_ctx.opts.demote_race_to_unknown &&
                           getenv("IKOS_BMC_RECOVER") != nullptr;
  const bool diagnostics_enabled = getenv("IKOS_BMC_DIAGNOSTICS") != nullptr;
  bmc::SearchBudget search_budget(this->_ctx.opts.bmc_deadline_ms,
                                 this->_ctx.opts.bmc_solver_timeout_ms);
  // A may-multiplicity flag cannot establish two live concrete instances.
  // Initially support two explicit spawns on a linear main prefix, with no
  // intervening waits or unknown calls. Loop/conditional creation needs modeling.
  std::map< std::pair< std::string, std::string >, std::vector< std::vector< bmc::BmcEvent > > > activation_prefixes;
  decltype(activation_prefixes) short_activation_prefixes;
  auto two_live_threads = [&](const std::string& thread_a, const std::string& thread_b) {
    ar::Function* main = this->_ctx.bundle->function_or_null("main");
    if (main == nullptr || main->body_or_null() == nullptr ||
        !main->body_or_null()->has_entry_block()) {
      return false;
    }
    std::map< std::string, unsigned > required{{thread_a, 1}, {thread_b, 1}};
    if (thread_a == thread_b) {
      required[thread_a] = 2;
    }
    std::map< std::string, unsigned > sites;
    for (const ThreadCreation& tc : this->_thread_creations) {
      if (required.count(tc.child)) {
        if (tc.stmt->parent()->code() != main->body_or_null()) {
          return false;
        }
        ++sites[tc.child];
      }
    }
    if (sites != required) {
      return false;
    }
    std::set< ar::BasicBlock* > visited;
    ar::BasicBlock* bb = main->body_or_null()->entry_block();
    std::map< std::string, unsigned > spawns;
    std::vector< bmc::BmcEvent > execution;
    while (visited.insert(bb).second) {
      for (ar::Statement* stmt : *bb) {
        bmc::BmcEvent step(stmt, bmc::EvKind::Local);
        bmc::classify_event(stmt, step);
        execution.push_back(step);
        if (isa< ar::Comparison >(stmt) || isa< ar::Unreachable >(stmt)) {
          return false;
        }
        if (auto* store = dyn_cast< ar::Store >(stmt)) {
          bmc::BmcEvent event;
          bmc::classify_event(store, event);
          core::ZNumber size;
          if (event.var_id != 0 || event.width == 0 || !bmc::memory_extent(event, size)) return false;
          if (event.const_off < 0 || event.const_off + event.width > size) return false;
        }
        if (auto* call = dyn_cast< ar::CallBase >(stmt)) {
          bool creation = false;
          for (const ThreadCreation& tc : this->_thread_creations) {
            if (tc.stmt == call) {
              bmc::BmcEvent event;
              ar::Function* child = this->_ctx.bundle->function_or_null(tc.child);
              if (!bmc::classify_event(call, event) || event.kind != bmc::EvKind::Create ||
                  child == nullptr || event.thread != reinterpret_cast< std::uint64_t >(child)) return false;
              creation = true;
              if (required.count(tc.child)) {
                ++spawns[tc.child];
                if (spawns == required) {
                  if (!bmc::prefix_values_defined(execution)) return false;
                  activation_prefixes[std::minmax(thread_a, thread_b)] = {execution};
                  return true;
                }
              }
              break;
            }
          }
          if (!creation && !bmc::supported_leaf_call(call)) return false;
        } else if (!bmc::total_local_statement(stmt) && !isa< ar::Store >(stmt)) {
          return false;
        }
      }
      if (bb->num_successors() != 1) {
        return false;
      }
      bb = *bb->successor_begin();
    }
    return false;
  };
  // Reuse the same bounded AR path engine for conditional creation. Static
  // site counts are not existence evidence; the retained guards are later
  // checked together with both workers' value/scheduling constraints.
  auto conditional_live_threads = [&](const std::string& thread_a, const std::string& thread_b,
                                      unsigned loop_bound, bool extra_occurrences = false,
                                      ar::CallBase* finite_source = nullptr, unsigned finite_value = 0) {
    auto* main = this->_ctx.bundle->function_or_null("main");
    if (main == nullptr || !main->is_definition()) return false;
    std::map< std::string, unsigned > required{{thread_a, 1}, {thread_b, 1}};
    if (thread_a == thread_b) required[thread_a] = 2;
    std::map< ar::Function*, unsigned > create_limits;
    if (!extra_occurrences) {
      for (const auto& entry : required) {
        auto* function = this->_ctx.bundle->function_or_null(entry.first);
        if (function == nullptr) return false;
        create_limits.emplace(function, entry.second);
      }
    }
    auto& alternatives = activation_prefixes[std::minmax(thread_a, thread_b)];
    for (const auto& creation : this->_thread_creations) {
      if (search_budget.expired()) break;
      if (!required.count(creation.child)) continue;
      // An exact-count prefix cannot contain a later occurrence of this entry.
      const unsigned occurrences = extra_occurrences ? 2 : required.at(creation.child);
      for (unsigned occurrence = 1; occurrence <= occurrences; ++occurrence) {
        if (extra_occurrences && search_budget.expired()) break;
        // Activation below rejects every join. Stop unsupported join suffixes
        // during construction instead of enumerating paths we cannot retain.
        auto result = finite_source == nullptr
            ? bmc::unroll_to(main, creation.stmt, occurrence, loop_bound, true,
                             extra_occurrences ? nullptr : &create_limits)
            : bmc::unroll_to(main, creation.stmt, occurrence, loop_bound, true,
                             extra_occurrences ? nullptr : &create_limits,
                             &search_budget.deadline, finite_source, finite_value);
        // Exhaustion omits alternatives, not the complete prefixes already
        // emitted. Keep their target/guard/definedness/activation obligations.
        for (auto& path : result.paths) {
          if (bmc::candidate_index(path) == path.size()) continue;
          std::map< std::string, unsigned > spawns;
          std::set< std::pair< std::uint64_t, core::ZNumber > > held;
          bool supported = true;
          for (std::size_t step = 0; step < path.size(); ++step) {
            auto& event = path[step];
            if (event.kind == bmc::EvKind::Unsupported || event.kind == bmc::EvKind::Join) {supported = false; break;}
            if (event.kind == bmc::EvKind::Lock || event.kind == bmc::EvKind::Unlock) {
              bmc::BmcEvent address;
              if (event.mutex == bmc::PSEUDO_ATOMIC_LOCK) address.base = bmc::PSEUDO_ATOMIC_LOCK;
              else {
                auto* call = cast< ar::CallBase >(event.stmt);
                core::ZNumber size;
                if (call->num_arguments() != 1 ||
                    !bmc::pointer_on_path(call->argument(0), path, step, main, nullptr, address) ||
                    address.base == 0 || address.var_id != 0 || !bmc::memory_extent(address, size) ||
                    address.const_off < 0 || address.const_off >= size) {supported = false; break;}
              }
              auto identity = std::make_pair(address.base, address.const_off);
              // This is a main-only execution: all spawned children can remain
              // unscheduled until the checked, fully released startup ends.
              if (event.kind == bmc::EvKind::Lock ? !held.insert(identity).second : held.erase(identity) == 0) {
                supported = false; break;
              }
            }
            if (event.kind == bmc::EvKind::Create) {
              auto* child = reinterpret_cast< ar::Function* >(event.thread);
              if (required.count(child->name())) ++spawns[child->name()];
            }
            if (event.kind == bmc::EvKind::Read || event.kind == bmc::EvKind::Write) {
              auto* pointer = event.kind == bmc::EvKind::Read
                                  ? cast< ar::Load >(event.stmt)->operand()
                                  : cast< ar::Store >(event.stmt)->pointer();
              // Resolve each executed iteration, not the static index variable.
              event.base = 0; event.local_base = nullptr; event.const_off = 0;
              event.var_id = 0; event.var_before = 0; event.coeff = 0;
              core::ZNumber size;
              if (!bmc::pointer_on_path(pointer, path, step, main, nullptr, event) ||
                  event.var_id != 0 || event.width == 0 || !bmc::memory_extent(event, size) ||
                  event.const_off < 0 || event.const_off + event.width > size) {supported = false; break;}
            }
          }
          bool enough = spawns == required;
          if (extra_occurrences) {
            enough = true;
            for (const auto& entry : required) enough &= spawns[entry.first] >= entry.second;
          }
          if (supported && held.empty() && enough &&
              bmc::prefix_values_defined(path, nullptr, nullptr, nullptr, nullptr, extra_occurrences)) {
            alternatives.push_back(std::move(path));
            if (alternatives.size() == 8) return true;
          }
        }
      }
    }
    return !alternatives.empty();
  };
  std::map< std::pair< std::string, std::string >, bool > instance_proofs;
  std::set< std::pair< std::string, std::string > > small_activation;

  // Preserve the original query pass before probing interrupted enumerations.
  // Deferred groups emit once after their last retry, under the same deadline.
  std::set< std::string > deferred_groups;
  std::set< std::string > larger_activation_groups;
  std::set< std::string > occurrence_groups;
  std::set< std::string > hinted_activation_groups;
  std::set< std::string > completion_groups;
  std::map< std::string, bmc::Result > deferred_results;
  std::map< std::string, JsonList > deferred_candidates;
  std::map< std::string, JsonList > occurrence_candidates;
  std::map< std::string, std::map< std::string, std::size_t > > deferred_counts;
  bool file_confirmed = false;
  unsigned startup_hint = 16;
  for (unsigned pass = 0; pass < 6; ++pass) {
    // Try every candidate's short execution before deeper loop alternatives.
    search_budget.defer_large_loops = pass < 2;
    if (pass == 4 && !hinted_activation_groups.empty() && !file_confirmed &&
        !search_budget.expired() && search_budget.candidates != 0 && search_budget.queries != 0) {
      // Even the hint scan follows every original pass; it cannot consume
      // the budget of an already supported short activation.
      auto* main = this->_ctx.bundle->function_or_null("main");
      if (main != nullptr) startup_hint = bmc::literal_loop_search_bound(main);
    }
    if (pass >= 2) {
      // A short startup is a search choice, not an activation/safety proof for
      // other prefixes. Retry deferred groups with the original larger bound.
      instance_proofs.clear();
      // Bounded DFS need not emit a superset: keep short startup + deep worker
      // combinations when the larger startup enumeration saturates.
      if (pass == 2) short_activation_prefixes = std::move(activation_prefixes);
      activation_prefixes.clear();
      if (pass >= 3) small_activation.clear();
    }
    if (pass != 0) {
      search_budget.probe_interrupted = true;
      if (pass < 3) {
        search_budget.candidates = 256;
        search_budget.queries = 1024;
      }
    }
    for (auto& kv : groups) {
    if ((pass == 1 && deferred_groups.count(kv.first) == 0) ||
        (pass == 2 && larger_activation_groups.count(kv.first) == 0) ||
        (pass == 3 && occurrence_groups.count(kv.first) == 0) ||
        (pass == 4 && hinted_activation_groups.count(kv.first) == 0) ||
        (pass == 5 && completion_groups.count(kv.first) == 0)) continue;
    search_budget.saw_interrupted = false;
    const std::vector< std::size_t >& pair_indices = kv.second;
    if (pair_indices.empty()) {
      continue;
    }

    // Anchor on the FIRST pair (Write of A or B).
    const PendingRacePair& first = all_pairs[pair_indices.front()];
    const PendingRacePair* selected = &first;
    int confirmed_pair_id = -1;

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

    bool bmc_confirmed = false;
    bmc::Result group_result(bmc::Status::NotSearched);
    if (pass != 0) group_result.merge(deferred_results.at(kv.first));
    std::vector< ar::Statement* > confirmed_creations;
    JsonList candidate_diagnostics;
    std::map< std::string, std::size_t > reason_counts;
    if (pass != 0 && diagnostics_enabled) reason_counts = deferred_counts.at(kv.first);
    bool group_small_activation = false;
    bool group_worker_pairs = false;
    const bool group_completion_pairs = std::any_of(pair_indices.begin(), pair_indices.end(), [&](std::size_t i) {
      const auto& pair = all_pairs[i];
      return (this->_accesses[pair.i].thread_id == "main") != (this->_accesses[pair.j].thread_id == "main");
    });
    std::vector< std::size_t > query_order;
    for (std::size_t i = 0; i < pair_indices.size(); ++i) query_order.push_back(i);
    // Legacy confirmations precede newly enabled activation alternatives
    // within the same file deadline. Preserve the report's original pair IDs.
    if (pass == 1 && !small_activation.empty()) {
      auto priority = [&](std::size_t i) {
        const auto& pair = all_pairs[pair_indices[i]];
        const auto& a = this->_accesses[pair.i].thread_id;
        const auto& b = this->_accesses[pair.j].thread_id;
        if (a == "main" || b == "main" || small_activation.count(std::minmax(a, b)) == 0) return 0;
        // Different entries may expose a protocol mismatch before spending
        // the deadline on repeated instances of one entry. This only ranks work.
        return a == b ? 2 : 1;
      };
      std::stable_sort(query_order.begin(), query_order.end(), [&](std::size_t i, std::size_t j) {
        return priority(i) < priority(j);
      });
    }
    // Held reports must still be emitted if another group succeeded or the
    // unchanged file deadline expired before the new occurrence-only pass.
    const bool final_budget_exhausted = pass >= 4 &&
        (search_budget.candidates == 0 || search_budget.queries == 0);
    if (pass >= 3 && (file_confirmed || search_budget.expired() || final_budget_exhausted ||
                      (pass == 4 && startup_hint <= 16))) {
      query_order.clear();
      if (pass == 4 && diagnostics_enabled) candidate_diagnostics = occurrence_candidates.at(kv.first);
      if (!file_confirmed && (search_budget.expired() || final_budget_exhausted))
        group_result.merge(bmc::Result(bmc::Status::BudgetExceeded));
      if (!file_confirmed) {
        if (diagnostics_enabled && search_budget.expired()) ++reason_counts["search_deadline"];
        if (diagnostics_enabled && final_budget_exhausted && search_budget.candidates == 0) ++reason_counts["candidate_budget"];
        if (diagnostics_enabled && final_budget_exhausted && search_budget.queries == 0) ++reason_counts["query_budget"];
      }
      if (diagnostics_enabled && (file_confirmed || search_budget.expired() || final_budget_exhausted))
        ++reason_counts[file_confirmed ? "not_searched_after_confirmation" : "search_budget"];
    }
    std::size_t group_attempts = 0;
    for (std::size_t candidate_id : query_order) {
      if (!bmc_enabled && !diagnostics_enabled) {
        break;
      }
      const std::size_t pk = pair_indices[candidate_id];
      const PendingRacePair& pr = all_pairs[pk];
      const AccessRecord& a = this->_accesses[pr.i];
      const AccessRecord& b = this->_accesses[pr.j];
      const bool worker_pair = a.thread_id != "main" && b.thread_id != "main";
      group_worker_pairs |= worker_pair;
      if (pass == 5 ? worker_pair : pass >= 3 && !worker_pair) continue;
      bmc::Result candidate_result(bmc::Status::NotSearched);
      bmc::Diagnostics diagnostics;
      bmc::Diagnostics* trace = diagnostics_enabled ? &diagnostics : nullptr;
      bool eligible = true;
      auto gate = [&](bool blocked, const char* reason, ar::Statement* stmt = nullptr) {
        if (blocked) {
          eligible = false;
          bmc::record(trace, reason, stmt);
          candidate_result.merge(bmc::Result(bmc::Status::Unsupported));
        }
      };
      // Evaluate independent eligibility gates even when another one fails.
      if (worker_pair) {
        auto key = std::minmax(a.thread_id, b.thread_id);
        auto proof = instance_proofs.find(key);
        if (proof == instance_proofs.end()) {
          bool live = false;
          if (pass >= 3) {
            if (pass == 4) bmc::record(trace, "startup_literal_loop_hint");
            live = conditional_live_threads(a.thread_id, b.thread_id, pass == 4 ? startup_hint : 16, true);
            if (!live && pass == 3) {
              auto* main = this->_ctx.bundle->function_or_null("main");
              auto* source = bmc::allocation_finite_source(main, 4);
              if (source != nullptr) {
                for (unsigned value = 1; value <= 4 && !search_budget.expired(); ++value) {
                  live |= conditional_live_threads(a.thread_id, b.thread_id, 16, true, source, value);
                  if (activation_prefixes[key].size() >= 8) break;
                }
                bmc::record(trace, "startup_finite_source");
              }
            }
          } else {
            live = two_live_threads(a.thread_id, b.thread_id);
            if (!live && pass < 2 && conditional_live_threads(a.thread_id, b.thread_id, 2)) {
              live = true;
              small_activation.insert(key);
            }
            if (!live) live = conditional_live_threads(a.thread_id, b.thread_id, 16);
          }
          if (pass == 2) {
            auto previous = short_activation_prefixes.find(key);
            if (previous != short_activation_prefixes.end() && !previous->second.empty()) {
              auto& paths = activation_prefixes[key];
              paths.insert(paths.begin(), previous->second.begin(), previous->second.end());
              live = true;
            }
          }
          proof = instance_proofs.emplace(key, live).first;
        }
        gate(!proof->second, "thread_activation_unproven");
        if (small_activation.count(key) != 0) {
          group_small_activation = true;
          bmc::record(trace, "activation_small_bound");
          if (pass == 0) {
            search_budget.saw_interrupted = true;
            gate(true, "activation_deferred");
          }
        }
      }
      gate(pr.is_unknown, "mixed_atomic");
      // The numeric identity tokens must not justify branch/value witnesses.
      // A future provenance model can narrow this program-wide boundary.
      gate(synthetic_handle_unsupported, "pthread_handle_representation");
      // Unsupported synchronization is checked on each executed prefix.
      // Unused functions and operations after the target cannot order a race
      // already present in that finite execution.
      gate(!bmc_enabled, "recovery_disabled");

      bool same_global = false;
      bool singleton = a.pts.is_set() && a.pts.size() == 1 &&
                       b.pts.is_set() && b.pts.size() == 1;
      auto supported_objects = [](const PointsToSet& pts) {
        return pts.is_set() && !pts.is_empty() &&
               std::all_of(pts.begin(), pts.end(), [](MemoryLocation* location) {
                 return isa< GlobalMemoryLocation >(location) || isa< DynAllocMemoryLocation >(location) ||
                        isa< LocalMemoryLocation >(location);
               });
      };
      bool global_candidates = supported_objects(a.pts) && supported_objects(b.pts);
      // May-alias sets only select candidates; executed definitions and the
      // encoder's same-base/overlap checks establish the actual object.
      gate(!singleton && !global_candidates, "points_to_not_singleton");
      gate(!global_candidates, "non_global_object");
      if (singleton) {
        MemoryLocation* ma = *a.pts.begin();
        MemoryLocation* mb = *b.pts.begin();
        bool global = isa< GlobalMemoryLocation >(ma) &&
                      isa< GlobalMemoryLocation >(mb);
        same_global = global &&
                      core::IndexableTraits< MemoryLocation* >::index(ma) ==
                          core::IndexableTraits< MemoryLocation* >::index(mb);
        gate(global && !same_global, "different_global_objects");
      }
      // This is only a may-overlap prefilter; SMT must establish actual overlap.
      auto eff_interval = [](const AccessRecord& access, ZNumber& lo,
                             ZNumber& hi) {
        if (access.offset.is_bottom()) {
          return false;
        }
        if (!access.offset.is_top()) {
          lo = access.offset.lb().to_z_number();
          hi = access.offset.ub().to_z_number();
          return true;
        }
        return false;
      };
      ZNumber lo_a, hi_a, lo_b, hi_b;
      bool off_overlap = false;
      if (eff_interval(a, lo_a, hi_a) && eff_interval(b, lo_b, hi_b)) {
        if (a.width != 0 && b.width != 0) {
          off_overlap = hi_a + ZNumber(a.width) > lo_b &&
                        hi_b + ZNumber(b.width) > lo_a;
        } else {
          off_overlap = lo_a <= hi_b && lo_b <= hi_a;
        }
      }
      bool var_index = a.offset.is_top() || b.offset.is_top();
      gate(!off_overlap && !var_index, "offset_prefilter");
      ar::Function* fa = this->_ctx.bundle->function_or_null(a.thread_id);
      ar::Function* fb = this->_ctx.bundle->function_or_null(b.thread_id);
      gate(fa == nullptr || fb == nullptr, "thread_entry_missing");
      auto direct_creator = [&](const std::string& x, const std::string& y) {
        auto it = this->_thread_creators.find(y);
        return it != this->_thread_creators.end() && it->second.count(x) != 0;
      };
      auto transitive_ancestor = [&](const std::string& x, const std::string& y) {
        auto it = this->_thread_ancestors.find(y);
        return it != this->_thread_ancestors.end() && it->second.count(x) != 0;
      };
      gate((transitive_ancestor(a.thread_id, b.thread_id) &&
            !direct_creator(a.thread_id, b.thread_id)) ||
               (transitive_ancestor(b.thread_id, a.thread_id) &&
                !direct_creator(b.thread_id, a.thread_id)),
           "transitive_thread_order");
      if (eligible) {
        if (bmc_confirmed || (pass != 0 && file_confirmed)) {
          bmc::record(trace, "not_searched_after_confirmation");
        } else if (group_attempts >= 32) {
          bmc::record(trace, "group_search_budget");
          candidate_result.merge(bmc::Result(bmc::Status::BudgetExceeded));
        } else if (search_budget.candidates == 0 || search_budget.expired()) {
          search_budget.record_exhaustion(trace);
          bmc::record(trace, "search_budget");
          candidate_result.merge(bmc::Result(bmc::Status::BudgetExceeded));
        } else {
          ++group_attempts;
          --search_budget.candidates;
          bmc::CandidateFacts facts;
          // Keep the historical master switch; independent factors isolate
          // numerical costs from helper-publication support.
          const bool disable_ai_facts = getenv("IKOS_BMC_DISABLE_AI_FACTS") != nullptr;
          if (!disable_ai_facts && getenv("IKOS_BMC_DISABLE_AI_SINGLE_EXECUTION") == nullptr) {
            facts.single_execution = [&](ar::Function* function) {
              return this->_ctx.concurrent_env.function_executes_at_most_once(function);
            };
          }
          if (!disable_ai_facts && getenv("IKOS_BMC_DISABLE_AI_NO_WRAP") == nullptr) {
            facts.no_wrap = &this->_bmc_no_wrap;
          }
          if (!disable_ai_facts && getenv("IKOS_BMC_DISABLE_AI_BOUNDS") == nullptr) {
            facts.bounded_a = eff_interval(a, facts.lo_a, facts.hi_a);
            facts.bounded_b = eff_interval(b, facts.lo_b, facts.hi_b);
          }
          auto confirm = [&] {
            if (pass == 5) {
              candidate_result.merge(bmc::confirm_race_completion(fa, a.stmt, a.width,
                  fb, b.stmt, b.width, trace, search_budget, &confirmed_creations, &facts));
            } else {
              candidate_result.merge(bmc::confirm_race(fa, a.stmt, a.width,
                  fb, b.stmt, b.width, trace, &search_budget, &confirmed_creations, &facts));
            }
            return candidate_result.confirmed();
          };
          if (worker_pair) {
            const auto& startups = activation_prefixes.at(std::minmax(a.thread_id, b.thread_id));
            if (pass < 3) {
              for (const auto& startup : startups) {
                facts.startup = startup;
                if ((bmc_confirmed = confirm())) {
                  confirmed_creations.clear();
                  for (const auto& event : startup)
                    if (event.kind == bmc::EvKind::Create) confirmed_creations.push_back(event.stmt);
                  break;
                }
              }
            } else {
              struct Selection {
                std::size_t rank;
                const std::vector< bmc::BmcEvent >* startup;
                std::size_t a, b;
              };
              std::vector< Selection > selections;
              bool more_occurrences = false;
              for (const auto& startup : startups) {
                // The cache key is unordered; count matching entries anew so
                // candidate A/B orientation cannot swap the selected instances.
                auto count = [&](ar::Function* entry) {
                  return static_cast< std::size_t >(std::count_if(startup.begin(), startup.end(), [&](const auto& event) {
                    return event.kind == bmc::EvKind::Create &&
                           event.thread == reinterpret_cast< std::uint64_t >(entry);
                  }));
                };
                const auto count_a = count(fa), count_b = count(fb);
                more_occurrences |= count_a * count_b - (fa == fb ? count_a : 0) > 32;
                std::size_t rank = 0;
                for (std::size_t ia = 0; ia < count_a && rank < 32; ++ia) {
                  for (std::size_t ib = 0; ib < count_b && rank < 32; ++ib) {
                    if (fa == fb && ia == ib) continue;
                    selections.push_back({rank++, &startup, ia, ib});
                  }
                }
              }
              // Give every retained startup its first choice before spending
              // the same selection budget on another choice of an earlier one.
              std::stable_sort(selections.begin(), selections.end(), [](const auto& x, const auto& y) {
                return x.rank < y.rank;
              });
              facts.selected_worker_occurrences = true;
              std::size_t occurrence_attempts = 0;
              for (const auto& selection : selections) {
                if (occurrence_attempts >= 32 || search_budget.expired() || search_budget.queries == 0) break;
                facts.startup = *selection.startup;
                facts.worker_a_occurrence = selection.a;
                facts.worker_b_occurrence = selection.b;
                ++occurrence_attempts;
                bmc::record(trace, "selected_activation_occurrences");
                if ((bmc_confirmed = confirm())) {
                  confirmed_creations.clear();
                  for (const auto& event : *selection.startup)
                    if (event.kind == bmc::EvKind::Create) confirmed_creations.push_back(event.stmt);
                  break;
                }
              }
              if (!bmc_confirmed && (occurrence_attempts < selections.size() || more_occurrences)) {
                bmc::record(trace, occurrence_attempts >= 32 ? "activation_selection_budget" : "search_budget");
                candidate_result.merge(bmc::Result(bmc::Status::BudgetExceeded));
              }
            }
          } else bmc_confirmed = confirm();
          if (bmc_confirmed) {
            selected = &pr;
            confirmed_pair_id = static_cast< int >(candidate_id);
          }
        }
      }
      group_result.merge(candidate_result);
      if (diagnostics_enabled) {
        const auto& evidence = candidate_result.evidence;
        JsonDict counts;
        for (const auto& entry : diagnostics.counts) {
          counts.put(entry.first, entry.second);
          reason_counts[entry.first] += entry.second;
        }
        // Keep examples bounded; aggregate counters still cover every pair.
        if (candidate_id < 256) {
          JsonList examples;
          for (const auto& example : diagnostics.examples) {
            JsonDict d;
            d.put("reason", example.reason);
            std::ostringstream ar_statement;
            example.stmt->dump(ar_statement);
            d.put("ar", ar_statement.str());
            if (auto loc = source_location(example.stmt)) {
              d.put("file", loc.path().string());
              d.put("line", static_cast< int >(loc.line()));
              d.put("column", static_cast< int >(loc.column()));
            }
            examples.add(d);
          }
          JsonDict d;
          d.put("pair_id", candidate_id);
          d.put("result", candidate_result.summary());
          d.put("first_blocker", diagnostics.first_blocker);
          d.put("counts", counts);
          d.put("paths_a", diagnostics.paths_a);
          d.put("paths_b", diagnostics.paths_b);
          d.put("path_pairs", diagnostics.path_pairs);
          d.put("examples", examples);
          if (!evidence.model.empty()) {
            auto path_info = [](const std::vector< bmc::Counterexample::Step >& steps) {
              JsonList path;
              for (const auto& step : steps) {
                JsonDict item;
                std::ostringstream statement;
                step.stmt->dump(statement);
                item.put("ar", statement.str());
                item.put("clock", step.clock);
                if (auto loc = source_location(step.stmt)) {
                  item.put("file", loc.path().string());
                  item.put("line", static_cast< int >(loc.line()));
                  item.put("column", static_cast< int >(loc.column()));
                }
                path.add(item);
              }
              return path;
            };
            d.put("confirmed_path_a", path_info(evidence.confirmed_a));
            d.put("confirmed_path_b", path_info(evidence.confirmed_b));
            d.put("confirmed_startup", path_info(evidence.confirmed_startup));
            d.put("smt_model", evidence.model);
            d.put("replay", evidence.replay);
          }
          candidate_diagnostics.add(d);
        }
      }
    }

    file_confirmed |= bmc_confirmed;
    const bool legacy_deferred = (pass == 0 && search_budget.saw_interrupted) ||
                                 (pass == 1 && (group_small_activation || search_budget.saw_interrupted) && !file_confirmed);
    const bool occurrence_deferred = pass < 3 && bmc_enabled && group_worker_pairs && !file_confirmed;
    // Hold the last unconfirmed worker reports until every original pass has
    // finished. With no larger hint the final pass only emits these results.
    const bool hint_deferred = pass == 3 && bmc_enabled && group_worker_pairs && !file_confirmed;
    // New completion searches follow all legacy passes in this same file budget.
    const bool completion_deferred = pass < 5 && bmc_enabled && group_completion_pairs && group_result.needs_completion && !file_confirmed;
    if (!bmc_confirmed && (legacy_deferred || occurrence_deferred || hint_deferred || completion_deferred)) {
      deferred_results[kv.first] = group_result;
      if (legacy_deferred) {
        if (pass == 0) deferred_groups.insert(kv.first);
        else larger_activation_groups.insert(kv.first);
      } else if (hint_deferred) hinted_activation_groups.insert(kv.first);
      else if (occurrence_deferred) occurrence_groups.insert(kv.first);
      if (completion_deferred) completion_groups.insert(kv.first);
      if (diagnostics_enabled) {
        if (pass == 0) deferred_candidates.emplace(kv.first, candidate_diagnostics);
        if (pass == 3) occurrence_candidates[kv.first] = candidate_diagnostics;
        deferred_counts[kv.first] = reason_counts;
      }
      continue;
    }
    completion_groups.erase(kv.first);
    // Flat targets and the report anchor must refer to the confirmed pair.
    JsonDict info{
        {"memory_location", first.first_conflict_loc.empty()
                                ? std::string("unknown") : first.first_conflict_loc},
        {"access_a", side_info(this->_accesses[selected->i],
                               selected->conflicting_pts, joined_names[selected->i])},
        {"access_b", side_info(this->_accesses[selected->j],
                               selected->conflicting_pts, joined_names[selected->j])},
        {"conflicting_accesses", conflicting_accesses},
        {"num_pairs", static_cast< int >(pair_indices.size())},
        {"unique_lockset_intersection", "empty"},
    };
    info.put("thread_creations", creation_info(bmc_confirmed ? &confirmed_creations : nullptr));
    if (bmc_confirmed) {
      info.put("confirmed_pair_id", confirmed_pair_id);
      info.put("counterexample", group_result.evidence.replay);
    }
    info.put("bmc_result", group_result.summary());
    if (group_is_unknown) {
      info.put("verdict", std::string("unknown"));
      info.put("unknown_reason", std::string("mixed atomic/non-atomic accesses"));
    }
    if (diagnostics_enabled) {
      JsonDict diagnostics, counts;
      for (const auto& entry : reason_counts) {
        counts.put(entry.first, entry.second);
      }
      diagnostics.put("counts", counts);
      diagnostics.put("budget", JsonDict{{"deadline_ms", search_budget.deadline_ms},
                                        {"solver_timeout_ms", search_budget.solver_timeout_ms},
                                        {"remaining_candidates", search_budget.candidates},
                                        {"remaining_queries", search_budget.queries}});
      diagnostics.put("candidates", candidate_diagnostics);
      if (pass != 0) diagnostics.put("first_pass_candidates", deferred_candidates.at(kv.first));
      if (pass == 4 && startup_hint > 16 && !query_order.empty())
        diagnostics.put("previous_activation_candidates", occurrence_candidates.at(kv.first));
      diagnostics.put("omitted_candidates", pair_indices.size() > 256 ? pair_indices.size() - 256 : 0);
      info.put("bmc", diagnostics);
    }

    this->_checks.insert(CheckKind::DataRace,
                         CheckerName::DataRace,
                         // Abstract candidates do not establish a real race,
                         // including when bounded confirmation is disabled.
                         (group_is_unknown || !bmc_confirmed)
                             ? Result::Warning
                             : Result::Error,
                         selected->report_stmt,
                         selected->report_cc,
                         /* operands = */ {},
                         /* info = */ info);
    // No terminal output: the aggregated info is the report. Downstream
    // tools (ikos-report, ikos-view) render it from the database.
  }
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

  this->_ownership->observe(stmt, inv, call_context);

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
      if (type != nullptr && inserted.first->second && bmc_local_integer(operation->left()) && bmc_local_integer(operation->right())) {
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
            cst->function()->name() != "pthread_create") {
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

void DataRaceChecker::check_extern_call_effects(
    ar::CallBase* call,
    const value::AbstractDomain& inv,
    CallContext* call_context) {
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
        if (fm != nullptr) may_thread_unsafe |= thread_unsafe(fm->function());
      }
    }
  } else {
    return; // undefined / null / asm / data: no side-effect synthesis
  }
  if (!extern_call) {
    return;
  }
  // Reuse the verified literal/integer-only summary, not a stdio name family:
  // buffer destinations, %n and pointer conversions can access shared data.
  if (bmc::constant_output_call(call)) return;

  auto scan = scalar_scan_effect(call);
  if (scan && !scan->implicit_state_closed) scan = boost::none;
  if (scan) {
    this->_checks.insert(
        CheckKind::DataRace, CheckerName::DataRace, Result::Ok,
        call, call_context, {},
        JsonDict{{"safe_reason", "exact scalar scanf application-memory effects"},
                 {"stdio_scan_contract", "scalar-percent-d-output-and-errno-v1"}});
  }

  const auto key_effect = pthread_key_effect(call, this->_ctx);
  if (key_effect == PthreadKeyEffect::Get || key_effect == PthreadKeyEffect::Set)
    return; // Runtime TLS slots, not memory pointed to by the stored value.
  if (key_effect == PthreadKeyEffect::Create &&
      !isa< ar::NullConstant >(call->argument(1))) {
    auto* callback = cast< ar::FunctionPointerConstant >(call->argument(1))->function();
    this->_checks.insert(
        CheckKind::DataRace, CheckerName::DataRace, Result::Ok,
        call, call_context, {},
        JsonDict{{"safe_reason", "complete TLS destructor has no application-memory or synchronization effects"},
                 {"tls_destructor_contract", "race-only-closed-memory-free-callback-v1"},
                 {"callback", callback->name()}});
  }

  auto* exact = bmc::exact_callee(call);
  const std::string callee_nm = exact != nullptr ? exact->name() : "";
  const bool initialization = certified_pthread_initialization(call, this->_ctx);
  unsigned ignored_arguments = 0;
  if (exact != nullptr && exact->is_declaration()) {
    // Preserve the existing lifetime/stack handling separately from user buffers.
    // Lifetime conflicts still need their own property and object model audit.
    if (exact->intrinsic_id() == ar::Intrinsic::LibcFree ||
        exact->intrinsic_id() == ar::Intrinsic::StackSave ||
        exact->intrinsic_id() == ar::Intrinsic::StackRestore) return;

    // Only runtime-managed sync slots are exempt. Attribute/timeout inputs and
    // caller-owned outputs remain conservative accesses; names alone are no gate.
    ignored_arguments = synchronization_ignored_arguments(call, exact);
  }

  if (initialization) ignored_arguments = 0;
  if (key_effect == PthreadKeyEffect::Create) ignored_arguments = 2;
  const auto& frame = this->_ctx.runtime_stdio_frame_certificate;
  if (frame && frame->omits_stderr_write(call, call_context)) {
    ignored_arguments |= 1u; // Only fprintf's actual runtime stream argument.
    auto info = frame->info(call);
    info.put("safe_reason", "runtime stderr is outside the complete application effect frame");
    this->_checks.insert(CheckKind::DataRace, CheckerName::DataRace, Result::Ok,
                         call, call_context, {}, std::move(info));
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
                         key_effect == PthreadKeyEffect::Create ? 4u : initialization
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
                   /*private_init=*/false,
                   /*promote_locks=*/{},
                   this->resolve_offset_congruence(load->operand(), inv)});
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
                   /*private_init=*/false,
                   /*promote_locks=*/{},
                   this->resolve_offset_congruence(store->pointer(), inv)});
}

} // end namespace analyzer
} // end namespace ikos
