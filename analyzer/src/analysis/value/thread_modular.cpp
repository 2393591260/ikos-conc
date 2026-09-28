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
#include <memory>
#include <vector>

#include <ikos/analyzer/analysis/context.hpp>
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
    auto& env = _ctx.concurrent_env;
    if (func->num_parameters() < 1 ||
        !env.get_spawn_arg(func, pts, offset) ||
        !pts.is_set() || pts.size() == 0) {
      return;
    }
    const auto& a0 = _ctx.lit_factory->get_scalar(func->param(0));
    if (!a0.is_pointer_var()) {
      return;
    }
    // Bind the formal to the recorded points-to set WITH its byte offset
    // (container_of field-recovery: `&s->f` must arrive as base+offset, not
    // base alone — race-2_3b-container_of.c FN). `pointer_refine(p, addrs,
    // offset)` is the existing offset-carrying primitive.
    entry_inv.first().normal().pointer_refine(a0.var(), pts, offset);
  };

  // Thread-local semantics (Mukherjee SAS'17): materialize the cross-thread
  // integer interference ONCE at function entry, by joining the flat
  // `_global_board` into the thread's LOCAL invariant. This replaces the
  // unsound per-read override (`int_set(lhs, injected)` in exec(Load)) that
  // narrowed a locally-widened global counter back to a stale singleton peek
  // and collapsed the loop exit to bottom (fib_*-racy.i FN). Joining at entry
  // is monotone (⊔), so it is sound and never narrows.
  auto materialize_globals = [&](AbstractDomain& entry_inv) {
    auto& env = _ctx.concurrent_env;
    auto board = env.snapshot_global_board();
    if (board.empty()) {
      return;
    }
    const ar::DataLayout& dl = bundle->data_layout();
    const std::uint64_t ptr_bw = dl.pointers.bit_width;
    for (auto it = bundle->global_begin(), et = bundle->global_end(); it != et;
         ++it) {
      ar::GlobalVariable* gv = *it;
      ar::Type* ty = gv->type()->pointee();
      if (!gv->is_definition() || !ty->is_integer()) {
        continue;
      }
      auto* int_ty = ar::cast< ar::IntegerType >(ty);
      MemoryLocation* loc = _ctx.mem_factory->get_global(gv);
      std::uint64_t addr = core::IndexableTraits< MemoryLocation* >::index(loc);
      auto bit = board.find(addr);
      if (bit == board.end()) {
        continue;
      }
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

  // ── Phase 3: Thread-modular worklist fixpoint ──────────────────────
  log::info("Starting global Worklist fixpoint iteration");

  std::vector< ar::Function* > analyzed_functions;
  std::vector< ar::Function* > worklist;

  // Seed worklist with entry points.
  for (ar::Function* ep : _ctx.opts.entry_points) {
    if (ep->is_definition()) {
      worklist.push_back(ep);
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
    // Reset spawn counters: each iteration replays pthread_create, so
    // the per-name counts must restart from zero (see reset_spawn_counts).
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
      // before exiting (main joins t_benign, t_benign joins t_fun ⇒ main
      // transitively joins t_fun — 51-threadjoins/01-trivial.c). Overwritten
      // each iteration; converges with the fixpoint.
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

    // ── Strict unlock-flushing boundary (AstréeA) ────────────────────
    // Drain the sentinel "no_lock" partition at iteration end so all
    // unlocked writes accumulated during this iteration's analysis leak
    // into `_global_board` atomically. This replaces the per-store
    // `env.join_global(addr, val)` that previously fired on every
    // unlocked write inside the worklist body (the "unconditional flush"
    // the user wanted to disable).
    //
    // The drain's return value sets `_is_dirty` via `join_global_nolock`
    // if any cell expanded, so the dirty-check below naturally triggers
    // another iteration if state actually changed. Writes accumulated
    // during iteration N are visible starting iteration N+1.
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

  // Insert all functions in the database.
  for (auto it = bundle->function_begin(), et = bundle->function_end();
       it != et;
       ++it) {
    _ctx.output_db->functions.insert(*it);
  }
}

} // end namespace value
} // end namespace analyzer
} // end namespace ikos