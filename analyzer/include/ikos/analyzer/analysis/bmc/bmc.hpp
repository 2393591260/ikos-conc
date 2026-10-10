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
  std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);

  bool expired() const {
    return std::chrono::steady_clock::now() >= deadline;
  }
};

/// 确认两个访问（在各自线程入口函数里）是否真竞争。
/// funcA/funcB = 两访问所在的线程入口函数；stmtA/stmtB = 两访问语句；
/// widthA/widthB = 两访问的字节宽度（data_layout 的 store size，0=未知），
/// 供 encode 阶段用字节范围重叠判同址。
/// 返回 true = 确认竞争（报 FALSE 找回 TP）；false = 不确认（保持 UNKNOWN）。
inline bool confirm_race(ar::Function* funcA, ar::Statement* stmtA,
                         std::uint64_t widthA, ar::Function* funcB,
                         ar::Statement* stmtB, std::uint64_t widthB,
                         Diagnostics* diagnostics = nullptr,
                         SearchBudget* budget = nullptr,
                         std::vector< ar::Statement* >* creations = nullptr,
                         const CandidateFacts* facts = nullptr,
                         unsigned loop_bound = 1) {
  // The actual startup and worker prefixes reject unsupported atomic memory
  // events/calls. Synchronization outside them need not be executable here.
  UnrollResult rA = unroll_to(funcA, stmtA, 1, loop_bound);
  UnrollResult rB = unroll_to(funcB, stmtB, 1, loop_bound);
  auto atomic_access = [](const std::vector< BmcEvent >& path) {
      int active = 0;
      for (const auto& event : path) {
        if (event.candidate) return active > 0;
        if (event.mutex != PSEUDO_ATOMIC_LOCK) continue;
        if (event.kind == EvKind::Lock) ++active;
        if (event.kind == EvKind::Unlock) --active;
      }
      return false;
  };
  // Both accesses are RMW-tagged in the verifier atomic contract. Discard this
  // non-racing pair before full completion can exhaust the query budget.
  if (!rA.paths.empty() && !rB.paths.empty() &&
      std::all_of(rA.paths.begin(), rA.paths.end(), atomic_access) &&
      std::all_of(rB.paths.begin(), rB.paths.end(), atomic_access)) {
    record(diagnostics, "atomic_pair");
    return false;
  }
  auto needs_completion = [&](const UnrollResult& unrolled) {
    return std::any_of(unrolled.paths.begin(), unrolled.paths.end(), atomic_access);
  };
  if (needs_completion(rA) || needs_completion(rB)) {
    rA = unroll_completed(funcA, stmtA, loop_bound);
    rB = unroll_completed(funcB, stmtB, loop_bound);
    record(diagnostics, "atomic_completed_paths");
  }
  if (rA.rejected_reason != nullptr) record(diagnostics, rA.rejected_reason, rA.rejected_statement);
  if (rB.rejected_reason != nullptr) record(diagnostics, rB.rejected_reason, rB.rejected_statement);
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
    if (rA.incomplete) {
      record(diagnostics, rA.reason != nullptr ? rA.reason : "unroll_incomplete", stmtA);
    }
    if (rB.incomplete) {
      record(diagnostics, rB.reason != nullptr ? rB.reason : "unroll_incomplete", stmtB);
    }
    if (budget == nullptr || !budget->probe_interrupted) {
      if (budget != nullptr && !rA.paths.empty() && !rB.paths.empty()) budget->saw_interrupted = true;
      return false;
    }
    // Keep first-pass queries unchanged. In deferred groups, a smaller complete
    // enumeration may find short executions absent from the saturated DFS set.
    if (loop_bound >= 16) {
      record(diagnostics, "interrupted_small_bound");
      if (confirm_race(funcA, stmtA, widthA, funcB, stmtB, widthB,
                       diagnostics, budget, creations, facts, 2)) return true;
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
      return confirm_race(funcA, stmtA, widthA, funcB, stmtB, widthB,
                          diagnostics, budget, creations, facts, bound);
    }
    return false;
  }
  // Prefer smaller concrete prefixes before spending the shared query budget.
  auto shorter = [](const auto& a, const auto& b) {return a.size() < b.size();};
  std::stable_sort(rA.paths.begin(), rA.paths.end(), shorter);
  std::stable_sort(rB.paths.begin(), rB.paths.end(), shorter);
  for (const auto& pA : rA.paths) {
    for (const auto& pB : rB.paths) {
      if (budget != nullptr && (budget->expired() || budget->queries == 0)) {
        record(diagnostics, "search_budget");
        return false;
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
        continue;
      }
      if (budget != nullptr) {
        --budget->queries;
      }
      if (check_race(pA, pB, funcA, funcB, diagnostics, facts)) {
        if (creations != nullptr) {
          creations->clear();
          for (const auto* path : {&pA, &pB})
            for (const auto& event : *path)
              if (event.kind == EvKind::Create) creations->push_back(event.stmt);
        }
        return true;
      }
    }
  }
  if (loop_bound == 1) {
    auto bound = std::max(literal_loop_search_bound(funcA), literal_loop_search_bound(funcB));
    if (bound > 16) record(diagnostics, "literal_loop_search_hint");
    return confirm_race(funcA, stmtA, widthA, funcB, stmtB, widthB,
                        diagnostics, budget, creations, facts, bound);
  }
  return false;
}

}  // namespace bmc
}  // namespace analyzer
}  // namespace ikos
