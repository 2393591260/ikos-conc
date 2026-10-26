/*******************************************************************************
 *
 * \file
 * \brief Thread-Modular value analysis driver (plugin-style)
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
 * THE UNITED STATES GOVERNMENT, ITS CONTRACTORS AND SUBCONTRACTORS, AS WELL AS
 * ANY PRIOR RECIPIENT.  IF RECIPIENT'S USE OF THE SUBJECT SOFTWARE RESULTS IN
 * ANY LIABILITIES, DEMANDS, DAMAGES, EXPENSES OR LOSSES ARISING FROM SUCH USE,
 * INCLUDING ANY DAMAGES FROM PRODUCTS BASED ON, OR RESULTING FROM, RECIPIENT'S
 * USE OF THE SUBJECT SOFTWARE, RECIPIENT SHALL INDEMNIFY AND HOLD HARMLESS THE
 * UNITED STATES GOVERNMENT, ITS CONTRACTORS AND SUBCONTRACTORS, AS WELL AS ANY
 * PRIOR RECIPIENT, TO THE EXTENT PERMITTED BY LAW.  RECIPIENT'S SOLE REMEDY
 * FOR ANY SUCH MATTER SHALL BE THE IMMEDIATE, UNILATERAL TERMINATION OF THIS
 * AGREEMENT.
 *
 ******************************************************************************/

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <vector>

#include <ikos/analyzer/analysis/context.hpp>
#include <ikos/analyzer/analysis/runtime_stdio_frame.hpp>
#include <ikos/analyzer/analysis/pthread_initialization_frame.hpp>
#include <ikos/analyzer/analysis/posix_semaphore.hpp>
#include <ikos/analyzer/analysis/posix_barrier.hpp>
#include <ikos/analyzer/analysis/execution_engine/protected_values.hpp>
#include <ikos/analyzer/analysis/execution_engine/heap_pointer_initialization.hpp>
#include <ikos/analyzer/analysis/value/abstract_domain.hpp>
#include <ikos/analyzer/analysis/value/global_variable.hpp>
#include <ikos/analyzer/analysis/value/interprocedural/init_invariant.hpp>
#include <ikos/analyzer/analysis/value/interprocedural/sequential/function_fixpoint.hpp>
#include <ikos/analyzer/analysis/value/interprocedural/sequential/global_init_fixpoint.hpp>
#include <ikos/analyzer/analysis/value/interprocedural/sequential/progress.hpp>
#include <ikos/analyzer/analysis/pointer/function.hpp>
#include <ikos/analyzer/analysis/value/thread_modular.hpp>
#include <ikos/analyzer/checker/checker.hpp>
#include <ikos/analyzer/util/demangle.hpp>
#include <ikos/analyzer/util/log.hpp>
#include <ikos/analyzer/util/progress.hpp>
#include <ikos/analyzer/util/timer.hpp>

#include <ikos/core/domain/concurrent_global_env.hpp>
#include <ikos/analyzer/checker/thread_argument_windows.hpp>

