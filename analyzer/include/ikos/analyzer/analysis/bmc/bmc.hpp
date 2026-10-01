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

/// 检测一个函数里有没有 BMC 第一版没建模的同步：
///   - C11 原子（atomic_int 等，Load/Store 的 ordering != NotAtomic）
///   - __VERIFIER_atomic_begin/end（伪原子段，BMC 还没建模）
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
          if (cst->function()->name().find("__VERIFIER_atomic") !=
              std::string::npos) {
            return true;
          }
        }
      }
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
  std::vector< std::vector< BmcEvent > > pathsA = unroll_to(funcA, stmtA);
  std::vector< std::vector< BmcEvent > > pathsB = unroll_to(funcB, stmtB);
  // 存在任意一对路径（A 的某条、B 的某条）可竞争，即真竞争。
  for (const auto& pA : pathsA) {
    for (const auto& pB : pathsB) {
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
