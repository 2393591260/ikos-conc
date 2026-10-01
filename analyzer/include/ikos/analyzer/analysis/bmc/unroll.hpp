// BMC 模块 —— 路径敏感 CFG 展开器 + 事件提取（第一版）
//
// 从线程入口函数沿 CFG 路径敏感地枚举到目标访问的所有路径，提取：
//   - 内存事件：ar::Load（读）/ ar::Store（写）
//   - 同步事件：pthread_mutex_lock（Lock）/ unlock（Unlock）
//
// 第一版只处理 DAG（直线代码 + 分支，无回边）；循环（回边）后续用 bound 展开。

#pragma once

#include <string>
#include <vector>

#include <ikos/ar/semantic/code.hpp>
#include <ikos/ar/semantic/statement.hpp>

namespace ikos {
namespace analyzer {
namespace bmc {

/// 事件种类。
enum class EvKind { Read, Write, Lock, Unlock };

/// 一个 BMC 事件（内存访问或锁同步）。
struct BmcEvent {
  ar::Statement* stmt;  // 对应的 AR 语句
  EvKind kind;

  BmcEvent(ar::Statement* s, EvKind k) : stmt(s), kind(k) {}
};

/// 判断一个 CallBase 是否是对 pthread_mutex 的 lock/unlock，返回事件种类；
/// 非锁调用返回 false。
inline bool classify_mutex_call(ar::CallBase* call, EvKind& kind) {
  ar::FunctionPointerConstant* cst =
      dyn_cast< ar::FunctionPointerConstant >(call->called());
  if (cst == nullptr || cst->function() == nullptr) {
    return false;
  }
  const std::string& nm = cst->function()->name();
  // Matches "pthread_mutex_lock" / "pthread_mutex_unlock" and the AR intrinsic
  // forms "ar.pthread.mutex.lock" / "ar.pthread.mutex.unlock".
  bool is_lock = nm.find("pthread_mutex_lock") != std::string::npos ||
                 nm.find("pthread.mutex.lock") != std::string::npos;
  bool is_unlock = nm.find("pthread_mutex_unlock") != std::string::npos ||
                   nm.find("pthread.mutex.unlock") != std::string::npos;
  if (is_lock) {
    kind = EvKind::Lock;
    return true;
  }
  if (is_unlock) {
    kind = EvKind::Unlock;
    return true;
  }
  return false;
}

/// 把一个语句分类成 BMC 事件（内存/锁/无关）。
inline bool classify_event(ar::Statement* stmt, BmcEvent& ev) {
  if (isa< ar::Load >(stmt)) {
    ev = BmcEvent{stmt, EvKind::Read};
    return true;
  }
  if (isa< ar::Store >(stmt)) {
    ev = BmcEvent{stmt, EvKind::Write};
    return true;
  }
  if (auto* call = dyn_cast< ar::CallBase >(stmt)) {
    EvKind kind;
    if (classify_mutex_call(call, kind)) {
      ev = BmcEvent{stmt, kind};
      return true;
    }
  }
  return false;
}

/// 路径敏感的 DFS：从 `bb` 出发，沿 CFG 后继枚举到 `target` 的所有路径。
/// `current` 是当前路径上的事件前缀；到达 `target` 时把路径记入 `out`。
/// `in_stack` 记录当前 DFS 栈上的基本块，用于检测回边（循环）并跳过——
/// 第一版不展开循环（DAG only），后续用 bound 展开。
void dfs_paths(ar::BasicBlock* bb, ar::Statement* target,
               std::vector< BmcEvent >& current,
               std::vector< std::vector< BmcEvent > >& out,
               std::vector< ar::BasicBlock* >& in_stack) {
  // 回边检测：bb 已在栈上 → 循环，跳过（不展开）。
  for (ar::BasicBlock* on_stack : in_stack) {
    if (on_stack == bb) {
      return;
    }
  }
  in_stack.push_back(bb);

  for (auto sit = bb->begin(), send = bb->end(); sit != send; ++sit) {
    ar::Statement* s = *sit;
    BmcEvent ev(nullptr, EvKind::Read);
    if (classify_event(s, ev)) {
      current.push_back(ev);
    }
    if (s == target) {
      out.push_back(current);
    }
  }
  for (auto succ = bb->successor_begin(), succ_end = bb->successor_end();
       succ != succ_end; ++succ) {
    dfs_paths(*succ, target, current, out, in_stack);
  }

  in_stack.pop_back();
}

/// 从函数入口到 `target` 的所有路径（事件序列）。
inline std::vector< std::vector< BmcEvent > > unroll_to(
    ar::Function* func, ar::Statement* target) {
  std::vector< std::vector< BmcEvent > > out;
  ar::Code* body = func->body_or_null();
  if (body == nullptr || !body->has_entry_block()) {
    return out;
  }
  std::vector< BmcEvent > current;
  std::vector< ar::BasicBlock* > in_stack;
  dfs_paths(body->entry_block(), target, current, out, in_stack);
  return out;
}

}  // namespace bmc
}  // namespace analyzer
}  // namespace ikos
