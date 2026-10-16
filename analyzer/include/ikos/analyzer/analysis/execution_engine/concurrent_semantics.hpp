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

#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>

#include <ikos/ar/semantic/code.hpp>
#include <ikos/ar/semantic/statement.hpp>
#include <ikos/ar/semantic/value.hpp>

#include <ikos/core/number/z_number.hpp>
#include <ikos/core/domain/machine_int/interval.hpp>
#include <ikos/core/domain/lockset/lockset_domain.hpp>
#include <ikos/core/domain/concurrent_global_env.hpp>

#include <ikos/analyzer/analysis/call_context.hpp>
#include <ikos/analyzer/analysis/execution_engine/symbolic_index.hpp>
#include <ikos/analyzer/analysis/execution_engine/handle_origin.hpp>
#include <ikos/analyzer/analysis/execution_engine/stable_publication.hpp>
#include <ikos/analyzer/analysis/execution_engine/readonly_pointer_array.hpp>
#include <ikos/analyzer/analysis/execution_engine/heap_pointer_initialization.hpp>
#include <ikos/analyzer/analysis/execution_engine/protected_values.hpp>
#include <ikos/analyzer/analysis/execution_engine/native_thread_handles.hpp>
#include <ikos/analyzer/analysis/literal.hpp>
#include <ikos/analyzer/analysis/memory_location.hpp>
#include <ikos/analyzer/analysis/variable.hpp>
#include <ikos/analyzer/support/assert.hpp>
#include <ikos/analyzer/support/cast.hpp>
#include <ikos/analyzer/util/log.hpp>