namespace ikos {
namespace analyzer {
namespace value {

ThreadModularAnalysis::ThreadModularAnalysis(Context& ctx,
                                             bool emit_concurrency_invariants)
    : _ctx(ctx),
      _emit_concurrency_invariants(emit_concurrency_invariants) {}

ThreadModularAnalysis::~ThreadModularAnalysis() = default;

void ThreadModularAnalysis::dump_global_blackboard() const {
  _ctx.concurrent_env.dump(log::msg().stream());
}

void ThreadModularAnalysis::run() {
  namespace seq = interprocedural::sequential;

  ar::Bundle* bundle = _ctx.bundle;
  // Driver options and native/AR input are now fixed. Each preflight Context
  // configures its own certificate, before any scan can publish MAY escape.
  Timer preparation;
  preparation.start();
  configure_runtime_stdio_frame(_ctx);
  configure_pthread_initialization_frame(_ctx);
  configure_native_thread_handles(_ctx);
  configure_posix_semaphore(_ctx);
  configure_posix_barrier(_ctx);
  preparation.stop();
  _ctx.output_db->times.insert("ikos-analyzer.prepare.library-frames",
                               preparation.elapsed().count());
  preparation.start();
  const bool preflight_enabled = _ctx.opts.use_checks &&
      std::find(_ctx.opts.analyses.begin(), _ctx.opts.analyses.end(),
                CheckerName::DataRace) != _ctx.opts.analyses.end();
  std::string preflight_status = preflight_enabled ? "missing-premise" : "not-enabled";
  std::size_t requested = 0, established = 0, published = 0;

  // Certify only opaque mutex effects in an independent, conservative run.
  // Its memory, coverage flags, threads and caches never enter the real run.
  if (preflight_enabled && _ctx.opts.entry_points.size() == 1 &&
      _ctx.opts.entry_points.front()->name() == "main" &&
      _ctx.opts.entry_points.front()->num_parameters() == 0 &&
      _ctx.opts.no_init_globals.empty() &&
      !bundle->global_or_null("ar.global_ctors") &&
      !bundle->global_or_null("ar.global_dtors")) {
    preflight_status = "not-applicable";
    std::map< ar::CallBase*, GlobalArraySubobject > queries;
    std::map< ar::Statement*, GlobalArraySubobject > bounds;
    std::vector< GlobalArraySubobject > nested;
    for (auto it = bundle->global_begin(); it != bundle->global_end(); ++it)
      for (const auto& object : global_array_subobjects(*it))
        if (object.nested() && object.type->element_type()->is_pointer()) nested.push_back(object);
    bool closed = true, constructor = false, scan_exhausted = false;
    unsigned budget = 8192;
    for (auto it = bundle->function_begin(); it != bundle->function_end(); ++it) {
      if (!(*it)->is_definition()) continue;
      for (auto* block : *(*it)->body()) for (auto* statement : *block) {
        if (budget == 0) { closed = false; scan_exhausted = true; break; }
        --budget;
        if (auto* cast = dyn_cast< ar::UnaryOperation >(statement)) {
          if (cast->op() == ar::UnaryOperation::UIToPtr ||
              cast->op() == ar::UnaryOperation::SIToPtr ||
              cast->op() == ar::UnaryOperation::PtrToUI ||
              cast->op() == ar::UnaryOperation::PtrToSI)
            closed &= zero_based_layout_projection(cast);
        }
        for (const auto& object : nested) {
          llvm::Value* index = nullptr;
          if (pointer_array_access(statement, object, &index)) {
            closed &= pure_local_array_index(index);
            bounds.emplace(statement, object);
          }
        }
        auto* call = dyn_cast< ar::CallBase >(statement);
        if (!call) continue;
        auto* exact = dyn_cast< ar::FunctionPointerConstant >(call->called());
        auto* source = call->has_frontend()
            ? llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >()) : nullptr;
        auto* source_callee = source ? llvm::dyn_cast< llvm::Function >(
            source->getCalledOperand()->stripPointerCasts()) : nullptr;
        if ((exact && (exact->function()->name() == "pthread_create" ||
                       exact->function()->intrinsic_id() == ar::Intrinsic::PthreadCreate)) ||
            (source_callee && source_callee->getName() == "pthread_create"))
          closed &= _ctx.native_handle_accesses.count(call) || unread_private_thread_output(call);
        if (auto object = mutex_array_effect_object(call)) {
          queries.emplace(call, object);
          continue;
        }
        if (!_ctx.native_handle_accesses.count(call) && !heap_pointer_call_covered(call, &_ctx)) closed = false;
        auto* target = dyn_cast< ar::FunctionPointerConstant >(call->called());
        constructor |= target &&
            target->function()->intrinsic_id() == ar::Intrinsic::LibcMalloc;
      }
    }
    const bool native_queries = _ctx.native_handles_only && !_ctx.native_handle_accesses.empty();
    if (closed && ((constructor && !queries.empty()) || native_queries)) {
      auto* source = _ctx.opts.entry_points.front()->frontend< llvm::Function >();
      closed = mutex_effect_pointer_representations_closed(*source->getParent());
    }
    requested = queries.size() + bounds.size() + _ctx.native_handle_accesses.size();
    if (scan_exhausted)
      preflight_status = "internal-bound";
    else if (requested != 0)
      preflight_status = "missing-premise";
    if (closed && ((constructor && !queries.empty()) || native_queries)) {
      auto options = _ctx.opts;
      options.use_checks = false;
      Context preflight(bundle, std::move(options), _ctx.wd, *_ctx.output_db,
                        *_ctx.mem_factory, *_ctx.var_factory, *_ctx.lit_factory,
                        *_ctx.call_context_factory, *_ctx.fixpoint_parameters);
      preflight.liveness = _ctx.liveness;
      preflight.function_pointer = _ctx.function_pointer;
      preflight.pointer = nullptr;
      preflight.memory_independent_ranges = true;
      preflight.native_handle_inventory_checked = true;
      preflight.native_handles_only = _ctx.native_handles_only;
      preflight.native_handle_accesses = _ctx.native_handle_accesses;
      preflight.native_handle_joins = _ctx.native_handle_joins;
      preflight.mutex_effect_queries = queries;
      preflight.subarray_bounds_queries = bounds;
      ThreadModularAnalysis(preflight, false).run();
      const bool stable = !preflight.concurrent_env.is_dirty();
      preflight_status = stable ? "missing-premise" : "invalidated";
      bool complete = stable &&
                      preflight.concurrent_env.unmodeled_thread_creations().empty();
      // Private unread handle slots cannot affect range decisions. Unknown
      // writes can change shared guards, so they must not certify ranges.
      for (const auto& effect : preflight.concurrent_env.unmodeled_effects())
        complete &= effect.second == "synthetic pthread handle representation not modeled";
      for (const auto& query : queries) {
        const bool proved = Context::range_premise(race_rules::Id::RangeMutexEffects, preflight.mutex_effect_ranges, query.first);
        established += proved;
        complete &= proved;
      }
      for (const auto& query : bounds) {
        const bool proved = Context::range_premise(race_rules::Id::RangeSubarray, preflight.subarray_ranges, query.first);
        established += proved;
        complete &= proved;
      }
      for (const auto& query : _ctx.native_handle_accesses) {
        const bool proved = Context::range_premise(race_rules::Id::RangeNativeHandles, preflight.native_handle_ranges, query.first);
        established += proved;
        complete &= proved;
      }
      if (complete) {
        preflight_status = "proved";
        published = requested;
        for (const auto& query : _ctx.native_handle_accesses)
          if (auto* call = dyn_cast< ar::CallBase >(query.first))
            _ctx._native_handle_effect_certificates.insert(call);
        if (native_queries) log::debug("Native pthread handles: complete output-effect preflight");
        _ctx._subarray_bounds_certificates = bounds;
        for (const auto& query : queries)
          _ctx._mutex_effect_certificates.insert(query.first);
        log::debug("Heap pointer effects: mutex-array preflight certified " +
                   std::to_string(queries.size()) + " calls");
      } else {
        log::debug("Heap pointer effects: incomplete mutex-array preflight");
      }
    }
  }

