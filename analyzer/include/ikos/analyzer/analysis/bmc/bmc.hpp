// BMC 模块 —— 有界竞争确认（供 checker 集成调用）
//
// confirm_race：展开实际执行前缀，再由 check_race 检查同步、值、地址和
// 实例前提。未支持的操作仅阻止执行到它的前缀，不因整函数扫描否决其他路径。

#pragma once

#include <chrono>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "encode.hpp"
#include "unroll.hpp"
#include "traces.hpp"

namespace ikos {
namespace analyzer {
namespace bmc {

// Optional static capability scan; production confirmation checks executed
// prefixes. Hardware builtins and bodyless named verifier atomics lack summaries.
inline bool is_unmodeled_atomic_call(ar::Function* function) {
  const auto& nm = function->name();
  if (nm.size() >= 7 && nm.compare(0, 7, "__sync_") == 0) {
    return true;
  }
  if (nm.size() >= 9 && nm.compare(0, 9, "__atomic_") == 0) {
    return true;
  }
  // Named definitions are wrapped by the executed-prefix unroller.
  if (function->is_declaration() && nm.compare(0, 18, "__VERIFIER_atomic_") == 0 &&
      nm != "__VERIFIER_atomic_begin" && nm != "__VERIFIER_atomic_end") {
    return true;
  }
  return false;
}

/// 检测一个函数里有没有 BMC 没建模的同步：
///   - C11 原子（atomic_int 等，Load/Store 的 ordering != NotAtomic）
///   - Hardware atomics and bodyless named verifier atomic calls.
///   - sem_*（信号量 post/wait 建 HB）
///   - pthread_barrier_* / pthread_spin_*（屏障/自旋锁）
/// A static summary only; executed-prefix rejection controls confirmation.
/// __VERIFIER_atomic_begin/end use a pseudo mutex. Condition variables and
/// other rwlock APIs currently lack sufficient summaries and remain UNKNOWN.
inline bool has_unmodeled_sync(ar::Function* func,
                               ar::Statement** unsupported = nullptr) {
  ar::Code* body = func->body_or_null();
  if (body == nullptr) {
    return false;
  }
  for (auto bit = body->begin(), bend = body->end(); bit != bend; ++bit) {
    for (auto sit = (*bit)->begin(), send = (*bit)->end(); sit != send; ++sit) {
      ar::Statement* s = *sit;
      if (auto* load = core::dyn_cast< ar::Load >(s)) {
        if (load->is_atomic()) {
          if (unsupported != nullptr) {
            *unsupported = s;
          }
          return true;
        }
      } else if (auto* store = core::dyn_cast< ar::Store >(s)) {
        if (store->is_atomic()) {
          if (unsupported != nullptr) {
            *unsupported = s;
          }
          return true;
        }
      } else if (auto* call = core::dyn_cast< ar::CallBase >(s)) {
        ar::Function* f = exact_callee(call);
        if (f != nullptr) {
          const std::string& nm = f->name();
          if (is_unmodeled_atomic_call(f)) {
            if (unsupported != nullptr) {
              *unsupported = s;
            }
            return true;
          }
          if (nm.find("pthread_barrier") != std::string::npos ||
              nm.find("pthread_spin") != std::string::npos ||
              nm.find("pthread_cond") != std::string::npos ||
              (nm.find("pthread_rwlock") != std::string::npos &&
               !(f->is_declaration() && call->num_arguments() == 1 &&
                 (nm == "pthread_rwlock_rdlock" || nm == "pthread_rwlock_wrlock" || nm == "pthread_rwlock_unlock"))) ||
              nm.rfind("sem_", 0) == 0) {
            if (unsupported != nullptr) {
              *unsupported = s;
            }
            return true;
          }
        }
      }
    }
  }
  return false;
}

/// 静态能力扫描；生产查询检查实际前缀，不用这个扫描否决整个程序。
inline bool has_unmodeled_sync(ar::Bundle* bundle,
                               ar::Statement** unsupported = nullptr) {
  for (auto fit = bundle->function_begin(), fend = bundle->function_end();
       fit != fend; ++fit) {
    if ((*fit)->is_definition() && has_unmodeled_sync(*fit, unsupported)) {
      return true;
    }
  }
  return false;
}

// A search hint, not a trip-count proof: only actual encoded guards certify a
// retained execution. Small literal loop limits include room for the exit test.
inline unsigned literal_loop_search_bound(ar::Function* root) {
  unsigned bound = 16;
  std::set< ar::Function* > visited;
  std::function< void(ar::Function*, unsigned) > scan;
  scan = [&](ar::Function* function, unsigned depth) {
    if (!inline_ok(function) || depth > 8 || visited.size() >= 64 || !visited.insert(function).second) return;
    for (auto* block : *function->body()) {
      for (auto* statement : *block) {
        if (auto* call = core::dyn_cast< ar::CallBase >(statement)) scan(exact_callee(call), depth + 1);
        auto* comparison = core::dyn_cast< ar::Comparison >(statement);
        if (comparison == nullptr) continue;
        switch (comparison->predicate()) {
          case ar::Comparison::SILT: case ar::Comparison::SILE:
          case ar::Comparison::SIGT: case ar::Comparison::SIGE:
          case ar::Comparison::UILT: case ar::Comparison::UILE:
          case ar::Comparison::UIGT: case ar::Comparison::UIGE: break;
          default: continue;
        }
        auto* constant = core::dyn_cast< ar::IntegerConstant >(comparison->right());
        if (constant == nullptr) constant = core::dyn_cast< ar::IntegerConstant >(comparison->left());
        if (constant == nullptr) continue;
        auto value = constant->value().to_z_number();
        if (value < 15 || value > 64) continue;
        std::vector< ar::BasicBlock* > pending;
        for (auto it = block->successor_begin(), end = block->successor_end(); it != end; ++it) pending.push_back(*it);
        std::set< ar::BasicBlock* > seen;
        while (!pending.empty()) {
          auto* current = pending.back(); pending.pop_back();
          if (current == block) {bound = std::max(bound, value.to< unsigned >() + 2); break;}
          if (!seen.insert(current).second) continue;
          for (auto it = current->successor_begin(), end = current->successor_end(); it != end; ++it) pending.push_back(*it);
        }
      }
    }
  };
  scan(root, 0);
  return bound;
}

// A shared budget bounds all groups; exhaustion only leaves races unconfirmed.
struct SearchBudget {
  std::size_t candidates = 256;
  std::size_t queries = 1024;
  bool probe_interrupted = false;
  bool defer_large_loops = false;
  bool saw_interrupted = false;
  const unsigned deadline_ms;
  const unsigned solver_timeout_ms;
  std::chrono::steady_clock::time_point deadline;