#include <cstdint>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ikos {
namespace analyzer {

/// Pack only representable identities. Zero means no definite lock; callers
/// must not admit it to MUST state or retain locks across an unknown release.
inline std::uint64_t offset_lock_key(std::uint64_t addr, const core::ZNumber& off) {
  if (addr == 0 || addr > 0xFFFFFFFFULL || off < core::ZNumber(0) ||
      off > core::ZNumber(0xFFFFFFFFULL)) return 0;
  return (addr << 32) | off.to< std::uint64_t >();
}

inline std::uint64_t lock_key_base(std::uint64_t key) {
  return key >> 32;
}

inline std::uint64_t lock_key_instance(std::uint64_t key) {
  return key & 0xFFFFFFFFULL;
}

/// Preserve the full allocation and offset identities; unknown fields overlap
/// every exact field rather than occupying a disjoint packed sentinel slot.
inline core::ConcurrentGlobalEnv::HeapPointerKey heap_pointer_key(
    std::uint64_t stable_id, const core::machine_int::Interval& off) {
  if (auto s = off.singleton())
    return {stable_id, true, s->to_z_number()};
  return {stable_id, false, core::ZNumber(0)};
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
inline bool dynalloc_in_loop(ar::Statement* call) {
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

inline bool concrete_lock_object(MemoryLocation* location,
                                 core::ConcurrentGlobalEnv& env) {
  if (auto* global = dyn_cast< GlobalMemoryLocation >(location))
    return !global->global_var()->is_thread_local();
  ar::Statement* allocation = nullptr;
  if (auto* dynamic = dyn_cast< DynAllocMemoryLocation >(location)) {
    allocation = dynamic->call();
  } else if (auto* local = dyn_cast< LocalMemoryLocation >(location)) {
    auto* function = local->local_var()->function();
    if (function == nullptr || !function->is_definition()) return false;
    for (auto* block : *function->body_or_null()) for (auto* statement : *block) {
      if (statement->result_or_null() != local->local_var()) continue;
      if (allocation != nullptr || !isa< ar::Allocate >(statement)) return false;
      allocation = statement;
    }
  }
  // A singleton abstract location can summarize several live allocations.
  return allocation != nullptr && !dynalloc_in_loop(allocation) &&
         allocation->code()->is_function_body() &&
         env.function_executes_at_most_once(allocation->code()->function());
}

/// \brief Classification of verifier atomic functions and section markers.
enum class AtomicKind {
  None,          // not an atomic primitive
  SectionBegin,  // __VERIFIER_atomic_begin — pseudo-lock added
                 // and kept across the call
  SectionEnd,    // __VERIFIER_atomic_end — pseudo-lock removed
                 // after the call (assumed on from the matching begin)
  SingleCall,    // __VERIFIER_atomic_<suffix> — pseudo-lock wraps the body
};

// Site ids are opaque identity tokens, not the integer representation of
// pthread_t. Only direct identity consumers and unique same-width copies may
// use these tokens without a representation/value-flow model.
inline bool identity_only_handle_uses(ar::InternalVariable* root) {
  auto* function = root->code()->function_or_null();
  if (function == nullptr || !function->is_definition()) return false;
  std::vector< ar::InternalVariable* > pending{root};
  std::unordered_set< ar::InternalVariable* > seen;
  while (!pending.empty()) {
    auto* value = pending.back(); pending.pop_back();
    if (!seen.insert(value).second) continue;
    if (seen.size() > 256) return false;
    for (auto* block : *function->body_or_null()) for (auto* statement : *block) {
      if (std::find(statement->op_begin(), statement->op_end(), value) ==
          statement->op_end()) continue;
      if (auto* call = dyn_cast< ar::CallBase >(statement)) {
        auto* target = dyn_cast< ar::FunctionPointerConstant >(call->called());
        if (target != nullptr && target->function()->is_declaration() &&
            (target->function()->name() == "pthread_join" ||
             target->function()->intrinsic_id() == ar::Intrinsic::PthreadJoin) &&
            call->num_arguments() == 2 && call->argument(0) == value &&
            call->argument(1) != value) continue;
        // POSIX detach consumes opaque identity and creates no join/HB edge.
        if (target != nullptr && target->function()->is_declaration() &&
            target->function()->name() == "pthread_detach" &&
            !target->function()->type()->is_var_arg() &&
            target->function()->num_parameters() == 1 &&
            call->num_arguments() == 1 && call->argument(0) == value) {
          auto* result_type = dyn_cast< ar::IntegerType >(
              target->function()->type()->return_type());
          if (result_type && result_type->bit_width() == 32) continue;
        }
        return false;
      }
      ar::InternalVariable* copy = nullptr;
      if (auto* assign = dyn_cast< ar::Assignment >(statement)) copy = assign->result();
      if (auto* cast = dyn_cast< ar::UnaryOperation >(statement)) {
        if (cast->op() == ar::UnaryOperation::Bitcast) copy = cast->result();
      }
      auto* source_type = dyn_cast< ar::IntegerType >(value->type());
      auto* copy_type = copy ? dyn_cast< ar::IntegerType >(copy->type()) : nullptr;
      if (copy_type == nullptr || source_type == nullptr ||
          copy_type->bit_width() != source_type->bit_width() ||
          unique_def(copy) != statement) return false;
      pending.push_back(copy);
    }
  }
  return true;
}

template < typename E >
bool touches_synthetic_handle(E& eng, const ScalarLit& pointer) {
  if (eng.ctx().native_handles_only || !pointer.is_pointer_var()) return false;
  auto points = eng.data().normal().pointer_to_points_to(pointer.var());
  if (points.is_top()) return true;
  if (!points.is_set()) return false;
  for (auto* location : points)
    if (eng.ctx().concurrent_env.is_synthetic_handle_slot(location)) return true;
  return false;
}

template < typename E >
bool initial_main_handle_seed(E& eng, ar::Statement* statement,
                              const ScalarLit& pointer) {
  auto* store = dyn_cast< ar::Store >(statement);
  auto* source = store && store->has_frontend()
      ? llvm::dyn_cast< llvm::StoreInst >(store->frontend< llvm::Value >()) : nullptr;
  if (!source || !source->getMetadata("ikos.freeze_uninit") ||
      source->isVolatile() || source->isAtomic() || !pointer.is_pointer_var()) return false;
  auto* local = dyn_cast< ar::LocalVariable >(store->pointer());
  auto* function = local ? local->function() : nullptr;
  auto* type = local ? dyn_cast< ar::IntegerType >(local->type()->pointee()) : nullptr;
  auto* variable = dyn_cast< ar::InternalVariable >(store->value());
  auto* seed = variable ? dyn_cast_or_null< ar::CallBase >(unique_def(variable)) : nullptr;
  auto* target = seed ? dyn_cast< ar::FunctionPointerConstant >(seed->called()) : nullptr;
  auto canonical = [](ar::CallBase* call) {
    auto* target = dyn_cast< ar::FunctionPointerConstant >(call->called());
    return target && target->function()->is_declaration() &&
        target->function()->intrinsic_id() == ar::Intrinsic::IkosNonDet &&
        !target->function()->type()->is_var_arg() && target->function()->num_parameters() == 0 &&
        call->num_arguments() == 0 && target->function()->type()->return_type()->is_integer();
  };
  auto* allocation_source = llvm::dyn_cast< llvm::AllocaInst >(source->getPointerOperand());
  auto* seed_source = llvm::dyn_cast< llvm::CallInst >(source->getValueOperand());
  auto* size = allocation_source
      ? llvm::dyn_cast< llvm::ConstantInt >(allocation_source->getArraySize()) : nullptr;
  if (!function || function->name() != "main" || !type || !seed || !target ||
      !canonical(seed) || target->function()->type()->return_type() != variable->type() ||
      !variable->type()->is_integer() ||
      cast< ar::IntegerType >(variable->type())->bit_width() != type->bit_width() ||
      !allocation_source || !size || !size->isOne() ||
      !allocation_source->getAllocatedType()->isIntegerTy(type->bit_width()) ||
      !seed_source || !seed_source->getMetadata("ikos.freeze_uninit") ||
      source->getPrevNode() != seed_source || !seed->has_frontend() ||
      seed->frontend< llvm::Value >() != seed_source ||
      !seed_source->getCalledFunction() || !seed_source->getCalledFunction()->isDeclaration() ||
      seed_source->getCalledFunction()->isVarArg() || seed_source->arg_size() != 0 ||
      seed_source->getCalledFunction()->arg_size() != 0 ||
      function->bundle()->global_or_null("ar.global_ctors") ||
      !eng.ctx().opts.no_init_globals.empty() ||
      !eng.ctx().concurrent_env.function_executes_at_most_once(function) ||
      (eng.call_context() && !eng.call_context()->empty()) ||
      eng.lockset().spawned_is_top() || !eng.lockset().get_spawned_instances().empty()) return false;
  for (auto* root : eng.ctx().opts.entry_points) if (root != function) return false;
  auto* entry = function->body()->entry_block();
  if (entry->num_predecessors() != 0 || store->parent() != entry) return false;
  auto points = eng.data().normal().pointer_to_pointer(pointer.var());
  auto offset = points.offset().singleton();
  if (!points.points_to().is_set() || points.points_to().size() != 1 ||
      *points.points_to().begin() != eng.ctx().mem_factory->get_local(local) ||
      !offset || !offset->is_zero()) return false;
  unsigned budget = 8192;
  for (auto it = function->bundle()->global_begin(); it != function->bundle()->global_end(); ++it)
    if (auto* code = (*it)->initializer_or_null())
      for (auto* block : *code) for (auto* item : *block) {
        if (budget-- == 0 || isa< ar::CallBase >(item)) return false;
      }
  ar::Allocate* allocation = nullptr;
  ar::Statement* previous = nullptr;
  for (auto* item : *entry) {
    if (budget-- == 0) return false;
    if (item == store) return allocation && previous == seed;
    if (item->result_or_null() == local) {
      if (allocation) return false;
      allocation = dyn_cast< ar::Allocate >(item);
      auto* count = allocation ? dyn_cast< ar::IntegerConstant >(allocation->array_size()) : nullptr;
      if (!allocation || !count || count->value().to_z_number() != 1 ||
          allocation->allocated_type() != type || !allocation->has_frontend() ||
          allocation->frontend< llvm::Value >() != allocation_source) return false;
    }
    if (auto* call = dyn_cast< ar::CallBase >(item)) if (!canonical(call)) return false;
    previous = item;
  }
  return false;
}

template < typename E >
void check_synthetic_handle_effect(E& eng, ar::Statement* statement,
                                   const ScalarLit& pointer,
                                   ar::InternalVariable* result = nullptr) {
  // A fresh frontend seed in the initial main prefix consumes no handle value.
  // Keep its memory/race effects and all later TOP/representation checks.
  if (initial_main_handle_seed(eng, statement, pointer)) return;
  // A private slot cannot contain this frame's create token before any of
  // its associated sites may have executed. This includes ikos-pp's seed
  // store, even if unrelated initializers precede the actual create call.
  if (isa< ar::Store >(statement) && pointer.is_pointer_var() &&
      !eng.lockset().spawned_is_top()) {
    auto points = eng.data().normal().pointer_to_points_to(pointer.var());
    auto spawned = eng.lockset().get_spawned_instances();
    bool before_create = points.is_set() && !points.is_empty();
    if (points.is_set()) for (auto* location : points) {
      if (!isa< LocalMemoryLocation >(location) ||
          eng.ctx().concurrent_env.is_escaped(location)) {
        before_create = false;
        break;
      }
      for (auto sid : eng.ctx().concurrent_env.synthetic_handle_slot_ids(location))
        if (spawned.count(sid)) before_create = false;
    }
    if (before_create) return;
  }
  if (touches_synthetic_handle(eng, pointer) &&
      (result == nullptr || !identity_only_handle_uses(result))) {
    eng.ctx().concurrent_env.record_unmodeled_effect(
        statement, "synthetic pthread handle representation not modeled");
  }
}

template < typename E >
bool proven_synthetic_handle_source(E& eng, ar::Value* value, std::uint64_t sid) {
  for (unsigned depth = 0; depth < 64; ++depth) {
    auto* variable = dyn_cast< ar::InternalVariable >(value);
    if (variable == nullptr) return false;
    auto* definition = unique_def(variable);
    if (auto* load = dyn_cast_or_null< ar::Load >(definition)) {
      const auto& pointer = eng.lit_factory().get_scalar(load->operand());
      if (!pointer.is_pointer_var()) return false;
      auto points = eng.data().normal().pointer_to_points_to(pointer.var());
      auto offset = eng.data().normal().pointer_to_pointer(pointer.var()).offset();
      auto* type = dyn_cast< ar::IntegerType >(load->result()->type());
      if (!points.is_set() || points.is_empty() || !offset.singleton() ||
          type == nullptr) return false;
      const auto width = eng.data_layout().store_size_in_bytes(type);
      for (auto* location : points)
        if (!eng.ctx().concurrent_env.synthetic_handle_range_has_source(
                location, offset.lb().to_z_number(), width, sid)) return false;
      return true;
    }
    if (auto* call = dyn_cast_or_null< ar::CallBase >(definition)) {
      auto* target = dyn_cast< ar::FunctionPointerConstant >(call->called());
      return target != nullptr && target->function()->is_declaration() &&
             target->function()->name() == "pthread_self";
    }
    ar::Value* operand = nullptr;
    if (auto* assignment = dyn_cast_or_null< ar::Assignment >(definition))
      operand = assignment->operand();
    if (auto* cast = dyn_cast_or_null< ar::UnaryOperation >(definition)) {
      if (cast->op() == ar::UnaryOperation::Bitcast) operand = cast->operand();
    }
    auto* source_type = operand ? dyn_cast< ar::IntegerType >(operand->type()) : nullptr;
    auto* type = dyn_cast< ar::IntegerType >(variable->type());
    if (source_type == nullptr || type == nullptr ||
        source_type->bit_width() != type->bit_width()) return false;
    value = operand;
  }
  return false;
}

/// \brief Single source of truth for atomic-intrinsic detection.
/// Centralised here so the DECLARATION-call, match_down, and match_up
/// sites all agree on what counts as atomic.
inline AtomicKind classify_atomic_intrinsic(const std::string& fname) {
  // Only begin/end delimit an atomic section in the caller. Other verifier
  // atomic functions, including acquire/release, make just their body atomic
  // under the SV-COMP rules, not the subsequent caller statements.
  if (fname == "__VERIFIER_atomic_begin") {
    return AtomicKind::SectionBegin;
  }
  if (fname == "__VERIFIER_atomic_end") {
    return AtomicKind::SectionEnd;
  }
  // GCC lowers supported builtins to atomic AR accesses. An arbitrary
  // __sync_*/__atomic_* name establishes neither an atomicity nor an effect
  // contract: analyze definitions normally and gate uncovered declarations.
  // SV-COMP single-call primitives. __VERIFIER_atomic_w is treated as a
  // single-call atomic (no companion end-marker; it just flips a flag).
  if (fname == "__VERIFIER_atomic_CAS" ||
      fname == "__VERIFIER_atomic_TAS" ||
      fname == "__VERIFIER_atomic_w" ||
      fname == "__VERIFIER_atomic_assert") {
    return AtomicKind::SingleCall;
  }
  // SV-COMP specifies every body with this prefix as non-interleaved.
  // Declaration calls still need the separate omitted-effect gate below.
  if (fname.size() > 18 && fname.compare(0, 18, "__VERIFIER_atomic_") == 0) {
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
  eng.ctx().concurrent_env.invalidate_mutex_values();
  eng.lockset().record_possible_mutex_acquisition();
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
          if (key != 0 && eng.lockset().holds_lock(key)) {
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
  auto old_held = value_mutexes(eng);
  const bool first_acquisition = !eng.lockset().may_ever_acquired_mutex() &&
                                 eng.lockset().held_mutexes().empty();
  auto* target = dyn_cast< ar::FunctionPointerConstant >(call->called());
  const bool ordinary_mutex = target &&
      (target->function()->name() == "pthread_mutex_lock" ||
       target->function()->name() == "ar.pthread.mutex.lock");
  if (!ordinary_mutex) eng.ctx().concurrent_env.invalidate_mutex_values();
  eng.lockset().record_possible_mutex_acquisition();
  bool precise_acquisition = false;
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
        if (concrete_lock_object(loc, eng.ctx().concurrent_env)) {
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
            if (key != 0) {
              acquired_addrs.push_back(key);
              precise_acquisition = true;
              env.record_lock_instance(key, instance_base_offset(eng, mutex_arg));
            }
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
                 points_to.points_to().size() > 1 &&
                 std::all_of(points_to.points_to().begin(), points_to.points_to().end(),
                             [&](MemoryLocation* loc) {
                               return concrete_lock_object(loc, eng.ctx().concurrent_env);
                             })) {
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
  // HELD_IF_NONZERO/HELD_IF_ZERO conditional fact in addition to the held
  // must-lock, so the correlation survives a join with the unlocked path.
  // The checker
  // resolves it only when the guard's interval is a singleton matching the
  // polarity — the sound direction (unknown guard ⇒ lock not admitted).
  std::uint64_t branch_guard = 0;
  core::lockset::CondPolarity branch_polarity =
      core::lockset::CondPolarity::HELD_IF_NONZERO;
  bool conditional_lock = false;
  ar::BasicBlock* branch_false_bb = nullptr; // false-branch block (for replay)
  auto* guard_function = call->code()->function_or_null();
  if (!read_lock && !trylock && guard_function != nullptr &&
      eng.ctx().concurrent_env.function_executes_at_most_once(guard_function)) {
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
            // Prove zero-equivalence at machine width and a fixed activation;
            // a static AR variable is not a dynamic call ID.
            branch_guard = nonzero_guard_identity(cmp->left());
            if (branch_guard != 0) {
              branch_polarity = core::lockset::CondPolarity::HELD_IF_NONZERO;
              conditional_lock = true;
            }
          } else {
            const Literal& g = eng.lit_factory().get(cmp->left());
            if (g.is_scalar() && g.scalar().is_var() &&
                nonzero_guard_identity(cmp->left()) != 0) {
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

  if (ordinary_mutex && precise_acquisition && !read_lock && !trylock &&
      acquired_addrs.size() == 1 && first_acquisition) {
    auto new_held = old_held;
    new_held.insert(acquired_addrs.front());
    refresh_protected_values(eng, new_held, true);
  } else {
    refresh_protected_values(eng, old_held, false);
  }
  for (std::uint64_t la : acquired_addrs) {
    if (!read_lock && !trylock && eng.lockset().held_mutexes().count(la) != 0) {
      // ERRORCHECK may fail without increasing depth; NORMAL may block.
      // A concrete address alone does not prove recursive acquisition success.
      eng.ctx().concurrent_env.record_unmodeled_effect(
          call, "repeated mutex acquisition type/success not covered");
    }
    if (read_lock) {
      eng.lockset().add_read_lock(la);
    } else if (trylock) {
      eng.lockset().add_cond_lock(cond_guard, la);
    } else if (conditional_lock) {
      // This branch has executed the blocking acquisition.
      eng.lockset().add_lock(la);
      eng.lockset().add_cond_lock(branch_guard, la, branch_polarity);
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
      // crash. Deferred writes are flushed by the unlock executor.
    }
  }

  // Initialise the return value (int rc) to a non-deterministic int.
  // pthread_mutex_lock may legally return success (0) or specific error
  // codes. MUST ownership requires the supported successful-acquire contract;
  // the return abstraction itself is not evidence of successful acquisition.
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

/// \brief Remove resolved locks and flush their deferred write queues.
template < typename E >
void exec_pthread_mutex_unlock(E& eng, ar::CallBase* call, bool also_read = false) {
  auto before = value_mutexes(eng);
  auto* target = dyn_cast< ar::FunctionPointerConstant >(call->called());
  const bool ordinary_mutex = target &&
      (target->function()->name() == "pthread_mutex_unlock" ||
       target->function()->name() == "ar.pthread.mutex.unlock");
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
              auto key = offset_lock_key(base, poff.lb().to_z_number());
              if (key != 0) released_addrs.push_back(key);
              else released_unknown = true;
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

  if (ordinary_mutex && !also_read && !released_unknown && released_addrs.size() == 1 &&
      before.count(released_addrs.front()) != 0) {
    publish_protected_release(eng, before, value_mutexes(eng));
  } else {
    eng.ctx().concurrent_env.invalidate_mutex_values();
  }

  // Flush deferred writes for resolved locks into their summary partitions.
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
      // the next iteration handle it. Deferred writes were flushed above.
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
  if (eng.ctx().native_handle_joins.count(call)) {
    // Dropping the wait admits schedules; a MAY native value proves no join.
    if (call->has_result()) {
      const auto& ret = eng.lit_factory().get_scalar(call->result());
      eng.data().normal().scalar_assign_nondet(ret.var());
    }
    return;
  }
  auto& env = eng.ctx().concurrent_env;
  std::unordered_set< std::uint64_t > joined_sids;

  ar::Value* handle_arg = *call->arg_begin();
  const auto& handle_lit = eng.lit_factory().get(handle_arg);
  if (handle_lit.is_scalar()) {
    const auto& scalar_lit = handle_lit.scalar();
    if (scalar_lit.is_machine_int_var()) {
      auto iv = eng.data().normal().int_to_interval(scalar_lit.var());
      // A hull's registered upper bound does not establish a MUST target.
      if (auto single = iv.singleton()) {
        std::uint64_t sid =
            single->cast(single->bit_width(), core::Unsigned)
                .to_z_number().template to< std::uint64_t >();
        if (sid == core::kMainThreadSid ||
            (sid != 0 && env.site_func(sid) != nullptr)) {
          joined_sids.insert(sid);
        }
      }
    } else if (scalar_lit.is_machine_int()) {
      std::uint64_t sid = scalar_lit.machine_int()
                              .cast(scalar_lit.machine_int().bit_width(), core::Unsigned)
                              .to_z_number()
                              .template to< std::uint64_t >();
      if (sid == core::kMainThreadSid ||
          (sid != 0 && env.site_func(sid) != nullptr)) {
        joined_sids.insert(sid);
      }
    }
    // Pointer / other arg forms carry no instance value → nothing to join.
  }

  if (!joined_sids.empty() &&
      !proven_synthetic_handle_source(eng, handle_arg, *joined_sids.begin())) {
    joined_sids.clear();
    env.record_unmodeled_effect(
        call, "synthetic pthread handle representation not modeled");
  }

  for (std::uint64_t sid : joined_sids) {
    eng.lockset().add_joined_thread(sid);
    // Transitive join HB: joining a thread also joins every thread IT
    // definitely joined before exiting (main joins t_benign, t_benign joins
    // t_fun ⇒ main transitively joins t_fun — 51-threadjoins/01-trivial.c).
    // The summary is the joined thread's exit digest, transitively closed
    // because this same site pulls the summary of whatever IT joined.
    ar::Function* f = env.site_func(sid);
    if (f != nullptr && env.thread_instances_are_static(f->name())) {
      eng.lockset().remove_live_child(sid);
    }
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
  if (once_func != nullptr && once_func->is_definition()) {
    // Key the once-instance by the ONCE-CONTROL object (arg0), not the
    // callback name: distinct fields of one global are distinct controls.
    // Prove an exact static base+offset; summarized or TLS objects may have
    // multiple concrete instances and cannot establish a MUST join.
    std::uint64_t key = 0;
    core::ZNumber offset(0);
    bool joinable = false;
    if (call->arg_begin() != call->arg_end()) {
      ar::Value* ctrl_arg = *call->arg_begin();
      for (int depth = 0; depth < 16; ++depth) {
        if (auto* gv = dyn_cast< ar::GlobalVariable >(ctrl_arg)) {
          auto* type = dyn_cast< ar::PointerType >(call->argument(0)->type());
          auto* object = gv->type()->pointee();
          if (gv->is_definition() && !gv->is_thread_local() &&
              !object->is_opaque() && type != nullptr && !type->pointee()->is_opaque() &&
              offset >= 0 && offset + eng.data_layout().store_size_in_bytes(type->pointee()) <=
                                 eng.data_layout().alloc_size_in_bytes(object)) {
            key = core::IndexableTraits< MemoryLocation* >::index(
                eng.ctx().mem_factory->get_global(gv));
            joinable = true;
          }
          break;
        }
        auto* iv = dyn_cast< ar::InternalVariable >(ctrl_arg);
        if (iv == nullptr) {
          break;
        }
        ar::Statement* def = unique_def(iv);
        if (auto* ps = dyn_cast_or_null< ar::PointerShift >(def)) {
          bool exact = true;
          for (std::size_t i = 0; i < ps->num_terms(); ++i) {
            auto term = ps->term(i);
            auto* constant = dyn_cast< ar::IntegerConstant >(term.second);
            if (constant == nullptr) { exact = false; break; }
            offset += term.first.to_z_number() * constant->value().to_z_number();
          }
          if (!exact) break;
          ctrl_arg = ps->pointer();
        } else if (auto* un = dyn_cast_or_null< ar::UnaryOperation >(def)) {
          ctrl_arg = un->op() == ar::UnaryOperation::Bitcast ? un->operand() : nullptr;
        } else {
          break; // Load / call / phi: ambiguous control
        }
        if (ctrl_arg == nullptr) {
          break;
        }
      }
    }
    if (!joinable) {
      key = reinterpret_cast< std::uint64_t >(call);
      eng.ctx().concurrent_env.record_unmodeled_effect(
          call, "pthread_once control object not proven unique");
    }
    std::uint64_t sid =
        eng.ctx().concurrent_env.register_once_thread(once_func, key, joinable, offset);
    if (sid != 0) {
      eng.lockset().add_joined_thread(sid);
    }
  } else {
    eng.ctx().concurrent_env.record_unmodeled_effect(
        call, "pthread_once callback body not covered");
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
    if (!identity_only_handle_uses(call->result())) {
      eng.ctx().concurrent_env.record_unmodeled_effect(
          call, "synthetic pthread handle representation not modeled");
    }
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


/// \brief Handle SV-COMP verifier atomic-section declarations.
///
/// Their indivisibility contract uses a common synthetic lock. Declarations
/// with an omitted body still need a separate memory/value coverage boundary.
/// GCC builtins lowered to atomic AR accesses use their Load/Store semantics;
/// other declarations require effect summaries and go through the unknown gate.
///
/// Distinguish three sub-cases (see `classify_atomic_intrinsic`):
///   1. SingleCall atomics: pseudo-lock wraps just the call.
///   2. SectionBegin markers: pseudo-lock is added and LEFT ON until the
///      matching end.
///   3. SectionEnd markers: pseudo-lock (assumed on since the matching
///      begin) is removed AFTER exec.
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
      // Atomicity does not cover an omitted body's values or memory effects.
      eng.ctx().concurrent_env.record_unmodeled_effect(
          call, "atomic call effects not covered");
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
/// For SECTION MARKERS (begin/end), the pseudo-lock is also
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
      // SectionEnd: pseudo-lock was added by the matching begin;
      // leave it across the call so match_up can remove it.
      // None: not an atomic primitive.
      break;
  }
}

/// \brief Remove the pseudo-lock after an atomic definition call (match_up
/// side of the pseudo-lock model).
///
/// For SECTION-END markers, the pseudo-lock was added by the matching
/// begin; remove it so the caller's state after the call no longer
/// carries the atomic section. For SINGLE-CALL verifier atomics, the
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

/// Join integer interference through memory, retaining the local alternative.
/// Flat summaries only describe full scalar globals; other shared layouts havoc.
template < typename E >
void on_load_integer_interference(E& eng,
                                  ar::Load* s,
                                  const ScalarLit& ptr,
                                  const ScalarLit& lhs,
                                  const core::MachineInt& size) {
  if (!lhs.is_machine_int_var()) return;
  auto& env = eng.ctx().concurrent_env;
  auto points = eng.data().normal().pointer_to_points_to(ptr.var());
  if (points.is_bottom()) return;
  bool shared = points.is_top();
  if (points.is_set()) {
    for (MemoryLocation* loc : points) {
      auto* global = dyn_cast< GlobalMemoryLocation >(loc);
      if ((global && (!global->global_var()->is_thread_local() ||
                      env.is_escaped(loc))) ||
          isa< DynAllocMemoryLocation >(loc) || env.is_escaped(loc)) {
        shared = true;
      }
    }
  }
  if (!shared) return;
  auto* type = cast< ar::IntegerType >(s->result()->type());
  auto external = core::machine_int::Interval::bottom(type->bit_width(), type->sign());
  bool full_scalar = false;
  if (points.is_set() && points.size() == 1) {
    if (auto* global = dyn_cast< GlobalMemoryLocation >(*points.begin())) {
      auto* declared = dyn_cast< ar::IntegerType >(global->global_var()->type()->pointee());
      auto offset = eng.data().normal().pointer_to_pointer(ptr.var()).offset().singleton();
      full_scalar = declared && declared->bit_width() == type->bit_width() &&
                    offset && offset->is_zero();
      if (full_scalar) {
        if (env.is_synthetic_handle_slot(global)) {
          auto sid = completed_handle_origin(eng, global->global_var(), s);
          if (sid != 0) {
            eng.data().normal().mem_write(
                ptr.var(), core::Literal< Variable*, MemoryLocation* >::machine_int(
                    core::MachineInt(sid, type->bit_width(), type->sign())), size);
            return;
          }
        }
        auto addr = core::IndexableTraits< MemoryLocation* >::index(global);
        auto held = value_mutexes(eng);
        if (env.mutex_values_available() && env.has_mutex_value_cell(addr) && !held.empty()) {
          external = env.read_mutex_ongoing(addr, held, type->bit_width(), type->sign());
          if (external.is_bottom()) return; // no new interference; retain own memory.
        } else {
          external = env.read_global_interference(addr, type->bit_width(), type->sign());
        }
      }
    }
  }
  // Absence is not a readonly/coverage proof, nor a field-sensitive summary.
  if (!full_scalar || external.is_bottom()) {
    eng.data().normal().mem_forget_reachable(ptr.var(), size);
    return;
  }
  auto alternative = eng.data();
  alternative.normal().scalar_assign_nondet(lhs.var());
  alternative.normal().int_set(lhs.var(), external);
  alternative.normal().mem_write(ptr.var(), lhs, size);
  alternative.normal().scalar_forget(lhs.var());
  eng.data().join_with(alternative);
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

/// Join pointer may-writes through memory; retain every local/source alternative.
template < typename E >
void on_load_pointer_restore(E& eng,
                             ar::Load* load,
                             const ScalarLit& ptr,
                             const ScalarLit& lhs,
                             const core::MachineInt& size) {
  auto sources = eng.data().normal().pointer_to_points_to(ptr.var());
  if (!sources.is_set()) return;
  auto& env = eng.ctx().concurrent_env;
  auto offset = eng.data().normal().pointer_to_pointer(ptr.var()).offset().singleton();
  if (sources.size() == 1 && offset && offset->is_zero() && lhs.var() != ptr.var() &&
      size.to_z_number() == core::ZNumber(eng.data_layout().pointers.bit_width / 8) &&
      load->code()->is_function_body()) {
    auto* source = dyn_cast< GlobalMemoryLocation >(*sources.begin());
    bool only_main_root = true;
    for (auto* root : eng.ctx().opts.entry_points) only_main_root &= root->name() == "main";
    if (source && only_main_root) {
      auto* writer = stable_publication_writer(source->global_var(), load->code()->function(), env);
      auto value = core::ConcurrentGlobalEnv::PointerValue::top(size.bit_width(), core::Unsigned);
      if (writer && env.get_publication_pointer(writer, source, value)) {
        eng.data().normal().pointer_assign_nondet(lhs.var());
        eng.data().normal().pointer_refine(lhs.var(), value);
        eng.data().normal().mem_write(ptr.var(), lhs, size);
        eng.data().normal().scalar_forget(lhs.var());
        return;
      }
    }
  }
  // Initialization precedes every worker invocation. Keep ALL later writes;
  // replacing absent local heap memory does not select an initializer's value.
  if (sources.size() == 1 && offset && lhs.var() != ptr.var() &&
      size.to_z_number() == core::ZNumber(eng.data_layout().pointers.bit_width / 8) &&
      load->code()->is_function_body() && load->code()->function()->name() != "main") {
    auto* source = dyn_cast< DynAllocMemoryLocation >(*sources.begin());
    bool only_main_root = true;
    for (auto* root : eng.ctx().opts.entry_points) only_main_root &= root->name() == "main";
    if (source && only_main_root && concrete_lock_object(source, env)) {
      auto key = heap_pointer_key(core::IndexableTraits< MemoryLocation* >::index(source),
                                 eng.data().normal().pointer_to_pointer(ptr.var()).offset());
      for (auto* initializer : env.heap_initializers(key)) {
        bool ordered = false;
        if (!env.get_heap_initializer_certificate(initializer, load->code()->function(), ordered)) {
          ordered = worker_created_after(initializer, load->code()->function(), env, true, &eng.ctx());
          env.cache_heap_initializer_certificate(initializer, load->code()->function(), ordered);
        }
        auto value = core::ConcurrentGlobalEnv::PointerValue::top(size.bit_width(), core::Unsigned);
        if (ordered && env.get_heap_pointer(key, value)) {
          eng.data().normal().pointer_assign_nondet(lhs.var());
          eng.data().normal().pointer_refine(lhs.var(), value);
          eng.data().normal().mem_write(ptr.var(), lhs, size);
          eng.data().normal().scalar_forget(lhs.var());
          return;
        }
      }
    }
  }
  auto alternatives = eng.data();
  bool changed = false;
  for (MemoryLocation* source : sources) {
    auto value = core::ConcurrentGlobalEnv::PointerValue::top(size.bit_width(), core::Unsigned);
    bool found = false;
    if (isa< GlobalMemoryLocation >(source)) {
      found = env.get_global_pointer(
          source, value, eng.data().normal().pointer_to_pointer(ptr.var()).offset());
    } else if (isa< DynAllocMemoryLocation >(source)) {
      auto offset = eng.data().normal().pointer_to_pointer(ptr.var()).offset();
      found = env.get_heap_pointer(heap_pointer_key(
          core::IndexableTraits< MemoryLocation* >::index(source), offset), value);
    }
    if (!found) {
      auto* global = dyn_cast< GlobalMemoryLocation >(source);
      if ((global && (!global->global_var()->is_thread_local() || env.is_escaped(source))) ||
          isa< DynAllocMemoryLocation >(source) || env.is_escaped(source)) {
        eng.data().normal().mem_forget_reachable(ptr.var(), size);
        return;
      }
      continue;
    }
    if (lhs.var() == ptr.var()) {
      eng.data().normal().mem_forget_reachable(ptr.var(), size);
      return;
    }
    auto observation = eng.data();
    observation.normal().mem_read(lhs, ptr.var(), size);
    if (value.leq(observation.normal().pointer_to_pointer(lhs.var()))) continue;
    auto alternative = eng.data();
    alternative.normal().pointer_assign_nondet(lhs.var());
    alternative.normal().pointer_refine(lhs.var(), value);
    alternative.normal().mem_write(ptr.var(), lhs, size);
    alternative.normal().scalar_forget(lhs.var());
    alternatives.join_with(alternative);
    changed = true;
  }
  if (changed) eng.data().join_with(alternatives);
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

/// \brief Remove direct heap targets from the fresh set on publication.
/// TODO: publication must also cover transitively reachable objects.
template < typename E >
void unfresh_pointer_targets(E& eng, const ScalarLit& rhs) {
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
                        ar::Type* written_type,
                        const ScalarLit& ptr,
                        const Literal& val) {
  if (val.is_scalar() && val.scalar().is_pointer_var()) {
    eng.ctx().concurrent_env.record_possible_escape(
        eng.data().normal().pointer_to_points_to(val.scalar().var()));
  }
  auto points_to = eng.data().normal().pointer_to_points_to(ptr.var());
  if (!points_to.is_top() && !points_to.is_bottom()) {
    auto& env = eng.ctx().concurrent_env;
    auto held_mutexes = eng.lockset().held_mutexes();
    const bool has_locks = !held_mutexes.empty();

    for (MemoryLocation* loc : points_to) {
      if (isa< GlobalMemoryLocation >(loc) || isa< DynAllocMemoryLocation >(loc)) {
        const bool complete_pointer = written_type->is_pointer() && val.is_scalar() &&
            (val.scalar().is_pointer_var() || val.scalar().is_null());
        env.record_pointer_write(
            core::IndexableTraits< MemoryLocation* >::index(loc),
            eng.data().normal().pointer_to_pointer(ptr.var()).offset(),
            eng.data_layout().store_size_in_bytes(written_type), complete_pointer);
      }
      // Heap-field pointer propagation (09-regions_03-list2_rc.c FN):
      // a DynAlloc field (`A->next = p`) is invisible to the global-only
      // boards, so a cross-thread deref through it loses the datum
      // access. Propagate pointer-typed writes onto the heap pointer
      // board keyed by (alloc-site, field-offset); unknown offsets overlap
      // all fields of the allocation.
      if (isa< DynAllocMemoryLocation >(loc)) {
        if (val.is_scalar()) {
          const ScalarLit& rhs = val.scalar();
          if (rhs.is_pointer_var() || rhs.is_null()) {
            auto store_ptr =
                eng.data().normal().pointer_to_pointer(ptr.var());
            auto rhs_ptr = rhs.is_null()
                ? core::ConcurrentGlobalEnv::PointerValue::null(
                      eng.ctx().bundle->data_layout().pointers.bit_width, core::Unsigned)
                : eng.data().normal().pointer_to_pointer(rhs.var());
            auto key = heap_pointer_key(
                static_cast< std::uint64_t >(
                    core::IndexableTraits< MemoryLocation* >::index(loc)),
                store_ptr.offset());
            env.join_heap_pointer(key, rhs_ptr);
            // Heap-node publication: if this heap node (the store destination)
            // is NOT fresh (i.e. it is already shared), then storing `rhs`
            // into its field publishes `rhs`'s targets too (`A->next = p` with
            // A shared ⟹ p shared). A store into a still-fresh node publishes
            // nothing (both stay thread-private).
            if (!eng.lockset().is_heap_node_fresh(
                    core::IndexableTraits< MemoryLocation* >::index(loc))) {
              if (rhs.is_pointer_var()) unfresh_pointer_targets(eng, rhs);
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
          env.join_global_pointer(gv, rhs_ptr);
          // A store into a GLOBAL publishes `rhs`'s heap nodes.
          unfresh_pointer_targets(eng, rhs);
        } else if (rhs.is_null() && written_type->is_pointer()) {
          env.join_global_pointer(gv, core::ConcurrentGlobalEnv::PointerValue::null(
              eng.ctx().bundle->data_layout().pointers.bit_width, core::Unsigned));
        }
      }

      if (auto* type = dyn_cast< ar::IntegerType >(gv->global_var()->type()->pointee())) {
        auto* written = dyn_cast< ar::IntegerType >(written_type);
        auto offset = eng.data().normal().pointer_to_pointer(ptr.var()).offset();
        // This board describes a whole scalar, not its starting byte. A
        // partial/shifted write cannot be modeled by numeric sign extension.
        if (written == nullptr || written->bit_width() != type->bit_width() ||
            offset.is_bottom() || offset.is_top() || offset.lb() != offset.ub() || !offset.lb().is_zero()) {
          val_interval = core::machine_int::Interval::top(type->bit_width(), type->sign());
        } else {
          val_interval = val_interval.cast(type->bit_width(), type->sign());
        }
      }

      if (!has_locks) {
        // Queue unlocked writes for publication at the iteration boundary.
        env.record_privatized_write(
            ikos::core::ConcurrentGlobalEnv::kNoLockPartitionAddr, addr,
            val_interval);
        if (log::is_enabled_for(LogLevel::Debug)) {
          log::msg() << "[Strict Flush] Deferred unlocked write to global: "
                     << var_name << " (val_interval=" << val_interval << ")"
                     << "\n";
        }
      } else {
        // Queue under every held lock; no variable protection set is consulted.
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

// Byte operations bypass Store/Load hooks. Only a complete scalar global
// write has a typed summary; other layouts need a property coverage boundary.
template < typename E >
ar::Type* byte_effect_type(E& eng, const ScalarLit& dest, const ScalarLit& size) {
  auto points = eng.data().normal().pointer_to_points_to(dest.var());
  auto length = size.is_machine_int()
      ? core::machine_int::Interval(size.machine_int())
      : eng.data().normal().int_to_interval(size.var());
  auto offset = eng.data().normal().pointer_to_pointer(dest.var()).offset();
  if (!points.singleton() || !offset.singleton() || !offset.lb().is_zero() ||
      !length.singleton()) return nullptr;
  auto* global = dyn_cast< GlobalMemoryLocation >(*points.singleton());
  if (global == nullptr) return nullptr;
  auto* type = global->global_var()->type()->pointee();
  if (!(type->is_integer() || type->is_pointer()) ||
      length.lb().to_z_number() != eng.data_layout().store_size_in_bytes(type))
    return nullptr;
  return type;
}

template < typename E >
bool byte_source_is_shared(E& eng, const ScalarLit& src) {
  auto points = eng.data().normal().pointer_to_points_to(src.var());
  if (points.is_top()) return true;
  if (!points.is_set()) return false;
  for (MemoryLocation* loc : points) {
    auto* global = dyn_cast< GlobalMemoryLocation >(loc);
    if ((global && !global->global_var()->is_thread_local()) ||
        isa< DynAllocMemoryLocation >(loc) || eng.ctx().concurrent_env.is_escaped(loc))
      return true;
  }
  return false;
}

template < typename E >
void publish_byte_effect(E& eng, const ScalarLit& dest, ar::Type* type) {
  // Reuse one projected scalar, avoiding a fresh permanent shadow per replay.
  auto points = eng.data().normal().pointer_to_points_to(dest.var());
  auto name = "concurrency.byte.effect." + std::to_string(
      core::IndexableTraits< MemoryLocation* >::index(*points.singleton()));
  Variable* tmp = eng.ctx().var_factory->get_named_shadow(type, name);
  auto rhs = type->is_pointer() ? ScalarLit::pointer_var(tmp)
                               : ScalarLit::machine_int_var(tmp);
  auto size = core::MachineInt(eng.data_layout().store_size_in_bytes(type),
                              eng.data_layout().pointers.bit_width, core::Unsigned);
  eng.data().normal().mem_read(rhs, dest.var(), size);
  on_store_privatize(eng, type, dest, Literal(rhs));
  eng.data().normal().scalar_forget(tmp);
}

// Only direct allocation addresses can exclude consumption of copied bytes.
inline ar::Value* direct_byte_object(ar::Value* value) {
  for (unsigned depth = 0; depth != 32; ++depth) {
    if (isa< ar::LocalVariable >(value) || isa< ar::GlobalVariable >(value)) return value;
    auto* variable = dyn_cast< ar::InternalVariable >(value);
    if (!variable || !variable->type()->is_pointer()) return nullptr;
    if (auto* function = variable->code()->function_or_null()) {
      for (auto it = function->param_begin(); it != function->param_end(); ++it)
        if (*it == variable) return nullptr;
    }
    auto* definition = unique_def(variable);
    if (auto* copy = dyn_cast_or_null< ar::Assignment >(definition)) {
      value = copy->operand();
    } else if (auto* cast = dyn_cast_or_null< ar::UnaryOperation >(definition)) {
      if (cast->op() != ar::UnaryOperation::Bitcast) return nullptr;
      value = cast->operand();
    } else if (auto* shift = dyn_cast_or_null< ar::PointerShift >(definition)) {
      value = shift->pointer();
    } else return nullptr;
  }
  return nullptr;
}

// Reject every unproved pointer/aggregate consumer or byte transport. Content
// havoc is still required: this certificate excludes new pointer edges only.
inline bool byte_copy_cannot_expose_pointers(
    ar::Bundle* bundle, ar::CallBase* copy,
    const core::PointsToSet< MemoryLocation* >& points) {
  // Constant stores can transport information through their control flow.
  // Treat their entire allocations as additional possible representations.
  std::unordered_set< ar::Value* > representations;
  unsigned budget = 8192;
  for (auto it = bundle->function_begin(); it != bundle->function_end(); ++it) {
    auto* code = (*it)->body_or_null();
    if (!code) continue;
    for (auto* block : *code) for (auto* statement : *block) {
      if (budget-- == 0) return false;
      auto* store = dyn_cast< ar::Store >(statement);
      if (!store || store->value()->type()->is_pointer()) continue;
      auto* source = store->has_frontend()
          ? llvm::dyn_cast< llvm::Instruction >(store->frontend< llvm::Value >()) : nullptr;
      if (source && source->getMetadata("ikos.freeze_uninit")) {
        auto* variable = dyn_cast< ar::InternalVariable >(store->value());
        auto* seed = variable ? dyn_cast_or_null< ar::CallBase >(unique_def(variable)) : nullptr;
        auto* target = seed ? dyn_cast< ar::FunctionPointerConstant >(seed->called()) : nullptr;
        if (target && target->function()->intrinsic_id() == ar::Intrinsic::IkosNonDet &&
            seed->num_arguments() == 0 &&
            dyn_cast_or_null< ar::LocalVariable >(direct_byte_object(store->pointer()))) continue;
      }
      if (!isa< ar::Constant >(store->value()) || isa< ar::UndefinedConstant >(store->value())) return false;
      auto* base = direct_byte_object(store->pointer());
      if (!base) return false;
      representations.insert(base);
    }
  }
  auto disjoint = [&](ar::Value* value) {
    if (isa< ar::NullConstant >(value) || isa< ar::FunctionPointerConstant >(value)) return true;
    auto* base = direct_byte_object(value);
    if (!base || representations.count(base)) return false;
    for (MemoryLocation* loc : points) {
      if (auto* global = dyn_cast< GlobalMemoryLocation >(loc)) {
        if (base == global->global_var()) return false;
      } else if (auto* local = dyn_cast< LocalMemoryLocation >(loc)) {
        if (base == local->local_var()) return false;
      }
    }
    return true;
  };
  // Other externs may turn scalar arguments into bytes/pointers (e.g. %p).
  // Defined callees are scanned; the small primitive set has no such output.
  auto covered_call = [&](ar::CallBase* call) {
    auto* target = dyn_cast< ar::FunctionPointerConstant >(call->called());
    if (!target) return false;
    auto* function = target->function();
    if (function->is_definition()) return true;
    switch (function->intrinsic_id()) {
      case ar::Intrinsic::IkosNonDet:
      case ar::Intrinsic::IkosAssert:
      case ar::Intrinsic::IkosAssume:
      case ar::Intrinsic::LibcMalloc:
      case ar::Intrinsic::LibcAbort:
      case ar::Intrinsic::PthreadCreate:
      case ar::Intrinsic::PthreadJoin:
      case ar::Intrinsic::PthreadMutexLock:
      case ar::Intrinsic::PthreadMutexUnlock: return true;
      case ar::Intrinsic::LibcStrcpy:
      case ar::Intrinsic::LibcStrcpyCheck: return true;
      default: break;
    }
    if (function->num_parameters() != call->num_arguments() ||
        function->type()->is_var_arg()) return false;
    for (std::size_t i = 0; i < call->num_arguments(); ++i)
      if (function->type()->param_type(i) != call->argument(i)->type()) return false;
    auto* returned = dyn_cast< ar::IntegerType >(function->type()->return_type());
    if (returned && returned->bit_width() == 32 && returned->is_signed()) {
      const auto& name = function->name();
      if (name == "pthread_create" && call->num_arguments() == 4) {
        auto* start = dyn_cast< ar::PointerType >(call->argument(2)->type());
        return call->argument(0)->type()->is_pointer() && call->argument(1)->type()->is_pointer() &&
               start && start->pointee()->is_function() && call->argument(3)->type()->is_pointer();
      }
      if (name == "pthread_join" && call->num_arguments() == 2) {
        auto* handle = dyn_cast< ar::IntegerType >(call->argument(0)->type());
        return call->argument(1)->type()->is_pointer() &&
               (call->argument(0)->type()->is_pointer() ||
                (handle && handle->bit_width() == bundle->data_layout().pointers.bit_width));
      }
      if ((name == "pthread_mutex_lock" || name == "pthread_mutex_unlock") &&
          call->num_arguments() == 1) return call->argument(0)->type()->is_pointer();
    }
    if (function->name() != "__assert_fail" || call->num_arguments() != 4 ||
        !function->type()->return_type()->is_void()) return false;
    for (auto i : {0u, 1u, 3u}) {
      auto* pointer = dyn_cast< ar::PointerType >(call->argument(i)->type());
      auto* byte = pointer ? dyn_cast< ar::IntegerType >(pointer->pointee()) : nullptr;
      if (!byte || byte->bit_width() != 8) return false;
    }
    auto* line = dyn_cast< ar::IntegerType >(call->argument(2)->type());
    return line && line->bit_width() == 32;
  };
  auto scan = [&](ar::Code* code) {
    if (!code) return true;
    for (auto* block : *code) for (auto* statement : *block) {
      if (budget-- == 0) return false;
      if (auto* cast = dyn_cast< ar::UnaryOperation >(statement)) {
        if (cast->op() == ar::UnaryOperation::UIToPtr ||
            cast->op() == ar::UnaryOperation::SIToPtr) return false;
      } else if (auto* load = dyn_cast< ar::Load >(statement)) {
        auto* type = load->result()->type();
        if ((type->is_pointer() || type->is_aggregate()) && !disjoint(load->operand())) return false;
      } else if (auto* extract = dyn_cast< ar::ExtractElement >(statement)) {
        if (extract->result()->type()->is_pointer()) return false;
      } else if (auto* call = dyn_cast< ar::CallBase >(statement)) {
        if (!covered_call(call)) return false;
        for (std::size_t i = 0; i < call->num_arguments(); ++i) {
          if (call == copy && i == 0) continue;
          auto* arg = call->argument(i);
          if (arg->type()->is_aggregate() ||
              (arg->type()->is_pointer() && !disjoint(arg))) return false;
        }
      }
    }
    return true;
  };
  for (auto it = bundle->function_begin(); it != bundle->function_end(); ++it)
    if (!scan((*it)->body_or_null())) return false;
  for (auto it = bundle->global_begin(); it != bundle->global_end(); ++it)
    if (!scan((*it)->initializer_or_null())) return false;
  return true;
}

// Unknown-length copies havoc every possible destination allocation. Publish
// matching interference instead of retaining typed values or pointer bounds.
// The optional exemption needs an independent whole-storage pointer-use proof.
template < typename E >
bool publish_unknown_byte_effect(E& eng, ar::CallBase* copy, const ScalarLit& dest,
                                 bool cannot_expose_pointers = false) {
  auto points = eng.data().normal().pointer_to_points_to(dest.var());
  if (!points.is_set() || points.is_empty()) return false;
  for (MemoryLocation* loc : points) {
    if (!isa< LocalMemoryLocation >(loc) && !isa< GlobalMemoryLocation >(loc) &&
        !isa< DynAllocMemoryLocation >(loc)) return false;
  }
  auto& env = eng.ctx().concurrent_env;
  auto offset = core::machine_int::Interval::top(
      eng.data_layout().pointers.bit_width, core::Unsigned);
  for (MemoryLocation* loc : points) {
    auto addr = core::IndexableTraits< MemoryLocation* >::index(loc);
    // TOP offset records an unbounded write, including a terminating NUL.
    env.record_pointer_write(addr, offset, core::ZNumber(1), false);
    if (env.has_mutex_value_cell(addr)) env.invalidate_mutex_values();
    if (auto* global = dyn_cast< GlobalMemoryLocation >(loc)) {
      auto value = core::machine_int::Interval::top(64, core::Signed);
      if (auto* type = dyn_cast< ar::IntegerType >(global->global_var()->type()->pointee())) {
        value = core::machine_int::Interval::top(type->bit_width(), type->sign());
      }
      // Noninteger entries are markers for aggregate initializer invalidation.
      env.record_privatized_write(
          core::ConcurrentGlobalEnv::kNoLockPartitionAddr, addr, value);
    }
  }
  // Partial bytes can create a pointer even from a pointer-free source.
  if (!cannot_expose_pointers &&
      !byte_copy_cannot_expose_pointers(eng.ctx().bundle, copy, points))
    env.record_possible_escape(core::PointsToSet< MemoryLocation* >::top());
  return true;
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
      if (fpc->function() && fpc->function()->is_definition()) {
        thread_func = fpc->function();
        thread_func_name = fpc->function()->name();
        spawn_resolved = true;
        eng.ctx().concurrent_env.register_thread_func(
            fpc->function(), create_site, call);
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
    // Missing child accesses cannot be repaired by a TOP spawned digest.
    eng.ctx().concurrent_env.record_unmodeled_thread_creation(call);
  }

  // Thread-instance id of this create site (0 when the spawn was indirect /
  // the thread function could not be resolved to a constant). Stored into the
  // pthread_t handle below so pthread_join can read it back.
  std::uint64_t sid = 0;
  if (spawn_resolved) {
    sid = eng.ctx().concurrent_env.site_id(create_site);
    eng.lockset().add_spawned_instance(sid);
    eng.lockset().add_live_child(sid);
    // Join-then-create HB: record the parent's MUST joined digest at this
    // create point so the child's entry can inherit it. A parent join of T
    // before this create means T terminated before the child — the child's
    // accesses happen-after T's (bigshot_s: create t1, join t1, create t2).
    auto joined = eng.lockset().get_joined_threads();
    eng.ctx().concurrent_env.record_spawn_joined(
        thread_func, create_site, /*parent_top=*/joined.empty(), joined);
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
        auto native = eng.ctx().native_handle_accesses.find(call);
        if (native != eng.ctx().native_handle_accesses.end()) {
          if (native_handle_range(eng, call, handle_arg)) {
            auto* tmp = eng.ctx().var_factory->get_named_shadow(native->second.type, "concurrency.native.handle");
            eng.data().normal().scalar_assign_nondet(tmp);
            eng.data().normal().mem_write(scalar_lit.var(), ScalarLit::machine_int_var(tmp),
                core::MachineInt(4, eng.data_layout().pointers.bit_width, core::Unsigned));
            eng.data().normal().scalar_forget(tmp);
          } else {
            eng.data().normal().mem_forget(eng.ctx().mem_factory->get_local(native->second.storage));
            eng.ctx().concurrent_env.record_unmodeled_effect(call, "native pthread handle storage range not covered");
          }
        } else {
          if (uncovered_mutex_value_write(eng, handle_arg, scalar_lit)) {
            eng.ctx().concurrent_env.record_unmodeled_effect(
                call, "pthread_create handle output effects not covered");
          }
          auto pts = eng.data().normal().pointer_to_points_to(scalar_lit.var());
          if (pts.is_top()) {
            // Local provenance excludes global writes, not stale local contents.
            if (auto* local = local_output_base(handle_arg))
              eng.data().normal().mem_forget(eng.ctx().mem_factory->get_local(local));
          }
          auto* pointee = cast< ar::PointerType >(handle_arg->type())->pointee();
          auto* type = dyn_cast< ar::IntegerType >(pointee);
          bool supported_bases = pts.is_set() && !pts.is_empty();
          if (pts.is_set()) for (auto* location : pts)
            supported_bases &= isa< LocalMemoryLocation >(location) ||
                               isa< GlobalMemoryLocation >(location);
          if (supported_bases && sid != 0 && type != nullptr) {
            const auto width = eng.data_layout().store_size_in_bytes(type);
            auto offset = eng.data().normal().pointer_to_pointer(scalar_lit.var()).offset();
            // Origins are monotone MAY observations. Current contents still come
            // from memory; non-identity writes retain the coverage gate above.
            for (auto* location : pts)
              eng.ctx().concurrent_env.record_synthetic_handle_slot(
                  location, sid, offset.singleton() ? offset.lb().to_z_number()
                                                   : core::ZNumber(0),
                  width, bool(offset.singleton()));
            // One write through the actual pointer preserves may-alias weak
            // updates and targets scalar subobjects rather than the whole array.
            eng.data().normal().mem_write(
                scalar_lit.var(),
                core::Literal< Variable*, MemoryLocation* >::machine_int(
                    core::MachineInt(sid, type->bit_width(), type->sign())),
                core::MachineInt(width, eng.data_layout().pointers.bit_width,
                                 core::Unsigned));
          } else if (pts.is_set()) {
            // An unresolved create supplies initialized, unknown handle bytes.
            for (auto* location : pts) eng.data().normal().mem_forget(location);
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
            eng.data().normal().pointer_offset_to_interval(scalar_lit.var());
        bool may_numeric_nonnull = pts.is_top() ||
            (pts.is_set() && pts.contains(eng.ctx().mem_factory->get_absolute_zero()) &&
             !(offset.singleton() && offset.lb().is_zero()));
        // PointerAbsValue normalizes NULL's offset to bottom. Transport the
        // raw components, and include numeric zero for every possible NULL.
        if (!eng.data().normal().nullity_is_non_null(scalar_lit.var())) {
          auto zero = core::machine_int::Interval(core::MachineInt::zero(
              offset.bit_width(), offset.sign()));
          auto null_pts = decltype(pts){eng.ctx().mem_factory->get_absolute_zero()};
          if (eng.data().normal().nullity_is_null(scalar_lit.var())) {
            pts = null_pts;
            offset = zero;
            may_numeric_nonnull = false;
          } else {
            pts.join_with(null_pts);
            offset.join_with(zero);
          }
        }
        eng.ctx().concurrent_env.record_spawn_arg(
            thread_func, pts, offset, may_numeric_nonnull);
        // Passing a pointer as the thread `arg` PUBLISHES its heap nodes: the
        // child dereferences the same cell (`&is[i]` escapes to the thread),
        // so they must leave the fresh set (per-thread-array-init-race FN).
        unfresh_pointer_targets(eng, scalar_lit);
      } else if (scalar_lit.is_null()) {
        eng.ctx().concurrent_env.record_spawn_arg(
            thread_func,
            core::PointsToSet< MemoryLocation* >{
                eng.ctx().mem_factory->get_absolute_zero()},
            core::machine_int::Interval(core::MachineInt::zero(
                eng.data_layout().pointers.bit_width, core::Unsigned)), false);
      }
    }
  }
}

} // end namespace analyzer
} // end namespace ikos