  preparation.stop();
  _ctx.output_db->times.insert("ikos-analyzer.prepare.range-preflight",
                               preparation.elapsed().count());
  if (log::is_enabled_for(LogLevel::Debug))
    log::debug("Cooperation preflight: status=" + preflight_status +
        " scope=pre-publication-certificate role=" +
        (_ctx.memory_independent_ranges ? "memory-independent" : "ordinary") +
        " requested=" + std::to_string(requested) +
        " established=" + std::to_string(established) +
        " published=" + std::to_string(published));

  {
    // Configuration and creation-count prefilters avoid helper allocation
    // before native variables are created.
    Timer window_preparation;
    window_preparation.start();
    _ctx.thread_argument_windows = ThreadArgumentWindows::prepare(_ctx);
    window_preparation.stop();
    if (_ctx.thread_argument_windows)
      _ctx.output_db->times.insert("ikos-analyzer.prepare.thread-argument-windows",
                                  window_preparation.elapsed().count());
  }

  // Create checkers
  std::vector< std::unique_ptr< Checker > > checkers;
  if (_ctx.opts.use_checks) {
    for (CheckerName name : _ctx.opts.analyses) {
      checkers.emplace_back(make_checker(_ctx, name));
    }
  }

  // Initial invariant
  AbstractDomain init_inv = make_initial_abstract_value(_ctx);

  // IPA context binding: bind a thread entry function's first formal
  // parameter to the points-to set of the `arg` actual captured at the
  // pthread_create site (04-mutex_45-escape_rc.c). Without this the `void*`
  // formal stays TOP and any dereference through it loses its target.
  auto bind_spawn_arg = [&](ar::Function* func, AbstractDomain& entry_inv) {
    core::PointsToSet< MemoryLocation* > pts =
        core::PointsToSet< MemoryLocation* >::empty();
    core::machine_int::Interval offset =
        core::machine_int::Interval::bottom(64, core::Signed);
    bool may_numeric_nonnull = true;
    auto& env = _ctx.concurrent_env;
    if (func->num_parameters() < 1 ||
        !env.get_spawn_arg(func, pts, offset, &may_numeric_nonnull) ||
        !pts.is_set() || pts.size() == 0) {
      return;
    }
    const auto& a0 = _ctx.lit_factory->get_scalar(func->param(0));
    if (!a0.is_pointer_var()) {
      return;
    }
    // With no numeric nonzero source, NULL is represented by nullity TOP.
    // Keeping it also as absolute_zero would invent arbitrary numeric aliases
    // when its zero offset is joined with an object's variable field offset.
    if (!may_numeric_nonnull && pts.size() > 1 &&
        entry_inv.first().normal().nullity_to_nullity(a0.var()).is_top()) {
      pts.remove(_ctx.mem_factory->get_absolute_zero());
    }
    // Bind the formal to the recorded points-to set WITH its byte offset
    // (container_of field-recovery: `&s->f` must arrive as base+offset, not
    // base alone — race-2_3b-container_of.c FN). `pointer_refine(p, addrs,
    // offset)` is the existing offset-carrying primitive.
    entry_inv.first().normal().pointer_refine(a0.var(), pts, offset);
    auto* anchor = _ctx.thread_argument_windows
        ? _ctx.thread_argument_windows->anchor(func) : nullptr;
    if (anchor) {
      // Copy only the ghost offset; never strengthen the original formal.
      entry_inv.first().normal().int_assign(anchor, a0.var()->offset_var());
      entry_inv.first().normal().uninit_assign_maybe(anchor);
      entry_inv.first().normal().uninit_refine(anchor, core::Uninitialized::initialized());
    }
  };