  explicit SearchBudget(unsigned deadline_ms = 10000,
                        unsigned solver_timeout_ms = 1000)
      : deadline_ms(deadline_ms),
        solver_timeout_ms(solver_timeout_ms),
        deadline(std::chrono::steady_clock::now() +
                 std::chrono::milliseconds(deadline_ms)) {}

  bool expired() const {
    return std::chrono::steady_clock::now() >= deadline;
  }

  void record_exhaustion(Diagnostics* diagnostics) const {
    if (diagnostics == nullptr) return;
    if (expired()) record(diagnostics, "search_deadline");
    if (candidates == 0) record(diagnostics, "candidate_budget");
    if (queries == 0) record(diagnostics, "query_budget");
  }
};

// Access ordinals are independent of matching-entry create ordinals in facts.
// Legacy search deliberately preserves its historical helper alternatives.
struct AccessTargets {
  unsigned a = 1, b = 1;
  bool exact = false;
};

struct LaterAccessState {
  unsigned limit_a = MAX_PATH_EVENTS, limit_b = MAX_PATH_EVENTS;
  bool pending(unsigned frontier) const {
    return limit_a != 0 && limit_b != 0 &&
           frontier <= std::max(limit_a, limit_b);
  }
};

struct AccessSearchProgress {
  struct Side {
    unsigned maximum = 0;
    bool observed = false, incomplete = false, pruned_join = false;
    bool complete_frontier() const { return observed && !incomplete && !pruned_join; }
  };
  Side a, b;
  void note(const UnrollResult& result, bool side_a) {
    auto& side = side_a ? a : b;
    side.observed = true;
    side.maximum = std::max(side.maximum, result.max_target_occurrence);
    side.incomplete |= result.incomplete;
    side.pruned_join |= result.pruned_join;
  }
};

inline bool atomic_candidate(const std::vector< BmcEvent >& path) {
  int active = 0;
  for (const auto& event : path) {
    if (event.candidate) return active > 0;
    if (event.mutex != PSEUDO_ATOMIC_LOCK) continue;
    if (event.kind == EvKind::Lock) ++active;
    if (event.kind == EvKind::Unlock) --active;
  }
  return false;
}

/// 确认两个访问（在各自线程入口函数里）是否真竞争。
/// funcA/funcB = 两访问所在的线程入口函数；stmtA/stmtB = 两访问语句；
/// widthA/widthB = 两访问的字节宽度（data_layout 的 store size，0=未知），
/// 供 encode 阶段用字节范围重叠判同址。
/// Structured bounded-search outcome; failures keep the checker inconclusive.
inline Result confirm_race(ar::Function* funcA, ar::Statement* stmtA,
                         std::uint64_t widthA, ar::Function* funcB,
                         ar::Statement* stmtB, std::uint64_t widthB,
                         Diagnostics* diagnostics = nullptr,
                         SearchBudget* budget = nullptr,
                         std::vector< ar::Statement* >* creations = nullptr,
                         const CandidateFacts* facts = nullptr,
                         unsigned loop_bound = 1,
                         ar::CallBase* finite_source = nullptr,
                         unsigned finite_value = 0,
                         AccessTargets targets = {},
                         AccessSearchProgress* progress = nullptr) {
  Result outcome(Status::NotSearched);
  if (targets.a == 0 || targets.b == 0) {
    record(diagnostics, "invalid_target_occurrence");
    return Result(Status::Unsupported);
  }
  if (budget != nullptr && budget->expired()) {
    budget->record_exhaustion(diagnostics);
    return Result(Status::BudgetExceeded);
  }
  // Cancellation must cover enumeration and solving, not just path-pair entry.
  const auto* deadline = budget != nullptr ? &budget->deadline : nullptr;
  // Enumerate a few actual values of one source in main. This is a bounded
  // witness search: each choice is attached to the executed call occurrence,
  // and the encoder must bind the same value before accepting a model. Run it
  // after the established path search, so existing confirmations keep priority.
  auto finite_fallback = [&] {
    if (outcome.confirmed() || finite_source != nullptr || loop_bound != 1 ||
        (funcA->name() == "main") == (funcB->name() == "main")) return;
    auto* main = funcA->name() == "main" ? funcA : funcB;
    auto* source = allocation_finite_source(main);
    if (source != nullptr)
      for (unsigned value = 1; value <= 4; ++value) {
        if (budget != nullptr && (budget->expired() || budget->queries == 0)) break;
        outcome.merge(confirm_race(funcA, stmtA, widthA, funcB, stmtB, widthB,
                                   diagnostics, budget, creations, facts, 1, source,
                                   value, targets, progress));
        if (outcome.confirmed()) return;
      }
  };
  // The actual startup and worker prefixes reject unsupported atomic memory
  // events/calls. Synchronization outside them need not be executable here.
  UnrollResult rA = unroll_to(funcA, stmtA, targets.a, loop_bound, true,
                              nullptr, deadline, finite_source, finite_value, targets.exact);
  UnrollResult rB = unroll_to(funcB, stmtB, targets.b, loop_bound, true,
                              nullptr, deadline, finite_source, finite_value, targets.exact);
  if (progress != nullptr) {
    progress->note(rA, true);
    progress->note(rB, false);
  }
  auto needs_completion_search = [](ar::Function* parent, ar::Function* child, const UnrollResult& paths) {
    if (parent->name() != "main" || child->name() == "main") return false;
    if (paths.pruned_join) return true;
    for (const auto& path : paths.paths) {
      bool joined = false; unsigned creations = 0;
      for (const auto& event : path) {
        joined |= event.kind == EvKind::Join;
        creations += event.kind == EvKind::Create && event.thread == reinterpret_cast< std::uint64_t >(child);
      }
      if (joined && creations > 1) return true;
    }
    return false;
  };
  outcome.needs_completion = needs_completion_search(funcA, funcB, rA) || needs_completion_search(funcB, funcA, rB);
  // Both accesses are RMW-tagged in the verifier atomic contract. Discard this
  // non-racing pair before full completion can exhaust the query budget.
  if (!rA.paths.empty() && !rB.paths.empty() &&
      std::all_of(rA.paths.begin(), rA.paths.end(), atomic_candidate) &&
      std::all_of(rB.paths.begin(), rB.paths.end(), atomic_candidate)) {
    record(diagnostics, "atomic_pair");
    return Result(Status::BoundedUnsat);
  }
  auto needs_completion = [&](const UnrollResult& unrolled) {
    return std::any_of(unrolled.paths.begin(), unrolled.paths.end(), atomic_candidate);
  };
  if (needs_completion(rA) || needs_completion(rB)) {
    // The completed main suffix can contain joins absent before either target.
    outcome.needs_completion |= (funcA->name() == "main") != (funcB->name() == "main");
    rA = unroll_completed(funcA, stmtA, loop_bound, deadline,
                          finite_source, finite_value, targets.a, targets.exact);
    rB = unroll_completed(funcB, stmtB, loop_bound, deadline,
                          finite_source, finite_value, targets.b, targets.exact);
    if (progress != nullptr) {
      progress->note(rA, true);
      progress->note(rB, false);
    }
    record(diagnostics, "atomic_completed_paths");
  }
  if (rA.rejected_reason != nullptr) {
    record(diagnostics, rA.rejected_reason, rA.rejected_statement);
    outcome.merge(Result(Status::Unsupported));
  }
  if (rB.rejected_reason != nullptr) {
    record(diagnostics, rB.rejected_reason, rB.rejected_statement);
    outcome.merge(Result(Status::Unsupported));
  }
  if (diagnostics != nullptr) {
    diagnostics->paths_a = rA.paths.size();
    diagnostics->paths_b = rB.paths.size();
  }
  // Atomic closure can follow the selected dynamic access.
  for (auto& p : rA.paths) {
    auto index = candidate_index(p);
    if (index < p.size()) p[index].width = widthA;
  }
  for (auto& p : rB.paths) {
    auto index = candidate_index(p);
    if (index < p.size()) p[index].width = widthB;
  }
  // Exhaustion leaves unexplored alternatives, not incomplete emitted prefixes.
  // Retained targets/returns still follow actual guards and atomic completion.
  if (rA.incomplete || rB.incomplete) {
    outcome.merge(Result(Status::BudgetExceeded));
    if (rA.incomplete) {
      record(diagnostics, rA.reason != nullptr ? rA.reason : "unroll_incomplete", stmtA);
    }
    if (rB.incomplete) {
      record(diagnostics, rB.reason != nullptr ? rB.reason : "unroll_incomplete", stmtB);
    }
    if (budget == nullptr || !budget->probe_interrupted) {
      if (budget != nullptr && !rA.paths.empty() && !rB.paths.empty()) budget->saw_interrupted = true;
      return outcome;
    }
    // Keep first-pass queries unchanged. In deferred groups, a smaller complete
    // enumeration may find short executions absent from the saturated DFS set.
    if (loop_bound >= 16) {
      record(diagnostics, "interrupted_small_bound");
      outcome.merge(confirm_race(funcA, stmtA, widthA, funcB, stmtB, widthB,
                                 diagnostics, budget, creations, facts, 2,
                                 finite_source, finite_value, targets, progress));
      if (outcome.confirmed()) return outcome;
      if (diagnostics != nullptr) {
        diagnostics->paths_a = rA.paths.size();
        diagnostics->paths_b = rB.paths.size();
      }
    }
  }
  if (rA.paths.empty() || rB.paths.empty()) {
    record(diagnostics, "target_not_reached", rA.paths.empty() ? stmtA : stmtB);
    if (loop_bound == 1) {
      auto bound = std::max(literal_loop_search_bound(funcA), literal_loop_search_bound(funcB));
      if (bound > 16) record(diagnostics, "literal_loop_search_hint");
      if (budget != nullptr && budget->defer_large_loops) {
        budget->saw_interrupted = true;
        record(diagnostics, "deferred_loop_search");
      } else outcome.merge(confirm_race(funcA, stmtA, widthA, funcB, stmtB, widthB,
                                        diagnostics, budget, creations, facts, bound,
                                        finite_source, finite_value, targets, progress));
      finite_fallback();
      return outcome;
    }
    outcome.merge(Result(Status::BoundedUnsat));
    return outcome;
  }
  // Prefer smaller concrete prefixes before spending the shared query budget.
  auto shorter = [](const auto& a, const auto& b) {return a.size() < b.size();};
  std::stable_sort(rA.paths.begin(), rA.paths.end(), shorter);
  std::stable_sort(rB.paths.begin(), rB.paths.end(), shorter);
  struct Writers {
    bool direct = true;
    std::set< ar::GlobalVariable* > globals;
  };
  auto writers = [](const auto& paths) {
    Writers result;
    for (const auto& path : paths)
      for (const auto& event : path) {
        auto* store = core::dyn_cast< ar::Store >(event.stmt);
        if (store == nullptr || abstract_initializer(store)) continue;
        auto* global = core::dyn_cast< ar::GlobalVariable >(store->pointer());
        if (global == nullptr) result.direct = false;
        else result.globals.insert(global);
      }
    return result;
  };
  const auto writers_a = writers(rA.paths), writers_b = writers(rB.paths);
  std::set< const std::vector< BmcEvent >* > rejected_a, rejected_b;
  auto reusable = [&](const GuardRejection& rejection, const auto& path, const Writers& other) {
    if (rejection.path == 0 || !other.direct) return false;
    // Root arguments may vary with the opposing main prefix. Direct stores
    // prevent that context from changing the rejected path's own read sources.
    for (const auto& event : path) {
      auto* store = core::dyn_cast< ar::Store >(event.stmt);
      if (store != nullptr && !abstract_initializer(store) &&
          !core::isa< ar::GlobalVariable >(store->pointer())) return false;
    }
    for (auto* global : rejection.reads)
      if (other.globals.count(global) != 0) return false;
    return true;
  };
  for (const auto& pA : rA.paths) {
    for (const auto& pB : rB.paths) {
      if (budget != nullptr && (budget->expired() || budget->queries == 0)) {
        budget->record_exhaustion(diagnostics);
        record(diagnostics, "search_budget");
        outcome.merge(Result(Status::BudgetExceeded));
        return outcome;
      }
      // Only exact false guards with invariant value sources are reusable.
      // The cache belongs to this fixed startup, path set and loop bound.
      if (rejected_a.count(&pA) != 0 || rejected_b.count(&pB) != 0) {
        record(diagnostics, "cached_guard_rejection");
        outcome.merge(Result(Status::BoundedUnsat));
        continue;
      }
      if (diagnostics != nullptr) {
        ++diagnostics->path_pairs;
      }
      auto starts = [](const std::vector< BmcEvent >& path, ar::Function* child) {
        for (const BmcEvent& event : path) {
          if (event.kind == EvKind::Create &&
              event.thread == reinterpret_cast< std::uint64_t >(child)) {
            return true;
          }
        }
        return false;
      };
      if ((funcA->name() == "main" && !starts(pA, funcB)) ||
          (funcB->name() == "main" && !starts(pB, funcA))) {
        record(diagnostics, "thread_activation_unproven");
        outcome.merge(Result(Status::Unsupported));
        continue;
      }
      if (budget != nullptr) {
        --budget->queries;
      }
      GuardRejection rejection;
      outcome.merge(check_race(pA, pB, funcA, funcB, diagnostics, facts, &rejection,
                              &outcome.metrics, nullptr, deadline,
                              budget != nullptr ? budget->solver_timeout_ms : 1000u));
      if (rejection.startup_infeasible) {
        // Every worker alternative in this call shares the same startup;
        // dormant children cannot repair its already-false concrete guard.
        record(diagnostics, "constant_startup_guard_unsat");
        return outcome;
      }
      if (outcome.confirmed()) {
        if (creations != nullptr) {
          creations->clear();
          for (const auto* path : {&pA, &pB})
            for (const auto& event : *path)
              if (event.kind == EvKind::Create) creations->push_back(event.stmt);
        }
        return outcome;
      }
      if (rejection.path == 1 && reusable(rejection, pA, writers_b)) rejected_a.insert(&pA);
      if (rejection.path == 2 && reusable(rejection, pB, writers_a)) rejected_b.insert(&pB);
    }
  }
  if (loop_bound == 1) {
    auto bound = std::max(literal_loop_search_bound(funcA), literal_loop_search_bound(funcB));
    if (bound > 16) record(diagnostics, "literal_loop_search_hint");
    if (budget != nullptr && budget->defer_large_loops) {
      budget->saw_interrupted = true;
      record(diagnostics, "deferred_loop_search");
    } else outcome.merge(confirm_race(funcA, stmtA, widthA, funcB, stmtB, widthB,
                                      diagnostics, budget, creations, facts, bound,
                                      finite_source, finite_value, targets, progress));
  }
  finite_fallback();
  return outcome;
}

// Finite auxiliary executions are a fallback for main/worker candidates. Other
// created children may remain dormant; joined generations must really finish.
inline Result confirm_race_completion(ar::Function* funcA, ar::Statement* stmtA,
    std::uint64_t widthA, ar::Function* funcB, ar::Statement* stmtB,
    std::uint64_t widthB, Diagnostics* diagnostics, SearchBudget& budget,
    std::vector< ar::Statement* >* creations, const CandidateFacts* facts,
    ar::CallBase* finite_source = nullptr, unsigned finite_value = 0,
    AccessTargets targets = {}, AccessSearchProgress* progress = nullptr) {
  Result outcome(Status::NotSearched);
  if (targets.a == 0 || targets.b == 0) {
    record(diagnostics, "invalid_target_occurrence");
    return Result(Status::Unsupported);
  }
  const bool main_a = funcA->name() == "main";
  if (main_a == (funcB->name() == "main")) return outcome;
  auto* main = main_a ? funcA : funcB;
  auto* worker = main_a ? funcB : funcA;
  auto* main_target = main_a ? stmtA : stmtB;
  auto* worker_target = main_a ? stmtB : stmtA;
  const auto main_occurrence = main_a ? targets.a : targets.b;
  const auto worker_occurrence = main_a ? targets.b : targets.a;
  const std::size_t main_index = main_a ? 1 : 2, target_index = main_a ? 2 : 1;
  std::set< ar::Function* > scanned;
  std::function< bool(ar::Function*, unsigned) > has_join;
  has_join = [&](ar::Function* function, unsigned depth) {
    if (!inline_ok(function) || depth > MAX_INLINE_DEPTH || !scanned.insert(function).second) return false;
    for (auto* block : *function->body()) for (auto* statement : *block) {
      BmcEvent event;
      if (classify_event(statement, event) && event.kind == EvKind::Join) return true;
      auto* call = core::dyn_cast< ar::CallBase >(statement);
      if (call == nullptr) continue;
      auto* callee = exact_callee(call);
      // Unknown indirect calls may select a helper containing a join.
      if (callee == nullptr || has_join(callee, depth + 1)) return true;
    }
    return false;
  };
  if (!has_join(main, 0)) return outcome;
  auto spend = [&]() {
    if (budget.expired() || budget.queries == 0) {
      budget.record_exhaustion(diagnostics);
      outcome.merge(Result(Status::BudgetExceeded));
      record(diagnostics, "completion_search_budget");
      return false;
    }
    --budget.queries;
    return true;
  };
  if (!spend()) return outcome;
  const auto bound = std::max(literal_loop_search_bound(main), literal_loop_search_bound(worker));
  auto mains = unroll_to(main, main_target, main_occurrence, bound, false, nullptr, &budget.deadline,
                         finite_source, finite_value, targets.exact);
  if (progress != nullptr) progress->note(mains, main_a);
  UnrollResult prefixes;
  UnrollResult completed;
  bool completed_unrolled = false;
  auto note_unroll = [&](const UnrollResult& result) {
    if (result.incomplete) {
      outcome.merge(Result(Status::BudgetExceeded));
      record(diagnostics, result.reason);
    }
    if (result.rejected_reason != nullptr) {
      outcome.merge(Result(Status::Unsupported));
      record(diagnostics, result.rejected_reason, result.rejected_statement);
    }
  };
  note_unroll(mains);
  prefixes = unroll_to(worker, worker_target, worker_occurrence, bound, false, nullptr,
                       &budget.deadline, nullptr, 0, targets.exact);
  if (progress != nullptr) progress->note(prefixes, !main_a);
  note_unroll(prefixes);
  const bool complete_roots = std::any_of(mains.paths.begin(), mains.paths.end(), atomic_candidate) ||
      std::any_of(prefixes.paths.begin(), prefixes.paths.end(), atomic_candidate);
  if (complete_roots) {
    mains = unroll_completed(main, main_target, bound, &budget.deadline, finite_source,
                             finite_value, main_occurrence, targets.exact);
    if (progress != nullptr) progress->note(mains, main_a);
    note_unroll(mains);
    record(diagnostics, "atomic_auxiliary_completed_paths");
  }
  std::map< ar::Function*, UnrollResult > returns;
  for (const auto& main_prefix : mains.paths) {
    if (complete_roots && completion_return(main_prefix, main) == nullptr) {
      record(diagnostics, "completion_main_return");
      outcome.merge(Result(Status::Unsupported)); continue;
    }
    if (!complete_roots && std::count_if(main_prefix.begin(), main_prefix.end(),
        [](const auto& event) {return event.kind == EvKind::Join;}) > 4) {
      record(diagnostics, "completion_trace_budget"); outcome.merge(Result(Status::BudgetExceeded)); continue;
    }
    if (!completion_prefix_supported(main_prefix)) {
      record(diagnostics, "completion_prefix"); outcome.merge(Result(Status::Unsupported)); continue;
    }
    std::map< std::size_t, std::size_t > joined; // generation -> main join step
    std::set< std::size_t > handle_reads;
    bool supported = true;
    for (std::size_t i = 0; i < main_prefix.size(); ++i) {
      if (main_prefix[i].kind != EvKind::Join) continue;
      std::size_t creation = 0;
      std::size_t read = 0;
      if (!completion_join_creation(main_prefix, i, creation, complete_roots, &read) ||
          !joined.emplace(creation, i).second) {
        record(diagnostics, "completion_join_generation", main_prefix[i].stmt);
        supported = false; break;
      }
      handle_reads.insert(read);
    }
    if (!supported) {outcome.merge(Result(Status::Unsupported)); continue;}
    if (joined.empty()) continue;
    std::vector< std::size_t > target_creations;
    for (std::size_t i = 0; i < main_prefix.size(); ++i)
      if (main_prefix[i].kind == EvKind::Create &&
          main_prefix[i].thread == reinterpret_cast< std::uint64_t >(worker)) target_creations.push_back(i);
    if (target_creations.empty()) continue;
    std::stable_sort(target_creations.begin(), target_creations.end(), [&](auto a, auto b) {
      return joined.count(a) < joined.count(b);
    });
    for (auto target_creation : target_creations) {
      // Only joined target instances finish before main's stack/process death.
      if (complete_roots && joined.count(target_creation) == 0) {
        record(diagnostics, "completion_target_unjoined");
        outcome.merge(Result(Status::Unsupported)); continue;
      }
      if (!spend()) return outcome;
      if (joined.count(target_creation) != 0 && !completed_unrolled) {
        completed = unroll_completed(worker, worker_target, bound, &budget.deadline,
                                     nullptr, 0, worker_occurrence, targets.exact);
        if (progress != nullptr) progress->note(completed, !main_a);
        completed_unrolled = true;
        note_unroll(completed);
      }
      const auto& workers = joined.count(target_creation) != 0 ? completed.paths : prefixes.paths;
      for (const auto& worker_prefix : workers) {
        if ((!complete_roots && atomic_candidate(worker_prefix)) ||
            (complete_roots && completion_return(worker_prefix, worker) == nullptr)) {
          record(diagnostics, "completion_worker_return");
          outcome.merge(Result(Status::Unsupported)); continue;
        }
        if (!completion_prefix_supported(worker_prefix, true)) {
          record(diagnostics, "completion_child_lifecycle"); outcome.merge(Result(Status::Unsupported)); continue;
        }
        auto main_path = main_prefix, worker_path = worker_prefix;
        for (auto read : handle_reads) main_path[read].joined_handle_read = true;
        auto main_candidate = candidate_index(main_path), worker_candidate = candidate_index(worker_path);
        if (main_candidate == main_path.size() || worker_candidate == worker_path.size()) continue;
        main_path[main_candidate].width = main_a ? widthA : widthB;
        worker_path[worker_candidate].width = main_a ? widthB : widthA;
        CompletionPlan plan;
        plan.target_creation = target_creation;
        bool plan_supported = true;
        std::vector< std::pair< std::size_t, std::size_t > > auxiliary;
        for (const auto& entry : joined) {
          if (entry.first == target_creation) {
            auto* ret = completion_return(worker_path, worker);
            if (ret == nullptr) {plan_supported = false; break;}
            main_path[entry.second].join_creation = entry.first;
            main_path[entry.second].join_completion = ret;
            plan.joins.emplace(entry.second, target_index);
          } else auxiliary.push_back(entry);
        }
        if (!plan_supported) {outcome.merge(Result(Status::Unsupported)); continue;}
        std::function< void(std::size_t, std::size_t) > enumerate;
        enumerate = [&](std::size_t next, std::size_t events) {
          if (outcome.confirmed() || budget.expired() || budget.queries == 0) return;
          if (events > 512) {
            record(diagnostics, "completion_event_budget");
            outcome.merge(Result(Status::BudgetExceeded)); return;
          }
          if (next == auxiliary.size()) {
            if (!spend()) return;
            if (diagnostics != nullptr) ++diagnostics->path_pairs;
            record(diagnostics, "auxiliary_completion_query");
            outcome.merge(check_race(main_a ? main_path : worker_path,
                main_a ? worker_path : main_path, funcA, funcB, diagnostics,
                facts, nullptr, &outcome.metrics, &plan, &budget.deadline,
                budget.solver_timeout_ms));
            return;
          }
          const auto creation = auxiliary[next].first, join_step = auxiliary[next].second;
          auto* root = reinterpret_cast< ar::Function* >(main_path[creation].thread);
          auto found = returns.find(root);
          if (found == returns.end()) {
            if (!spend()) return;
            auto result = unroll_terminated(root, literal_loop_search_bound(root), &budget.deadline);
            note_unroll(result);
            found = returns.emplace(root, std::move(result)).first;
          }
          if (found->second.paths.empty()) outcome.merge(Result(Status::Unsupported));
          for (const auto& path : found->second.paths) {
            if (!spend()) return;
            auto* ret = completion_return(path, root);
            if (ret == nullptr || !completion_prefix_supported(path, true)) {
              record(diagnostics, "completion_child_lifecycle");
              outcome.merge(Result(Status::Unsupported)); continue;
            }
            ExecutionTrace trace;
            trace.id = "aux_" + std::to_string(next);
            trace.root = root; trace.events = path;
            trace.creator_path = main_index; trace.creator_step = creation;
            if (!bind_creation_step(root, main_path, creation, trace.argument)) {
              outcome.merge(Result(Status::Unsupported)); continue;
            }
            plan.auxiliary.push_back(std::move(trace));
            plan.joins[join_step] = next + 3;
            main_path[join_step].join_creation = creation;
            main_path[join_step].join_completion = ret;
            enumerate(next + 1, events + path.size());
            plan.auxiliary.pop_back();
            plan.joins.erase(join_step);
            if (outcome.confirmed()) return;
          }
        };
        enumerate(0, main_path.size() + worker_path.size());
        if (outcome.confirmed()) {
          if (creations != nullptr) {
            creations->clear();
            for (const auto& event : main_path)
              if (event.kind == EvKind::Create) creations->push_back(event.stmt);
          }
          return outcome;
        }
        if (budget.expired() || budget.queries == 0) {
          budget.record_exhaustion(diagnostics);
          outcome.merge(Result(Status::BudgetExceeded)); return outcome;
        }
      }
    }
  }
  if (!outcome.confirmed() && finite_source == nullptr && !budget.expired()) {
    auto* source = allocation_finite_source(main);
    if (source != nullptr)
      for (unsigned value = 1; value <= 4 && !budget.expired() && budget.queries != 0; ++value) {
        outcome.merge(confirm_race_completion(funcA, stmtA, widthA, funcB, stmtB, widthB,
            diagnostics, budget, creations, facts, source, value, targets, progress));
        if (outcome.confirmed()) break;
      }
  }
  return outcome;
}

// Additional exact-access requests run only after every legacy search pass.
// The frontier is bounded by retained event capacity and shrinks when a fully
// constructed prefix search cannot even visit the requested occurrence. This
// only schedules witness searches; it never proves an absent race or SAFE.
template < typename ConfirmExact >
inline Result search_later_access_frontiers(Diagnostics* diagnostics,
    SearchBudget& budget, ConfirmExact&& confirm_exact,
    unsigned requested_frontier = 0, LaterAccessState* saved_state = nullptr) {
  Result outcome(Status::NotSearched);
  LaterAccessState local_state;
  auto& state = saved_state != nullptr ? *saved_state : local_state;
  auto& limit_a = state.limit_a;
  auto& limit_b = state.limit_b;
  const unsigned first = requested_frontier != 0 ? requested_frontier : 2;
  for (unsigned frontier = first; state.pending(frontier) &&
       (requested_frontier == 0 || frontier == requested_frontier); ++frontier) {
    for (unsigned a = 1; a <= frontier && a <= limit_a; ++a) {
      for (unsigned b = 1; b <= frontier && b <= limit_b; ++b) {
        if (std::max(a, b) != frontier) continue;
        if (budget.expired() || budget.candidates == 0 || budget.queries == 0) {
          budget.record_exhaustion(diagnostics);
          record(diagnostics, "later_access_budget");
          outcome.merge(Result(Status::BudgetExceeded));
          return outcome;
        }
        --budget.candidates;
        record(diagnostics, "later_access_query");
        const AccessTargets targets{a, b, true};
        AccessSearchProgress ordinary, completed;
        auto attempt = confirm_exact(targets, ordinary, completed);
        outcome.merge(std::move(attempt));
        if (outcome.confirmed()) {
          record(diagnostics, "later_access_confirmed");
          return outcome;
        }
        auto narrow = [](unsigned requested, const AccessSearchProgress::Side& prefix,
                         const AccessSearchProgress::Side& completion, unsigned& limit) {
          const auto observed = std::max(prefix.maximum, completion.maximum);
          if (observed < requested &&
              (prefix.complete_frontier() || completion.complete_frontier()))
            limit = std::min(limit, observed);
        };
        narrow(a, ordinary.a, completed.a, limit_a);
        narrow(b, ordinary.b, completed.b, limit_b);
      }
    }
  }
  return outcome;
}

// Preserve the original pass-7 strategy and state ownership. Only its shell
// loop is shared with provider search; no cache, plan or budget is shared anew.
inline Result confirm_race_later(ar::Function* funcA, ar::Statement* stmtA,
    std::uint64_t widthA, ar::Function* funcB, ar::Statement* stmtB,
    std::uint64_t widthB, Diagnostics* diagnostics, SearchBudget& budget,
    std::vector< ar::Statement* >* creations, const CandidateFacts* facts,
    unsigned requested_frontier = 0, LaterAccessState* saved_state = nullptr) {
  auto confirm_exact = [&](AccessTargets targets, AccessSearchProgress& ordinary,
                           AccessSearchProgress& completed) {
    auto attempt = confirm_race(funcA, stmtA, widthA, funcB, stmtB, widthB,
        diagnostics, &budget, creations, facts, 1, nullptr, 0, targets, &ordinary);
    // A later main target may reveal joins absent before the first target.
    // Do not inherit completion eligibility solely from an earlier request.
    if (!attempt.confirmed() && attempt.needs_completion &&
        !budget.expired() && budget.queries != 0)
      attempt.merge(confirm_race_completion(funcA, stmtA, widthA, funcB, stmtB,
          widthB, diagnostics, budget, creations, facts, nullptr, 0, targets, &completed));
    return attempt;
  };
  return search_later_access_frontiers(diagnostics, budget, confirm_exact,
                                       requested_frontier, saved_state);
}

// The main prefix is not evaluated before child execution. All its guards,
// data reads, creates, locks and exact joins share the solver timeline with a
// real finite provider and the two target instances. This is a witness search,
// not an activation summary, and no failure grants a SAFE permission.
enum class ProviderEndpoint { Create, CreatorRelease };

inline bool creator_release_search_enabled() {
  return std::getenv("IKOS_BMC_DISABLE_CREATOR_RELEASE") == nullptr;
}

inline bool creator_release_endpoint(ar::Statement* statement) {
  auto* call = core::dyn_cast< ar::CallBase >(statement);
  auto* callee = call != nullptr ? exact_callee(call) : nullptr;
  BmcEvent event;
  return callee != nullptr && callee->is_declaration() &&
         (callee->name() == "pthread_mutex_unlock" ||
          callee->intrinsic_id() == ar::Intrinsic::PthreadMutexUnlock) &&
         call->num_arguments() == 1 &&
         classify_event(statement, event) && event.kind == EvKind::Unlock;
}

inline Result confirm_race_provider(ar::Function* funcA, ar::Statement* stmtA,
    std::uint64_t widthA, ar::Function* funcB, ar::Statement* stmtB,
    std::uint64_t widthB, Diagnostics* diagnostics, SearchBudget& budget,
    std::vector< ar::Statement* >* creations, const CandidateFacts* facts,
    AccessTargets access_targets = {}, AccessSearchProgress* progress = nullptr,
    AccessSearchProgress* completion_progress = nullptr,
    ProviderEndpoint endpoint_mode = ProviderEndpoint::Create) {
  Result outcome(Status::NotSearched);
  const bool release = endpoint_mode == ProviderEndpoint::CreatorRelease;
  if (release && !creator_release_search_enabled()) return outcome;
  if (funcA->name() == "main" || funcB->name() == "main") return outcome;
  auto* main = funcA->bundle()->function_or_null("main");
  if (main == nullptr || !main->is_definition() || access_targets.a == 0 ||
      access_targets.b == 0 ||
      (facts != nullptr && (!facts->startup.empty() || facts->selected_worker_occurrences)))
    return Result(Status::Unsupported);
  auto spend = [&]() {
    if (budget.expired() || budget.queries == 0) {
      budget.record_exhaustion(diagnostics);
      record(diagnostics, "provider_search_budget");
      outcome.merge(Result(Status::BudgetExceeded));
      return false;
    }
    --budget.queries;
    return true;
  };
  auto note = [&](const UnrollResult& result) {
    if (result.incomplete) {
      record(diagnostics, result.reason ? result.reason : "provider_unroll_incomplete");
      outcome.merge(Result(Status::BudgetExceeded));
    }
    if (result.rejected_reason != nullptr) {
      record(diagnostics, result.rejected_reason, result.rejected_statement);
      outcome.merge(Result(Status::Unsupported));
    }
  };
  const auto bound = std::max(literal_loop_search_bound(main),
      std::max(literal_loop_search_bound(funcA), literal_loop_search_bound(funcB)));
  if (!spend()) return outcome;
  auto pathsA = unroll_to(funcA, stmtA, access_targets.a, bound, false, nullptr,
                         &budget.deadline, nullptr, 0, access_targets.exact);
  note(pathsA);
  if (progress != nullptr) progress->note(pathsA, true);
  if (!spend()) return outcome;
  auto pathsB = unroll_to(funcB, stmtB, access_targets.b, bound, false, nullptr,
                         &budget.deadline, nullptr, 0, access_targets.exact);
  note(pathsB);
  if (progress != nullptr) progress->note(pathsB, false);
  UnrollResult completeA, completeB;
  const bool atomic = std::any_of(pathsA.paths.begin(), pathsA.paths.end(), atomic_candidate) ||
                      std::any_of(pathsB.paths.begin(), pathsB.paths.end(), atomic_candidate);
  if (atomic) {
    if (!spend()) return outcome;
    completeA = unroll_completed(funcA, stmtA, bound, &budget.deadline, nullptr, 0,
                                access_targets.a, access_targets.exact);
    note(completeA);
    if (completion_progress != nullptr) completion_progress->note(completeA, true);
    if (!spend()) return outcome;
    completeB = unroll_completed(funcB, stmtB, bound, &budget.deadline, nullptr, 0,
                                access_targets.b, access_targets.exact);
    note(completeB);
    if (completion_progress != nullptr) completion_progress->note(completeB, false);
  }
  const auto& choicesA = atomic ? completeA.paths : pathsA.paths;
  const auto& choicesB = atomic ? completeB.paths : pathsB.paths;
  if (choicesA.empty() || choicesB.empty()) return outcome;
  std::vector< ar::Statement* > endpoints;
  for (auto* block : *main->body()) for (auto* statement : *block) {
    BmcEvent event;
    if (release ? creator_release_endpoint(statement) :
        (classify_event(statement, event) && event.kind == EvKind::Create &&
         (event.thread == reinterpret_cast< std::uint64_t >(funcA) ||
          event.thread == reinterpret_cast< std::uint64_t >(funcB)))) endpoints.push_back(statement);
  }
  std::map< ar::Function*, UnrollResult > completed_providers;
  // Create mode preserves the original occurrence order. Release mode searches
  // only its first three actual occurrences; this is a search limit, not a
  // claim that the create bound limits how often a mutex may be released.
  for (auto* endpoint : endpoints) for (unsigned occurrence = 1; occurrence <= 3; ++occurrence) {
    if (!spend()) return outcome;
    auto mains = unroll_to(main, endpoint, occurrence, bound, false, nullptr,
                           &budget.deadline, nullptr, 0, true);
    note(mains);
    for (const auto& main_prefix : mains.paths) {
      if (!startup_provider_prefix_supported(main_prefix)) {
        record(diagnostics, "provider_main_profile");
        outcome.merge(Result(Status::Unsupported)); continue;
      }
      std::vector< std::size_t > created;
      for (std::size_t i = 0; i < main_prefix.size(); ++i)
        if (main_prefix[i].kind == EvKind::Create) created.push_back(i);
      if (created.size() < 2 || created.size() > 3) {
        record(diagnostics, "provider_instance_bound"); continue;
      }
      for (auto ca : created) for (auto cb : created) {
        if (release && (main_prefix.empty() ||
                        main_prefix.back().stmt != endpoint ||
                        main_prefix.back().kind != EvKind::Unlock ||
                        ca >= main_prefix.size() - 1 || cb >= main_prefix.size() - 1)) continue;
        if (ca == cb || main_prefix[ca].thread != reinterpret_cast< std::uint64_t >(funcA) ||
            main_prefix[cb].thread != reinterpret_cast< std::uint64_t >(funcB)) continue;
        StartupProviderPlan plan;
        plan.main = main_prefix; plan.creation_a = ca; plan.creation_b = cb;
        for (auto& event : plan.main) event.candidate = false;
        auto cp = ExecutionTrace::no_creation;
        for (auto creation : created) if (creation != ca && creation != cb) cp = creation;
        bool supported = true;
        for (std::size_t j = 0; j < plan.main.size(); ++j) {
          if (plan.main[j].kind != EvKind::Join) continue;
          std::size_t joined = 0, read = 0;
          if (cp == ExecutionTrace::no_creation || !plan.joins.empty() ||
              !completion_join_creation(plan.main, j, joined, false, &read) || joined != cp) {
            supported = false; break;
          }
          plan.joins.emplace(j, 4);
          plan.main[read].joined_handle_read = true;
        }
        if (!supported) {
          record(diagnostics, "provider_join_generation");
          outcome.merge(Result(Status::Unsupported)); continue;
        }
        auto check_targets = [&]() {
          for (const auto& originalA : choicesA) for (const auto& originalB : choicesB) {
            if (!startup_provider_prefix_supported(originalA, true) ||
                !startup_provider_prefix_supported(originalB, true) ||
                (atomic && (completion_return(originalA, funcA) == nullptr ||
                            completion_return(originalB, funcB) == nullptr))) {
              record(diagnostics, "provider_target_profile");
              outcome.merge(Result(Status::Unsupported)); continue;
            }
            if (plan.main.size() + plan.provider.events.size() + originalA.size() +
                originalB.size() > MAX_PATH_EVENTS) {
              record(diagnostics, "provider_event_budget");
              outcome.merge(Result(Status::BudgetExceeded)); continue;
            }
            auto a = originalA, b = originalB;
            const auto ta = candidate_index(a), tb = candidate_index(b);
            if (ta == a.size() || tb == b.size()) continue;
            a[ta].width = widthA; b[tb].width = widthB;
            if (!spend()) return;
            if (diagnostics != nullptr) ++diagnostics->path_pairs;
            record(diagnostics, "provider_timeline_query");
            if (release) record(diagnostics, "creator_release_timeline_query");
            outcome.merge(check_race(a, b, funcA, funcB, diagnostics, facts, nullptr,
                &outcome.metrics, nullptr, &budget.deadline, budget.solver_timeout_ms, &plan));
            if (outcome.confirmed()) return;
          }
        };
        if (cp == ExecutionTrace::no_creation) {
          check_targets();
        } else {
          auto* root = reinterpret_cast< ar::Function* >(plan.main[cp].thread);
          auto found = completed_providers.find(root);
          if (found == completed_providers.end()) {
            if (!spend()) return outcome;
            auto paths = unroll_terminated(root, literal_loop_search_bound(root), &budget.deadline);
            note(paths);
            found = completed_providers.emplace(root, std::move(paths)).first;
          }
          for (const auto& provider : found->second.paths) {
            auto* returned = completion_return(provider, root);
            if (returned == nullptr || !startup_provider_prefix_supported(provider, true)) {
              record(diagnostics, "provider_completion_profile");
              outcome.merge(Result(Status::Unsupported)); continue;
            }
            plan.provider.id = "provider";
            plan.provider.root = root; plan.provider.events = provider;
            plan.provider.creator_path = 3; plan.provider.creator_step = cp;
            for (auto& event : plan.provider.events) event.candidate = false;
            if (!bind_creation_step(root, plan.main, cp, plan.provider.argument)) continue;
            for (const auto& join : plan.joins) {
              plan.main[join.first].join_creation = cp;
              plan.main[join.first].join_completion = returned;
            }
            check_targets();
            if (outcome.confirmed() || budget.expired() || budget.queries == 0) break;
          }
        }
        if (outcome.confirmed()) {
          record(diagnostics, "provider_timeline_confirmed");
          if (release) record(diagnostics, "creator_release_timeline_confirmed");
          if (creations != nullptr) {
            creations->clear();
            for (auto index : created) creations->push_back(plan.main[index].stmt);
          }
          return outcome;
        }
        if (budget.expired() || budget.queries == 0) {
          budget.record_exhaustion(diagnostics);
          outcome.merge(Result(Status::BudgetExceeded)); return outcome;
        }
      }
    }
  }
  return outcome;
}

// Run only after every first-access provider candidate has had its existing
// pass. Actual target ordinals are independent of the provider plan's exact
// create steps, including two distinct instances of one shared entry function.
inline Result confirm_race_provider_later(ar::Function* funcA, ar::Statement* stmtA,
    std::uint64_t widthA, ar::Function* funcB, ar::Statement* stmtB,
    std::uint64_t widthB, Diagnostics* diagnostics, SearchBudget& budget,
    std::vector< ar::Statement* >* creations, const CandidateFacts* facts,
    unsigned requested_frontier = 0, LaterAccessState* saved_state = nullptr,
    ProviderEndpoint endpoint_mode = ProviderEndpoint::Create) {
  if (endpoint_mode == ProviderEndpoint::CreatorRelease &&
      !creator_release_search_enabled()) return Result(Status::NotSearched);
  auto confirm_exact = [&](AccessTargets targets, AccessSearchProgress& ordinary,
                           AccessSearchProgress& completed) {
    record(diagnostics, "provider_later_access_query");
    return confirm_race_provider(funcA, stmtA, widthA, funcB, stmtB, widthB,
        diagnostics, budget, creations, facts, targets, &ordinary, &completed, endpoint_mode);
  };
  auto outcome = search_later_access_frontiers(diagnostics, budget, confirm_exact,
                                              requested_frontier, saved_state);
  if (outcome.confirmed()) record(diagnostics, "provider_later_access_confirmed");
  return outcome;
}

// Project the candidate's real statements through a finite physical caller.
// AI once callback SIDs choose targets only; they never become pthread roots.
inline Result confirm_race_once_physical(ar::Statement* stmtA, std::uint64_t widthA,
    ar::Statement* stmtB, std::uint64_t widthB, Diagnostics* diagnostics,
    SearchBudget& budget, std::vector< ar::Statement* >* creations,
    const std::function< bool(ar::CallBase*) >& sealed, bool main_argc_one) {
  Result outcome(Status::NotSearched);
  if (getenv("IKOS_BMC_DISABLE_ONCE_EXECUTION") != nullptr) return outcome;
  auto* main = stmtA->bundle()->function_or_null("main");
  if (!main || !main->is_definition()) return Result(Status::Unsupported);
  CandidateFacts physical_facts;
  physical_facts.main_argc_one = main_argc_one;
  if (main->num_parameters() != 0 && !main_argc_one) {
    record(diagnostics, "main_entry_environment"); return Result(Status::Unsupported);
  }
  auto spend = [&]() {
    if (budget.expired() || budget.queries == 0) {
      budget.record_exhaustion(diagnostics); outcome.merge(Result(Status::BudgetExceeded)); return false;
    }
    --budget.queries; return true;
  };
  auto note = [&](const UnrollResult& paths) {
    if (paths.incomplete) {
      record(diagnostics, paths.reason ? paths.reason : "once_unroll_budget");
      outcome.merge(Result(Status::BudgetExceeded));
    }
    if (paths.rejected_reason) {
      record(diagnostics, paths.rejected_reason, paths.rejected_statement);
      outcome.merge(Result(Status::Unsupported));
    }
  };
  std::set< ar::Function* > children;
  for (auto* block : *main->body()) for (auto* statement : *block) {
    BmcEvent event;
    if (classify_event(statement, event) && event.kind == EvKind::Create)
      children.insert(reinterpret_cast< ar::Function* >(event.thread));
  }
  for (auto* worker : children) for (bool main_a : {true, false}) {
    if (!worker || !worker->is_definition() || worker == main) continue;
    auto* rootA = main_a ? main : worker;
    auto* rootB = main_a ? worker : main;
    // Only the current finite literal bound; no new budget or loop escalation.
    const auto bound = std::max(literal_loop_search_bound(main), literal_loop_search_bound(worker));
    if (!spend()) return outcome;
    auto pathsA = unroll_to(rootA, stmtA, 1, bound, false, nullptr,
                            &budget.deadline, nullptr, 0, true, true);
    note(pathsA);
    if (!spend()) return outcome;
    auto pathsB = unroll_to(rootB, stmtB, 1, bound, false, nullptr,
                            &budget.deadline, nullptr, 0, true, true);
    note(pathsB);
    for (auto a : pathsA.paths) for (auto b : pathsB.paths) {
      const auto& main_path = main_a ? a : b;
      const auto& worker_path = main_a ? b : a;
      std::size_t creation = main_path.size(), count = 0;
      bool supported = completion_return(main_path, main) == nullptr, has_once = false;
      for (std::size_t i = 0; i < main_path.size(); ++i) {
        if (main_path[i].kind == EvKind::Create) { creation = i; ++count; }
        if (main_path[i].kind == EvKind::Join) supported = false;
      }
      supported &= count == 1 && creation < main_path.size() &&
          main_path[creation].thread == reinterpret_cast< std::uint64_t >(worker);
      for (const auto& event : worker_path)
        if (event.kind == EvKind::Create || event.kind == EvKind::Join) supported = false;
      for (const auto* path : {&a, &b}) for (const auto& event : *path) {
        if (event.mutex == PSEUDO_ATOMIC_LOCK) supported = false;
        if (event.once_stage == OnceStage::Call) {
          has_once = true;
          auto* call = core::dyn_cast< ar::CallBase >(event.stmt);
          if (!call || !sealed(call)) supported = false;
        }
      }
      if (!supported || !has_once) {
        record(diagnostics, "once_physical_caller_profile"); outcome.merge(Result(Status::Unsupported)); continue;
      }
      const auto ta = candidate_index(a), tb = candidate_index(b);
      if (ta == a.size() || tb == b.size()) continue;
      a[ta].width = widthA; b[tb].width = widthB;
      if (!spend()) return outcome;
      if (diagnostics) ++diagnostics->path_pairs;
      record(diagnostics, "once_physical_query");
      outcome.merge(check_race(a, b, rootA, rootB, diagnostics, &physical_facts, nullptr,
          &outcome.metrics, nullptr, &budget.deadline, budget.solver_timeout_ms));
      if (outcome.confirmed()) {
        record(diagnostics, "once_physical_confirmed");
        if (creations) { creations->clear(); creations->push_back(main_path[creation].stmt); }
        return outcome;
      }
    }
  }
  return outcome;
}

}  // namespace bmc
}  // namespace analyzer
}  // namespace ikos
