// BMC 模块 —— 顶层 API：原子检测 + 竞争确认（供 checker 集成调用）
//
// confirm_race：对 checker 报的一对访问，先检测函数里有没有 BMC 没建模的
// 同步（C11 原子 / __VERIFIER_atomic），有则保守返回 false（不能确认）；
// 否则路径敏感展开两线程路径 + check_race 编码判定。

#pragma once

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

/// 检测一条展开路径上有没有「锁不平衡」（某把锁 lock 次数 > unlock 次数，即永久持有，
/// 如 time_var_mutex 的 m_busy）。有则 BMC 不能 sound 地判竞争。
inline bool has_unbalanced_lock(const std::vector< BmcEvent >& path) {
  std::map< std::uint64_t, int > balance;
  for (const BmcEvent& ev : path) {
    if (ev.kind == EvKind::Lock) {
      ++balance[ev.mutex];
    } else if (ev.kind == EvKind::Unlock) {
      --balance[ev.mutex];
    }
  }
  for (const auto& kv : balance) {
    if (kv.second != 0) {
      return true;
    }
  }
  return false;
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
  // 存在任意一对路径（A 的某条、B 的某条）可竞争，即真竞争。
  // 跳过锁不平衡的路径（永久锁 time_var_mutex，不能 sound 判）。
  for (const auto& pA : rA.paths) {
    if (has_unbalanced_lock(pA)) {
      continue;
    }
    for (const auto& pB : rB.paths) {
      if (has_unbalanced_lock(pB)) {
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