  // Join-then-create HB: inherit the parent's MUST joined digest at the create
  // point. A parent that joined T before creating this thread means T has
  // terminated, so this thread's accesses happen-after T's. Intersected over
  // spawn sites by get_spawn_joined (bigshot_s/singleton_with-uninit: `v`).
  auto bind_spawn_joined = [&](ar::Function* func, AbstractDomain& entry_inv) {
    bool top = true;
    std::unordered_set< std::uint64_t > set;
    if (!_ctx.concurrent_env.get_spawn_joined(func, top, set) || top) {
      return; // no resolved spawn, or ⊤ (nothing definitely joined before)
    }
    for (std::uint64_t sid : set) {
      entry_inv.second().add_joined_thread(sid);
    }
  };

  // Join published integer summaries into the function entry memory.
  // Integer loads also join interference after local writes in the executor.
  auto materialize_globals = [&](AbstractDomain& entry_inv) {
    auto& env = _ctx.concurrent_env;
    auto board = env.snapshot_global_interference();
    if (board.empty()) {
      return;
    }
    const ar::DataLayout& dl = bundle->data_layout();
    const std::uint64_t ptr_bw = dl.pointers.bit_width;
    for (auto it = bundle->global_begin(), et = bundle->global_end(); it != et;
         ++it) {
      ar::GlobalVariable* gv = *it;
      ar::Type* ty = gv->type()->pointee();
      if (!gv->is_definition()) {
        continue;
      }
      MemoryLocation* loc = _ctx.mem_factory->get_global(gv);
      std::uint64_t addr = core::IndexableTraits< MemoryLocation* >::index(loc);
      auto bit = board.find(addr);
      if (bit == board.end()) {
        continue;
      }
      if (!ty->is_integer()) {
        if (ty->is_pointer()) continue; // separate pointer-interference channel
        // The flat board has no field offsets. Retaining the static array or
        // struct initializer would discard real writes made before a spawn.
        Variable* ptr_var = _ctx.var_factory->get_global(gv);
        entry_inv.first().normal().pointer_assign(ptr_var, loc, core::Nullity::non_null());
        entry_inv.first().normal().mem_forget_reachable(ptr_var);
        continue;
      }
      auto* int_ty = ar::cast< ar::IntegerType >(ty);
      // Join the board interval into the global's integer CONTENT. The
      // content is a memory CELL, not a scalar, so `int_set` on the pointer
      // variable (or the cell variable) is not applicable — go through the
      // memory domain: read the cell into a fresh scalar, join, write back.
      // The board value may have been normalized to a different width/sign
      // by the blackboard's widening discipline, so cast it to the global's
      // type before joining.
      Signedness sign = int_ty->sign();
      std::uint64_t bit_width = int_ty->bit_width();
      core::machine_int::Interval board_val = bit->second;
      if (board_val.bit_width() != bit_width || board_val.sign() != sign) {
        board_val = board_val.cast(bit_width, sign);
      }
      core::MachineInt size(dl.store_size_in_bytes(int_ty), ptr_bw,
                            core::Unsigned);
      Variable* ptr_var = _ctx.var_factory->get_global(gv);
      // Globals are NOT stored in the entry invariant's points-to — they are
      // initialized on the fly when first used (exec engine
      // `init_global_operand`). Without this, `pointer_to_points_to(ptr_var)`
      // is TOP and `mem_write` would `mem_forget_all()` (blowing the whole
      // invariant to TOP). Pin the pointer to its global cell first, exactly
      // like `init_global_operand`.
      entry_inv.first().normal().pointer_assign(ptr_var, loc, core::Nullity::non_null());
      Variable* tmp = _ctx.var_factory->create_unnamed_shadow(int_ty);
      entry_inv.first().normal().mem_read(
          core::Literal< Variable*, MemoryLocation* >::machine_int_var(tmp),
          ptr_var,
          size);
      auto local = entry_inv.first().normal().int_to_interval(tmp);
      entry_inv.first().normal().int_set(tmp, local.join(board_val));
      entry_inv.first().normal().mem_write(
          ptr_var,
          core::Literal< Variable*, MemoryLocation* >::machine_int_var(tmp),
          size);
    }
  };

