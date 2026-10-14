// BMC 模块 —— 有界竞争确认（供 checker 集成调用）
//
// confirm_race：展开实际执行前缀，再由 check_race 检查同步、值、地址和
// 实例前提。未支持的操作仅阻止执行到它的前缀，不因整函数扫描否决其他路径。

#pragma once

#include <chrono>
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
        if (value < 15 || value > 32) continue;
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
                         unsigned loop_bound = 1) {
  Result outcome(Status::NotSearched);
  // The actual startup and worker prefixes reject unsupported atomic memory
  // events/calls. Synchronization outside them need not be executable here.
  UnrollResult rA = unroll_to(funcA, stmtA, 1, loop_bound, true);
  UnrollResult rB = unroll_to(funcB, stmtB, 1, loop_bound, true);
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
    rA = unroll_completed(funcA, stmtA, loop_bound);
    rB = unroll_completed(funcB, stmtB, loop_bound);
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
                                 diagnostics, budget, creations, facts, 2));
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
      outcome.merge(confirm_race(funcA, stmtA, widthA, funcB, stmtB, widthB,
                                 diagnostics, budget, creations, facts, bound));
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
                              &outcome.metrics, nullptr, nullptr,
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
    outcome.merge(confirm_race(funcA, stmtA, widthA, funcB, stmtB, widthB,
                               diagnostics, budget, creations, facts, bound));
  }
  return outcome;
}

// Finite auxiliary executions are a fallback for main/worker candidates. Other
// created children may remain dormant; joined generations must really finish.
inline Result confirm_race_completion(ar::Function* funcA, ar::Statement* stmtA,
    std::uint64_t widthA, ar::Function* funcB, ar::Statement* stmtB,
    std::uint64_t widthB, Diagnostics* diagnostics, SearchBudget& budget,
    std::vector< ar::Statement* >* creations, const CandidateFacts* facts) {
  Result outcome(Status::NotSearched);
  const bool main_a = funcA->name() == "main";
  if (main_a == (funcB->name() == "main")) return outcome;
  auto* main = main_a ? funcA : funcB;
  auto* worker = main_a ? funcB : funcA;
  auto* main_target = main_a ? stmtA : stmtB;
  auto* worker_target = main_a ? stmtB : stmtA;
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
  auto mains = unroll_to(main, main_target, 1, bound, false, nullptr, &budget.deadline);
  UnrollResult prefixes;
  bool prefixes_unrolled = false;
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
  std::map< ar::Function*, UnrollResult > returns;
  for (const auto& main_prefix : mains.paths) {
    // Atomic targets need both completion suffixes; this fallback retains main
    // only through its target. Leave those queries to the complete-path search.
    if (atomic_candidate(main_prefix)) {
      record(diagnostics, "completion_atomic_target");
      outcome.merge(Result(Status::Unsupported)); continue;
    }
    if (std::count_if(main_prefix.begin(), main_prefix.end(),
        [](const auto& event) {return event.kind == EvKind::Join;}) > 4) {
      record(diagnostics, "completion_trace_budget"); outcome.merge(Result(Status::BudgetExceeded)); continue;
    }
    if (!completion_prefix_supported(main_prefix)) {
      record(diagnostics, "completion_prefix"); outcome.merge(Result(Status::Unsupported)); continue;
    }
    std::map< std::size_t, std::size_t > joined; // generation -> main join step
    bool supported = true;
    for (std::size_t i = 0; i < main_prefix.size(); ++i) {
      if (main_prefix[i].kind != EvKind::Join) continue;
      std::size_t creation = 0;
      if (!completion_join_creation(main_prefix, i, creation) ||
          !joined.emplace(creation, i).second) {
        record(diagnostics, "completion_join_generation", main_prefix[i].stmt);
        supported = false; break;
      }
    }
    if (!supported) {outcome.merge(Result(Status::Unsupported)); continue;}
    if (joined.empty()) continue;
    std::vector< std::size_t > targets;
    for (std::size_t i = 0; i < main_prefix.size(); ++i)
      if (main_prefix[i].kind == EvKind::Create &&
          main_prefix[i].thread == reinterpret_cast< std::uint64_t >(worker)) targets.push_back(i);
    if (targets.empty()) continue;
    if (!prefixes_unrolled) {
      prefixes = unroll_to(worker, worker_target, 1, bound, false, nullptr, &budget.deadline);
      prefixes_unrolled = true;
      note_unroll(prefixes);
    }
    std::stable_sort(targets.begin(), targets.end(), [&](auto a, auto b) {
      return joined.count(a) < joined.count(b);
    });
    for (auto target_creation : targets) {
      if (!spend()) return outcome;
      if (joined.count(target_creation) != 0 && !completed_unrolled) {
        completed = unroll_completed(worker, worker_target, bound, &budget.deadline);
        completed_unrolled = true;
        note_unroll(completed);
      }
      const auto& workers = joined.count(target_creation) != 0 ? completed.paths : prefixes.paths;
      for (const auto& worker_prefix : workers) {
        if (atomic_candidate(worker_prefix)) {
          record(diagnostics, "completion_atomic_target");
          outcome.merge(Result(Status::Unsupported)); continue;
        }
        if (!completion_prefix_supported(worker_prefix, true)) {
          record(diagnostics, "completion_child_lifecycle"); outcome.merge(Result(Status::Unsupported)); continue;
        }
        auto main_path = main_prefix, worker_path = worker_prefix;
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
  return outcome;
}

}  // namespace bmc
}  // namespace analyzer
}  // namespace ikos
