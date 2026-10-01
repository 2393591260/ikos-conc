// BMC 模块 —— Z3 编码：po + mutex 同步 + clock + 竞争查询（第一版）
//
// 对两个展开路径（各是一条事件序列，最后一个事件是竞争访问），编码 SC 模型：
//   - po：路径内相邻事件 clock 递增
//   - mutex 同步：同一把锁的 Unlock -> Lock（互斥：要么 A 先要么 B 先，用布尔选择）
//   - 竞争查询：两个竞争访问能否在线性化里相邻（clock 差 1）
//
// Soundness 约束（关键）：BMC 只对「没用到它没建模的同步」的程序报 FALSE。
// 第一版只建模 mutex + po，所以调用方必须先确认程序没有原子/锁无关同步
// （见 unroll.hpp 的 classify + 调用方的 atomic 检测），否则不能报 FALSE。

#pragma once

#include <z3++.h>

#include <map>
#include <vector>

#include "unroll.hpp"

namespace ikos {
namespace analyzer {
namespace bmc {

/// 判断两个访问（各线程路径的最后一个事件）是否真竞争。
/// 返回 true = SAT（可相邻，竞争）；false = UNSAT（不可相邻，无竞争）。
inline bool check_race(const std::vector< BmcEvent >& pathA,
                       const std::vector< BmcEvent >& pathB) {
  if (pathA.empty() || pathB.empty()) {
    return false;
  }

  z3::context ctx;
  z3::solver s(ctx);

  const std::size_t nA = pathA.size();
  const std::size_t nB = pathB.size();

  // clock 变量：A 的 nA 个 + B 的 nB 个。
  std::vector< z3::expr > clock;
  for (std::size_t i = 0; i < nA; ++i) {
    clock.push_back(ctx.int_const(("a" + std::to_string(i)).c_str()));
  }
  for (std::size_t i = 0; i < nB; ++i) {
    clock.push_back(ctx.int_const(("b" + std::to_string(i)).c_str()));
  }

  // po：路径内相邻事件 clock 递增。
  for (std::size_t i = 0; i + 1 < nA; ++i) {
    s.add(clock[i] < clock[i + 1]);
  }
  for (std::size_t i = 0; i + 1 < nB; ++i) {
    s.add(clock[nA + i] < clock[nA + i + 1]);
  }

  // mutex 同步（互斥）：收集每把锁在每个路径里的 Lock/Unlock 位置。
  std::map< std::uint64_t, std::size_t > lockA, unlockA, lockB, unlockB;
  for (std::size_t i = 0; i < nA; ++i) {
    if (pathA[i].kind == EvKind::Lock) {
      lockA[pathA[i].mutex] = i;
    } else if (pathA[i].kind == EvKind::Unlock) {
      unlockA[pathA[i].mutex] = i;
    }
  }
  for (std::size_t i = 0; i < nB; ++i) {
    if (pathB[i].kind == EvKind::Lock) {
      lockB[pathB[i].mutex] = i;
    } else if (pathB[i].kind == EvKind::Unlock) {
      unlockB[pathB[i].mutex] = i;
    }
  }

  // 对每一把在 A、B 都出现过的锁 m：二选一
  //   ord_m 真 -> Unlock_A(m) -> Lock_B(m)   （A 先）
  //   ord_m 假 -> Unlock_B(m) -> Lock_A(m)   （B 先）
  std::size_t ord_idx = 0;
  for (const auto& kv : unlockA) {
    std::uint64_t m = kv.first;
    if (!lockB.count(m) || !lockA.count(m) || !unlockB.count(m)) {
      continue;
    }
    z3::expr ord = ctx.bool_const(("ord" + std::to_string(ord_idx++)).c_str());
    s.add(z3::implies(ord, clock[kv.second] < clock[nA + lockB[m]]));
    s.add(z3::implies(!ord, clock[nA + unlockB[m]] < clock[lockA[m]]));
  }

  // 竞争查询：两个访问（各自路径最后一个事件）能否相邻。
  bool any = false;
  for (int dir = 0; dir < 2; ++dir) {
    s.push();
    if (dir == 0) {
      s.add(clock[nA - 1] == clock[nA + nB - 1] + 1);
    } else {
      s.add(clock[nA + nB - 1] == clock[nA - 1] + 1);
    }
    if (s.check() == z3::sat) {
      any = true;
    }
    s.pop();
  }
  return any;
}

}  // namespace bmc
}  // namespace analyzer
}  // namespace ikos