  // ── Phase 1: Static initialization of globals ──────────────────────
  {
    log::debug("Computing global variable static initialization");

    GlobalsInitPolicy policy = _ctx.opts.globals_init_policy;

    std::unique_ptr< analyzer::ProgressLogger > logger =
        make_progress_logger(_ctx.opts.progress,
                             LogLevel::Debug,
                             /* num_tasks = */
                             std::count_if(bundle->global_begin(),
                                           bundle->global_end(),
                                           [=](ar::GlobalVariable* gv) {
                                             return gv->is_definition() &&
                                                    is_initialized(gv, policy);
                                           }));
    ScopeLogger scope(*logger);

    for (auto it = bundle->global_begin(), et = bundle->global_end(); it != et;
         ++it) {
      ar::GlobalVariable* gv = *it;
      if (gv->is_definition() && is_initialized(gv, policy)) {
        logger->start_task("Initializing global variable '" +
                           demangle(gv->name()) + "'");
        seq::GlobalVarInitializerFixpoint fixpoint(_ctx, gv);
        fixpoint.run(init_inv);
        init_inv = fixpoint.exit_invariant();
      }
    }
  }

  if (_ctx.opts.display_invariants == DisplayOption::All) {
    LogMessage msg = log::msg();
    msg << "Invariant after global variable static initialization:\n";
    init_inv.dump(msg.stream());
    msg << "\n";
  }

  // ── Phase 2: Global constructors ───────────────────────────────────
  ar::GlobalVariable* gv_ctors = bundle->global_or_null("ar.global_ctors");
  if (gv_ctors != nullptr) {
    log::info("Computing global variable dynamic initialization");

    std::vector< std::pair< ar::Function*, MachineInt > > ctors =
        global_ctors(gv_ctors);

    for (const auto& entry : ctors) {
      ar::Function* ctor = entry.first;

      if (ctor->is_declaration()) {
        log::error("global constructor '" + ctor->name() + "' is extern");
        _ctx.race_entry_obligations.push_back("extern global constructor: " + ctor->name());
        continue;
      }

      std::unique_ptr< seq::ProgressLogger > logger =
          seq::make_progress_logger(_ctx, _ctx.opts.progress, LogLevel::Info);
      ScopeLogger scope(*logger);

      seq::FunctionFixpoint fixpoint(_ctx, checkers, *logger, ctor);

      {
        log::info("Analyzing global constructor '" + demangle(ctor->name()) +
                  "'");
        ScopeTimerDatabase t(_ctx.output_db->times,
                             "ikos-analyzer.value." + ctor->name());
        fixpoint.run(init_inv);
      }

      if (!checkers.empty()) {
        log::info("Checking properties for global constructor '" +
                  demangle(ctor->name()) + "'");
        ScopeTimerDatabase t(_ctx.output_db->times,
                             "ikos-analyzer.check." + ctor->name());
        fixpoint.run_checks();
      }

      init_inv = fixpoint.exit_invariant();
    }

    if (_ctx.opts.display_invariants == DisplayOption::All) {
      LogMessage msg = log::msg();
      msg << "Invariant after global variable dynamic initialization:\n";
      init_inv.dump(msg.stream());
      msg << "\n";
    }
  }

