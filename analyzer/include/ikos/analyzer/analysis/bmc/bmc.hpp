// BMC 模块 —— 顶层 API：原子检测 + 竞争确认（供 checker 集成调用）
//
// confirm_race：对 checker 报的一对访问，先检测函数里有没有 BMC 没建模的
// 同步（C11 原子 / __VERIFIER_atomic），有则保守返回 false（不能确认）；
// 否则路径敏感展开两线程路径 + check_race 编码判定。

#pragma once

#include <set>
#include <string>
#include <vector>

#include "encode.hpp"
#include "unroll.hpp"

namespace ikos {
namespace analyzer {
namespace bmc {

/// 检测一个函数里有没有 BMC 第一期没建模的同步：
///   - C11 原子（atomic_int 等，Load/Store 的 ordering != NotAtomic）
///   - __VERIFIER_atomic_begin/end（伪原子段）
///   - pthread_cond_*（条件变量 signal/wait 建 HB）
///   - sem_*（信号量 post/wait 建 HB）
///   - pthread_barrier_* / pthread_spin_*（屏障/自旋锁）
/// 有则返回 true（调用方必须报 UNKNOWN，不能报 FALSE）。
inline bool has_unmodeled_sync(ar::Function* func) {
  ar::Code* body = func->body_or_null();
  if (body == nullptr) {
    return false;
  }
  for (auto bit = body->begin(), bend = body->end(); bit != bend; ++bit) {
    for (auto sit = (*bit)->begin(), send = (*bit)->end(); sit != send; ++sit) {
      ar::Statement* s = *sit;
      if (auto* load = core::dyn_cast< ar::Load >(s)) {
        if (load->is_atomic()) {
          return true;
        }
      } else if (auto* store = core::dyn_cast< ar::Store >(s)) {
        if (store->is_atomic()) {
          return true;
        }
      } else if (auto* call = core::dyn_cast< ar::CallBase >(s)) {
        ar::FunctionPointerConstant* cst =
            core::dyn_cast< ar::FunctionPointerConstant >(call->called());
        if (cst != nullptr && cst->function() != nullptr) {
          const std::string& nm = cst->function()->name();
          if (nm.find("__VERIFIER_atomic") != std::string::npos ||
              nm.find("pthread_cond") != std::string::npos ||
              nm.find("pthread_barrier") != std::string::npos ||
              nm.find("pthread_spin") != std::string::npos ||
              nm.rfind("sem_", 0) == 0) {
            return true;
          }
        }
      }
    }
  }
  return false;
}

/// 检测整个程序里有没有 BMC 第一期没建模的同步（保守：任一函数有 → 整个程序报 UNKNOWN）。
inline bool has_unmodeled_sync(ar::Bundle* bundle) {
  for (auto fit = bundle->function_begin(), fend = bundle->function_end();
       fit != fend; ++fit) {
    if ((*fit)->is_definition() && has_unmodeled_sync(*fit)) {
      return true;
    }
  }
  return false;
}

/// 一条展开路径末尾仍持有的锁（lock 次数 > unlock 次数 = 访问时在临界区内）。
inline std::set< std::uint64_t > held_locks(const std::vector< BmcEvent >& path) {
  std::map< std::uint64_t, int > balance;
  for (const BmcEvent& ev : path) {
    if (ev.kind == EvKind::Lock) {
      ++balance[ev.mutex];
    } else if (ev.kind == EvKind::Unlock) {
      --balance[ev.mutex];
    }
  }
  std::set< std::uint64_t > held;
  for (const auto& kv : balance) {
    if (kv.second > 0) {
      held.insert(kv.first);
    }
  }
  return held;
}

/// 整个函数里出现过的所有锁（任一分支上的 lock/unlock 都算）。
/// 用「函数级」而非「路径级」：标志式 HB（time_var_mutex 的 busy 标志、
/// privatized 的 trace）靠一把在两个线程里都出现的锁保护标志变量，这把锁
/// 可能在另一条分支上才被当前线程使用，路径级 used 会漏掉它 → 误判竞争。
inline std::set< std::uint64_t > func_used_locks(ar::Function* func) {
  std::set< std::uint64_t > locks;
  ar::Code* body = func->body_or_null();
  if (body == nullptr) {
    return locks;
  }
  for (auto bit = body->begin(), bend = body->end(); bit != bend; ++bit) {
    for (auto sit = (*bit)->begin(), send = (*bit)->end(); sit != send; ++sit) {
      if (auto* call = core::dyn_cast< ar::CallBase >(*sit)) {
        EvKind kind;
        if (classify_mutex_call(call, kind)) {
          locks.insert(mutex_id(call));
        }
      }
    }
  }
  return locks;
}

/// 确认两个访问（在各自线程入口函数里）是否真竞争。
/// funcA/funcB = 两访问所在的线程入口函数；stmtA/stmtB = 两访问语句。
/// 返回 true = 确认竞争（报 FALSE 找回 TP）；false = 不确认（保持 UNKNOWN）。
inline bool confirm_race(ar::Function* funcA, ar::Statement* stmtA,
                         ar::Function* funcB, ar::Statement* stmtB) {
  // Soundness 门：有任何未建模同步，就不能报 FALSE。
  if (has_unmodeled_sync(funcA) || has_unmodeled_sync(funcB)) {
    return false;
  }
  UnrollResult rA = unroll_to(funcA, stmtA);
  UnrollResult rB = unroll_to(funcB, stmtB);
  // Soundness 门：展开遇到回边（循环）→ 路径不完整，循环里可能藏着
  // join/create/mutex 等未展开的同步（如 pthread-numerical-integration 的
  // join 循环），不能 sound 判 FALSE → 保守 UNKNOWN（能力边界，后续加 bound 展开）。
  if (rA.incomplete || rB.incomplete) {
    return false;
  }
  // 锁门（sound）：跳过「一方访问时持有的锁，被另一方函数用到」的路径对——
  // 这种锁可能承载标志式 HB / 永久锁（time_var_mutex、privatized），互斥未完整
  // 建模，可能排序两个访问，保守 UNKNOWN。已配对（balanced）的共享锁由
  // check_race 的互斥析取建模；完全不共享的锁（经典不同锁竞争）不约束对方，
  // 照常判。
  std::set< std::uint64_t > funcLocksA = func_used_locks(funcA);
  std::set< std::uint64_t > funcLocksB = func_used_locks(funcB);
  for (const auto& pA : rA.paths) {
    std::set< std::uint64_t > heldA = held_locks(pA);
    for (const auto& pB : rB.paths) {
      std::set< std::uint64_t > heldB = held_locks(pB);
      bool shared_held = false;
      for (std::uint64_t m : heldA) {
        if (funcLocksB.count(m)) {
          shared_held = true;
          break;
        }
      }
      if (!shared_held) {
        for (std::uint64_t m : heldB) {
          if (funcLocksA.count(m)) {
            shared_held = true;
            break;
          }
        }
      }
      if (shared_held) {
        continue;
      }
      if (check_race(pA, pB)) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace bmc
}  // namespace analyzer
}  // namespace ikos
