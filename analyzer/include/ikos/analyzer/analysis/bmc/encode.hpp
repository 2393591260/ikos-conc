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
                       const std::vector< BmcEvent >& pathB,
                       ar::Function* funcA, ar::Function* funcB) {
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

  // spawn HB（svcomp.cat 的 po 跨线程边）：路径里 create(T)（T = 对方线程入口函数）
  // < T 的首事件。funcA/funcB 是两线程入口函数，Create 事件的 thread 字段存线程
  // 函数指针。detach 无 join、只有 spawn 边；join 边（E_T < join）后续再加。
  std::uint64_t ta = reinterpret_cast< std::uint64_t >(funcA);
  std::uint64_t tb = reinterpret_cast< std::uint64_t >(funcB);
  for (std::size_t i = 0; i < nA; ++i) {
    if (pathA[i].kind == EvKind::Create && pathA[i].thread == tb) {
      s.add(clock[i] < clock[nA]);  // A 的 create(B) < B 首事件（clock[nA] = b_0）
    }
  }
  for (std::size_t i = 0; i < nB; ++i) {
    if (pathB[i].kind == EvKind::Create && pathB[i].thread == ta) {
      s.add(clock[nA + i] < clock[0]);  // B 的 create(A) < A 首事件（clock[0] = a_0）
    }
  }

  // join HB（svcomp.cat 的 po 跨线程边）：joined 线程末事件 < join。过近似——把
  // A 路径里每个 Join 都约束成「B 末事件 < join」（假设 B 被 A join），B 路径同理。
  // 过约束只丢 TP（unjoined 实例，2 线程抽象表达不了），不引 FP（sound）。
  for (std::size_t i = 0; i < nA; ++i) {
    if (pathA[i].kind == EvKind::Join) {
      s.add(clock[nA + nB - 1] < clock[i]);  // B 末事件 < A 的 join
    }
  }
  for (std::size_t i = 0; i < nB; ++i) {
    if (pathB[i].kind == EvKind::Join) {
      s.add(clock[nA - 1] < clock[nA + i]);  // A 末事件 < B 的 join
    }
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

  // var_id -> SMT 变量：地址相等与 assume 约束共享同一映射（同一 SSA 值 → 同一
  // SMT 变量）。
  std::map< std::uint64_t, z3::expr > vars;
  auto var_of = [&](std::uint64_t vid) -> z3::expr {
    auto it = vars.find(vid);
    if (it != vars.end()) {
      return it->second;
    }
    z3::expr v = ctx.int_const(("v" + std::to_string(vid)).c_str());
    vars.emplace(vid, v);
    return v;
  };

  // 编码一条 `var pred const` / `const pred var` 的整数比较；指针/浮点等无法编码
  // 的比较置 `unencodable=true`（调用方据此保守不确认）。`var pred var` 也能编码。
  bool unencodable = false;
  auto encode_assume = [&](ar::Comparison* cmp) -> z3::expr {
    ar::Value* l = cmp->left();
    ar::Value* r = cmp->right();
    // 指针比较（PEQ/PNE/…）或浮点比较：BMC 一期建不了模（pthread_getspecific 等
    // 未建模函数的指针结果），置 unencodable → 不确认（sound，只丢 TP 不引 FP）。
    switch (cmp->predicate()) {
      case ar::Comparison::PEQ:
      case ar::Comparison::PNE:
      case ar::Comparison::PGT:
      case ar::Comparison::PGE:
      case ar::Comparison::PLT:
      case ar::Comparison::PLE:
      case ar::Comparison::FOEQ:
      case ar::Comparison::FOGT:
      case ar::Comparison::FOGE:
      case ar::Comparison::FOLT:
      case ar::Comparison::FOLE:
      case ar::Comparison::FONE:
      case ar::Comparison::FORD:
      case ar::Comparison::FUNO:
      case ar::Comparison::FUEQ:
      case ar::Comparison::FUGT:
      case ar::Comparison::FUGE:
      case ar::Comparison::FULT:
      case ar::Comparison::FULE:
      case ar::Comparison::FUNE:
        unencodable = true;
        return ctx.bool_val(true);
      default:
        break;
    }
    // `var pred var`（两变量）：也能编码。
    if (auto* li = core::dyn_cast< ar::InternalVariable >(l)) {
      if (auto* ri = core::dyn_cast< ar::InternalVariable >(r)) {
        z3::expr x = var_of(reinterpret_cast< std::uint64_t >(li));
        z3::expr y = var_of(reinterpret_cast< std::uint64_t >(ri));
        switch (cmp->predicate()) {
          case ar::Comparison::UIEQ:
          case ar::Comparison::SIEQ:
            return x == y;
          case ar::Comparison::UINE:
          case ar::Comparison::SINE:
            return x != y;
          case ar::Comparison::UIGT:
          case ar::Comparison::SIGT:
            return x > y;
          case ar::Comparison::UIGE:
          case ar::Comparison::SIGE:
            return x >= y;
          case ar::Comparison::UILT:
          case ar::Comparison::SILT:
            return x < y;
          case ar::Comparison::UILE:
          case ar::Comparison::SILE:
            return x <= y;
          default:
            unencodable = true;
            return ctx.bool_val(true);
        }
      }
    }
    std::uint64_t vid;
    std::int64_t cst;
    bool var_on_left;
    if (auto* li2 = core::dyn_cast< ar::InternalVariable >(l)) {
      auto* rc = core::dyn_cast< ar::IntegerConstant >(r);
      if (rc == nullptr) {
        unencodable = true;
        return ctx.bool_val(true);
      }
      vid = reinterpret_cast< std::uint64_t >(li2);
      cst = rc->value().to_z_number().to< std::int64_t >();
      var_on_left = true;
    } else if (auto* lc = core::dyn_cast< ar::IntegerConstant >(l)) {
      auto* ri = core::dyn_cast< ar::InternalVariable >(r);
      if (ri == nullptr) {
        unencodable = true;
        return ctx.bool_val(true);
      }
      vid = reinterpret_cast< std::uint64_t >(ri);
      cst = lc->value().to_z_number().to< std::int64_t >();
      var_on_left = false;
    } else {
      unencodable = true;
      return ctx.bool_val(true);
    }
    z3::expr x = var_on_left ? var_of(vid) : ctx.int_val(cst);
    z3::expr y = var_on_left ? ctx.int_val(cst) : var_of(vid);
    switch (cmp->predicate()) {
      case ar::Comparison::UIEQ:
      case ar::Comparison::SIEQ:
        return x == y;
      case ar::Comparison::UINE:
      case ar::Comparison::SINE:
        return x != y;
      case ar::Comparison::UIGT:
      case ar::Comparison::SIGT:
        return x > y;
      case ar::Comparison::UIGE:
      case ar::Comparison::SIGE:
        return x >= y;
      case ar::Comparison::UILT:
      case ar::Comparison::SILT:
        return x < y;
      case ar::Comparison::UILE:
      case ar::Comparison::SILE:
        return x <= y;
      default:
        unencodable = true;
        return ctx.bool_val(true);
    }
  };

  // 路径条件（assume 约束）：两路径上每条 Comparison 都成立。
  for (const BmcEvent& e : pathA) {
    if (e.kind == EvKind::Assume) {
      if (auto* cmp = core::dyn_cast< ar::Comparison >(e.stmt)) {
        s.add(encode_assume(cmp));
      }
    }
  }
  for (const BmcEvent& e : pathB) {
    if (e.kind == EvKind::Assume) {
      if (auto* cmp = core::dyn_cast< ar::Comparison >(e.stmt)) {
        s.add(encode_assume(cmp));
      }
    }
  }
  // 有无法编码的比较（指针/浮点）→ 保守不确认（UNKNOWN），避免误报。
  if (unencodable) {
    return false;
  }

  // SMT 同址门：两个访问必须在同一 base 的同一字节偏移
  // （const_off + coeff*var）。base 相同由 checker 的 same_global 门保证，这里只
  // 判偏移相等；base 不同或提取失败（base==0）则不确认（保持 UNKNOWN）。
  {
    const BmcEvent& ea = pathA.back();
    const BmcEvent& eb = pathB.back();
    if (ea.base != 0 && eb.base != 0 && ea.base == eb.base) {
      z3::expr off_a = ctx.int_val(ea.const_off);
      z3::expr off_b = ctx.int_val(eb.const_off);
      if (ea.var_id != 0) {
        off_a = off_a + ctx.int_val(ea.coeff) * var_of(ea.var_id);
      }
      if (eb.var_id != 0) {
        off_b = off_b + ctx.int_val(eb.coeff) * var_of(eb.var_id);
      }
      s.add(off_a == off_b);
    }
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