  // Capture I once. Constructors/custom roots need a separate initialization
  // coverage proof before they can use this first-acquisition subset.
  bool ordinary_initialization = gv_ctors == nullptr && _ctx.opts.no_init_globals.empty();
  for (auto* root : _ctx.opts.entry_points) ordinary_initialization &= root->name() == "main";
  // A syntactic zero proof must describe the final initialized state. Refuse
  // this shortcut if another executed initializer could overwrite its owner.
  bool owned_initializers = true;
  for (auto it = bundle->global_begin(); it != bundle->global_end(); ++it) {
    auto* owner = *it;
    if (!owner->is_definition() || !is_initialized(owner, _ctx.opts.globals_init_policy)) continue;
    auto* initializer = owner->initializer_or_null();
    if (!initializer) { owned_initializers = false; continue; }
    for (auto* block : *initializer) for (auto* statement : *block) {
      auto* store = dyn_cast< ar::Store >(statement);
      owned_initializers &= store && store->pointer() == owner &&
          store->value()->type() == owner->type()->pointee() &&
          !store->is_atomic() && !store->is_volatile();
    }
  }
  if (ordinary_initialization && !init_inv.first().normal().is_bottom()) {
    for (auto it = bundle->global_begin(); it != bundle->global_end(); ++it) {
      auto* global = *it;
      if (!global->type()->pointee()->is_integer() &&
          std::getenv("IKOS_DISABLE_MUTEX_FIELD_VALUES")) continue;
      const auto& cells = source_program_facts(_ctx).integer_cells(global);
      if (!protected_integer_accesses(global, cells)) continue;
      for (const auto& cell : cells) {
        auto* type = cell.type;
        auto* location = _ctx.mem_factory->get_global(global);
        auto* pointer = _ctx.var_factory->create_unnamed_shadow(global->type());
        auto* temporary = _ctx.var_factory->create_unnamed_shadow(type);
        auto literal = core::Literal< Variable*, MemoryLocation* >::machine_int_var(temporary);
        auto observation = init_inv.first();
        core::MachineInt size(bundle->data_layout().store_size_in_bytes(type),
                              bundle->data_layout().pointers.bit_width, core::Unsigned);
        observation.normal().pointer_assign(pointer, location, core::Nullity::non_null());
        observation.normal().pointer_assign(pointer, pointer,
            core::MachineInt(cell.offset, size.bit_width(), core::Unsigned));
        observation.normal().mem_read(literal, pointer, size);
        auto initial = observation.normal().int_to_interval(temporary);
        if (initial.is_bottom()) initial.set_to_top();
        _ctx.concurrent_env.initialize_mutex_value(
            core::IndexableTraits< MemoryLocation* >::index(location), initial, cell.offset);
      }
      if (std::getenv("IKOS_DISABLE_MUTEX_FIELD_EQUALITIES")) {
        log::debug("Cooperation equality: status=not-enabled reason=channel-disabled");
        continue;
      }
      // Native memory does not extract subcell values from aggregate_zero.
      // One complete typed zero initializer is also a direct equality proof.
      unsigned blocks = 0, statements = 0;
      ar::Store* zero_store = nullptr;
      if (auto* initializer = global->initializer_or_null())
        for (auto* block : *initializer) {
          ++blocks;
          for (auto* statement : *block) {
            ++statements;
            zero_store = dyn_cast< ar::Store >(statement);
          }
        }
      const bool zero_initialized = owned_initializers &&
          is_initialized(global, _ctx.opts.globals_init_policy) &&
          blocks == 1 && statements == 1 && zero_store &&
          zero_store->pointer() == global && !zero_store->is_atomic() &&
          !zero_store->is_volatile() && isa< ar::AggregateZeroConstant >(zero_store->value()) &&
          zero_store->value()->type() == global->type()->pointee();
      // Finite initial templates; release checks monotonically discard them.
      unsigned pairs = 0;
      for (std::size_t a = 0; a < cells.size(); ++a) {
        for (std::size_t b = a + 1; b < cells.size() && pairs < 64; ++b, ++pairs) {
          if (cells[a].type->bit_width() != cells[b].type->bit_width() ||
              cells[a].type->sign() != cells[b].type->sign()) continue;
          if (!zero_initialized &&
              !protected_cells_equal(init_inv.first(), _ctx, global, cells[a], cells[b])) continue;
          auto* location = _ctx.mem_factory->get_global(global);
          _ctx.concurrent_env.initialize_mutex_equality(
              core::IndexableTraits< MemoryLocation* >::index(location),
              cells[a].offset, cells[b].offset, cells[a].type->bit_width());
          if (log::is_enabled_for(LogLevel::Debug))
            log::debug("Mutex field equality: initialized global=" + global->name() +
                " fields=" + std::to_string(cells[a].offset) + "," +
                std::to_string(cells[b].offset));
        }
      }
    }
  }

  // ── Phase 2.5: Function pointer analysis ────────────────────────────
  // The FPA only runs under Procedural::Intraprocedural in the driver;
  // the thread-modular (concurrency) path needs its results for (a) the
  // indirect-call resolution of thread-entry function pointers stored
  // through global memory (04-mutex_27-base_rc.c) and (b) the FS+FI
  // hybrid join. Run it locally if no earlier pass already did.
  if (_ctx.function_pointer == nullptr) {
    log::info("Running function pointer analysis (thread-modular)");
    auto* fpa = new analyzer::FunctionPointerAnalysis(_ctx);
    fpa->run();
    _ctx.function_pointer = fpa; // process-lifetime; single-shot
  }

  if (ordinary_initialization) _ctx.concurrent_env.enable_mutex_values();

  // ── Phase 3: Thread-modular worklist fixpoint ──────────────────────
  log::info("Starting global Worklist fixpoint iteration");

  std::vector< ar::Function* > analyzed_functions;
  std::vector< ar::Function* > worklist;

  // Seed worklist with entry points, retaining skipped required roots.
  for (ar::Function* ep : _ctx.opts.entry_points) {
    if (ep->is_definition()) {
      worklist.push_back(ep);
    } else {
      _ctx.race_entry_obligations.push_back("extern analysis entry: " + ep->name());
    }
  }

  _ctx.concurrent_env.clear_dirty();

  unsigned global_iteration = 0;

  while (true) {
    // Reset
    global_iteration++;
    if (log::is_enabled_for(LogLevel::Debug)) {
      log::msg() << "========================================" << "\n";
      log::msg() << ">>> [Concurrency] Starting Global Iteration " << global_iteration
                 << "\n";
    }
    // Publish the global iteration to the blackboard's widening schedule
    // (Cousot-Cousot delayed widening: JOIN below kWideningDelay, WIDEN after).
    _ctx.concurrent_env.set_global_iteration(
        global_iteration);
    _ctx.concurrent_env.clear_dirty();
    // Reset ephemeral condition state; persistent may/must spawn summaries
    // survive replay and participate in the coupled fixpoint.
    _ctx.concurrent_env.reset_spawn_counts();
    log::debug("=== Worklist iteration: dirty cleared ===");

    // Execute
    for (ar::Function* func : worklist) {
      analyzed_functions.push_back(func);
      if (log::is_enabled_for(LogLevel::Debug)) {
        log::msg() << ">>> [Concurrency] Pulling from worklist, analyzing thread: "
                   << func->name() << "\n";
      }

      AbstractDomain entry_inv = make_bottom_abstract_value(_ctx);

      if (std::find(_ctx.opts.no_init_globals.begin(),
                    _ctx.opts.no_init_globals.end(),
                    func) == _ctx.opts.no_init_globals.end()) {
        entry_inv = init_inv;
      } else {
        entry_inv = make_initial_abstract_value(_ctx);
      }

      if (func->name() == "main" && func->num_parameters() >= 2) {
        entry_inv = interprocedural::init_main_invariant(_ctx, func, entry_inv);
      }
      bind_spawn_arg(func, entry_inv);
      bind_spawn_joined(func, entry_inv);
      materialize_globals(entry_inv);

      std::unique_ptr< seq::ProgressLogger > logger =
          seq::make_progress_logger(_ctx, _ctx.opts.progress, LogLevel::Info);
      ScopeLogger scope(*logger);

      seq::FunctionFixpoint fixpoint(_ctx, checkers, *logger, func);

      {
        log::info("Analyzing function '" + demangle(func->name()) + "'");
        ScopeTimerDatabase t(_ctx.output_db->times,
                             "ikos-analyzer.value." + func->name());
        fixpoint.run(entry_inv);
      }
      // Record this function's MUST-join summary for transitive join HB: a
      // later pthread_join of this thread must also join everything IT joined
      // before exiting. Changed MUST facts also require another global round.
      _ctx.concurrent_env.set_join_summary(
          func, fixpoint.exit_invariant().second().get_joined_threads());
      // NOTE: run_checks() intentionally omitted here for fixpoint iteration.

      // If the user asked to observe the concurrency invariants, print the
      // exit invariant of this function (with the lockset digest) after
      // every iteration.
      if (_emit_concurrency_invariants) {
        LogMessage inv_msg = log::msg();
        inv_msg << "[Concurrency] exit invariant of " << func->name()
                << " after iteration " << global_iteration << ":\n";
        fixpoint.exit_invariant().dump(inv_msg.stream());
        inv_msg << "\n";
      }
    }

    // Every partition contributes interference, even without a later unlock.
    _ctx.concurrent_env.flush_all_privatized_locks();
    _ctx.concurrent_env.flush_unlocked_partition();

    // Check fixpoint
    if (!_ctx.concurrent_env.is_dirty()) {
      if (log::is_enabled_for(LogLevel::Debug)) {
        log::msg() << "Global fixpoint reached after "
                   << analyzed_functions.size() << " function analyses"
                   << "\n";
      }
      if (_emit_concurrency_invariants) {
        log::msg() << "=== Blackboard at convergence ===\n";
        dump_global_blackboard();
      }
      break;
    }

    // Rebuild worklist
    worklist.clear();
    for (ar::Function* ep : _ctx.opts.entry_points) {
      if (ep->is_definition()) {
        worklist.push_back(ep);
      }
    }
    auto thread_funcs =
        _ctx.concurrent_env.get_all_thread_functions();
    for (ar::Function* tf : thread_funcs) {
      if (tf->is_definition()) {
        worklist.push_back(tf);
      }
    }
    std::sort(worklist.begin(), worklist.end(),
              [](ar::Function* a, ar::Function* b) {
                return a->name() < b->name();
              });
    worklist.erase(std::unique(worklist.begin(), worklist.end()),
                   worklist.end());

    if (log::is_enabled_for(LogLevel::Debug)) {
      log::msg() << "Worklist rebuilt with " << worklist.size()
                 << " functions, continuing iteration" << "\n";
    }
  }

  // Emit concurrency invariants snapshot if requested.
  if (_emit_concurrency_invariants) {
    LogMessage msg = log::msg();
    msg << "=== Final concurrency invariants snapshot ===" << "\n";
    msg << "Converged after " << global_iteration << " global iterations" << "\n";
    dump_global_blackboard();
  }

  // A final replay may discover effects absent from the converged snapshot.
  _ctx.concurrent_env.clear_dirty();

  // ── Phase 4: Post-loop checks ───────────────────────────────────────
  if (!checkers.empty()) {
    log::info("Running post-loop checks for all analyzed functions");

    std::sort(analyzed_functions.begin(), analyzed_functions.end(),
              [](ar::Function* a, ar::Function* b) {
                return a->name() < b->name();
              });
    analyzed_functions.erase(std::unique(analyzed_functions.begin(),
                                        analyzed_functions.end()),
                           analyzed_functions.end());

    for (ar::Function* func : analyzed_functions) {
      AbstractDomain entry_inv = make_bottom_abstract_value(_ctx);

      if (std::find(_ctx.opts.no_init_globals.begin(),
                    _ctx.opts.no_init_globals.end(),
                    func) == _ctx.opts.no_init_globals.end()) {
        entry_inv = init_inv;
      } else {
        entry_inv = make_initial_abstract_value(_ctx);
      }

      if (func->name() == "main" && func->num_parameters() >= 2) {
        entry_inv = interprocedural::init_main_invariant(_ctx, func, entry_inv);
      }
      bind_spawn_arg(func, entry_inv);
      bind_spawn_joined(func, entry_inv);
      materialize_globals(entry_inv);

      std::unique_ptr< seq::ProgressLogger > logger =
          seq::make_progress_logger(_ctx, _ctx.opts.progress, LogLevel::Info);
      seq::FunctionFixpoint fixpoint(_ctx, checkers, *logger, func);

      {
        log::info("Checking properties for function '" +
                  demangle(func->name()) + "'");
        ScopeTimerDatabase t(_ctx.output_db->times,
                             "ikos-analyzer.check." + func->name());
        fixpoint.run(entry_inv);
        // Replay must satisfy the same MUST-summary equation as convergence.
        // A changed result leaves dirty set for the final coverage gate.
        _ctx.concurrent_env.set_join_summary(
            func, fixpoint.exit_invariant().second().get_joined_threads());
        fixpoint.run_checks();
      }
    }
  }

  // ── Phase 5: Global destructors ─────────────────────────────────────
  ar::GlobalVariable* gv_dtors = bundle->global_or_null("ar.global_dtors");
  if (gv_dtors != nullptr) {
    log::info("Analyzing global destructors");

    std::vector< std::pair< ar::Function*, MachineInt > > dtors =
        global_dtors(gv_dtors);

    for (const auto& entry : dtors) {
      ar::Function* dtor = entry.first;

      if (dtor->is_declaration()) {
        log::error("global destructor '" + dtor->name() + "' is extern");
        _ctx.race_entry_obligations.push_back("extern global destructor: " + dtor->name());
        continue;
      }

      std::unique_ptr< seq::ProgressLogger > logger =
          seq::make_progress_logger(_ctx, _ctx.opts.progress, LogLevel::Info);
      ScopeLogger scope(*logger);

      seq::FunctionFixpoint fixpoint(_ctx, checkers, *logger, dtor);

      {
        log::info("Analyzing global destructor '" + demangle(dtor->name()) +
                  "'");
        ScopeTimerDatabase t(_ctx.output_db->times,
                             "ikos-analyzer.value." + dtor->name());
        // Note: We currently analyze destructors with the initial invariant.
        fixpoint.run(init_inv);
      }

      if (!checkers.empty()) {
        log::info("Checking properties for global destructor: '" +
                  demangle(dtor->name()) + "'");
        ScopeTimerDatabase t(_ctx.output_db->times,
                             "ikos-analyzer.check." + dtor->name());
        fixpoint.run_checks();
      }

      init_inv = fixpoint.exit_invariant();
    }
  }

  _ctx.concurrent_env.flush_all_privatized_locks();
  _ctx.concurrent_env.flush_unlocked_partition();
  if (_ctx.concurrent_env.is_dirty() &&
      std::find(_ctx.opts.analyses.begin(), _ctx.opts.analyses.end(),
                CheckerName::DataRace) != _ctx.opts.analyses.end() &&
      _ctx.opts.use_checks) {
    // Stale access records cannot prove SAFE. Reconvergence can replace this
    // conservative boundary when checks are separated from transfer replay.
    ar::Statement* anchor = nullptr;
    for (ar::Function* func : analyzed_functions) {
      for (auto* block : *func->body()) {
        if (!block->empty()) { anchor = block->front(); break; }
      }
      if (anchor != nullptr) break;
    }
    if (anchor != nullptr) {
      _ctx.output_db->checks.insert(
          CheckKind::DataRace, CheckerName::DataRace,
          Result::Warning, anchor, _ctx.call_context_factory->get_empty(), {},
          {{"unknown_reason", "shared summaries changed after convergence"}});
    }
  }

  // Publish only after all replay has finished. Late shared effects invalidate
  // the snapshot; preflight coverage and each consumer's other premises remain
  // separate from these all-access range facts.
  const bool stable_ranges = !_ctx.concurrent_env.is_dirty();
  _ctx.mutex_effect_ranges.finish(stable_ranges);
  _ctx.subarray_ranges.finish(stable_ranges);
  _ctx.native_handle_ranges.finish(stable_ranges);

  // Insert all functions in the database.
  for (auto it = bundle->function_begin(), et = bundle->function_end();
       it != et;
       ++it) {
    _ctx.output_db->functions.insert(*it);
  }

  _ctx.race_analysis_complete = true;
  for (const auto& checker : checkers) {
    checker->finalize();
  }
}

} // end namespace value
} // end namespace analyzer
} // end namespace ikos
