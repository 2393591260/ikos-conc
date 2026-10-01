// BMC 模块 —— 路径敏感 CFG 展开器 + 事件提取（第一版）
//
// 从线程入口函数沿 CFG 路径敏感地枚举到目标访问的所有路径，提取：
//   - 内存事件：ar::Load（读）/ ar::Store（写）
//   - 同步事件：pthread_mutex_lock（Lock）/ unlock（Unlock）
//
// 第一版只处理 DAG（直线代码 + 分支，无回边）；循环（回边）后续用 bound 展开。

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <ikos/ar/semantic/code.hpp>
#include <ikos/ar/semantic/statement.hpp>
#include <ikos/core/support/cast.hpp>

namespace ikos {
namespace analyzer {
namespace bmc {

/// 事件种类。
enum class EvKind { Read, Write, Lock, Unlock };

/// __VERIFIER_atomic_begin/end 的合成全局锁 id（与 abstract checker 的
/// PSEUDO_ATOMIC_LOCK 同值）。伪原子段 = 一把所有线程共享的锁。
constexpr std::uint64_t PSEUDO_ATOMIC_LOCK = 0xA7E0AD1CULL;

/// 一个 BMC 事件（内存访问或锁同步）。
struct BmcEvent {
  ar::Statement* stmt;      // 对应的 AR 语句
  EvKind kind;
  std::uint64_t mutex = 0;  // Lock/Unlock 的锁对象语法同一性（arg0 指针）；内存事件为 0

  BmcEvent(ar::Statement* s, EvKind k, std::uint64_t m = 0)
      : stmt(s), kind(k), mutex(m) {}
};

/// 锁的语法同一性：把 arg0 解析到其底层全局变量，用 GlobalVariable 指针做身份。
/// 不能直接用 `*arg_begin()` 的裸指针——同一把锁在 lock / unlock 两个调用点
/// 的 arg 可能是不同的 InternalVariable（各带一条 PointerShift/Bitcast 定义链），
/// 裸指针会拆成两个假锁，破坏锁集平衡与共享锁判定。
inline ar::Statement* unique_def_bmc(ar::InternalVariable* iv) {
  ar::Code* code = iv->code();
  if (code == nullptr) {
    return nullptr;
  }
  ar::Statement* found = nullptr;
  for (ar::BasicBlock* bb : *code) {
    for (ar::Statement* s : *bb) {
      if (s->result_or_null() == iv) {
        if (found != nullptr) {
          return nullptr;  // phi：多个 def，当作叶子
        }
        found = s;
      }
    }
  }
  return found;
}

inline std::uint64_t mutex_id(ar::CallBase* call) {
  if (call->arg_begin() == call->arg_end()) {
    return 0;
  }
  ar::Value* v = *call->arg_begin();
  for (int depth = 0; depth < 16; ++depth) {
    if (auto* gv = core::dyn_cast< ar::GlobalVariable >(v)) {
      return reinterpret_cast< std::uint64_t >(gv);
    }
    auto* iv = core::dyn_cast< ar::InternalVariable >(v);
    if (iv == nullptr) {
      break;
    }
    ar::Statement* def = unique_def_bmc(iv);
    if (auto* ps = core::dyn_cast_or_null< ar::PointerShift >(def)) {
      v = ps->pointer();
    } else if (auto* un = core::dyn_cast_or_null< ar::UnaryOperation >(def)) {
      if (un->op() == ar::UnaryOperation::Bitcast ||
          un->op() == ar::UnaryOperation::PtrToUI ||
          un->op() == ar::UnaryOperation::UIToPtr) {
        v = un->operand();
      } else {
        break;
      }
    } else {
      break;
    }
  }
  return reinterpret_cast< std::uint64_t >(*call->arg_begin());
}

/// 判断一个 CallBase 是否是对 pthread_mutex 的 lock/unlock，返回事件种类；
/// 非锁调用返回 false。
inline bool classify_mutex_call(ar::CallBase* call, EvKind& kind) {
  ar::FunctionPointerConstant* cst =
      core::dyn_cast< ar::FunctionPointerConstant >(call->called());
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
  // __VERIFIER_atomic_begin/end = 伪原子段（PSEUDO_ATOMIC_LOCK），与 abstract
  // checker 同语义（concurrent_semantics.hpp SectionBegin/End）。
  if (nm.find("__VERIFIER_atomic_begin") != std::string::npos) {
    kind = EvKind::Lock;
    return true;
  }
  if (nm.find("__VERIFIER_atomic_end") != std::string::npos) {
    kind = EvKind::Unlock;
    return true;
  }
  return false;
}

/// 判断一个 CallBase 是否是对 __VERIFIER_atomic_begin/end 的调用。
inline bool is_verifier_atomic(ar::CallBase* call) {
  ar::FunctionPointerConstant* cst =
      core::dyn_cast< ar::FunctionPointerConstant >(call->called());
  if (cst == nullptr || cst->function() == nullptr) {
    return false;
  }
  const std::string& nm = cst->function()->name();
  return nm.find("__VERIFIER_atomic_begin") != std::string::npos ||
         nm.find("__VERIFIER_atomic_end") != std::string::npos;
}

/// 把一个语句分类成 BMC 事件（内存/锁/无关）。
inline bool classify_event(ar::Statement* stmt, BmcEvent& ev) {
  if (core::isa< ar::Load >(stmt)) {
    ev = BmcEvent{stmt, EvKind::Read};
    return true;
  }
  if (core::isa< ar::Store >(stmt)) {
    ev = BmcEvent{stmt, EvKind::Write};
    return true;
  }
  if (auto* call = core::dyn_cast< ar::CallBase >(stmt)) {
    EvKind kind;
    if (classify_mutex_call(call, kind)) {
      std::uint64_t mid =
          is_verifier_atomic(call) ? PSEUDO_ATOMIC_LOCK : mutex_id(call);
      ev = BmcEvent{stmt, kind, mid};
      return true;
    }
  }
  return false;
}

/// 路径敏感的 DFS：从 `bb` 出发，沿 CFG 后继枚举到 `target` 的所有路径。
/// `current` 是当前路径上的事件前缀；到达 `target` 时把路径记入 `out` 并停止
/// （target 之后的语句/后继与本次访问的竞争判定无关）。
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
      in_stack.pop_back();
      return;
    }
  }
  for (auto succ = bb->successor_begin(), succ_end = bb->successor_end();
       succ != succ_end; ++succ) {
    dfs_paths(*succ, target, current, out, in_stack);
  }

  in_stack.pop_back();
}

/// 收集 CFG 里所有回边（循环）头：DFS 中回边 succ（已在栈上）的 succ 即循环头。
inline void collect_loop_headers(ar::BasicBlock* bb,
                                 std::vector< ar::BasicBlock* >& in_stack,
                                 std::vector< ar::BasicBlock* >& headers) {
  for (ar::BasicBlock* on_stack : in_stack) {
    if (on_stack == bb) {
      return;
    }
  }
  in_stack.push_back(bb);
  for (auto succ = bb->successor_begin(), succ_end = bb->successor_end();
       succ != succ_end; ++succ) {
    bool on_stack = false;
    for (ar::BasicBlock* os : in_stack) {
      if (os == *succ) {
        on_stack = true;
        break;
      }
    }
    if (on_stack) {
      headers.push_back(*succ);
    } else {
      collect_loop_headers(*succ, in_stack, headers);
    }
  }
  in_stack.pop_back();
}

/// tgt 是否从 src 沿 CFG 前向可达。
inline bool cfg_reachable(ar::BasicBlock* src, ar::BasicBlock* tgt,
                          std::vector< ar::BasicBlock* >& visited) {
  if (src == tgt) {
    return true;
  }
  for (ar::BasicBlock* v : visited) {
    if (v == src) {
      return false;
    }
  }
  visited.push_back(src);
  for (auto succ = src->successor_begin(), succ_end = src->successor_end();
       succ != succ_end; ++succ) {
    if (cfg_reachable(*succ, tgt, visited)) {
      return true;
    }
  }
  return false;
}

/// 展开结果：路径集合 + 是否不完整（target 在循环体内或循环之后）。
struct UnrollResult {
  std::vector< std::vector< BmcEvent > > paths;
  bool incomplete = false;  // true = target 受某循环影响，循环内容未展开
};

/// 从函数入口到 `target` 的所有路径（事件序列）。
inline UnrollResult unroll_to(ar::Function* func, ar::Statement* target) {
  UnrollResult result;
  ar::Code* body = func->body_or_null();
  if (body == nullptr || !body->has_entry_block()) {
    return result;
  }
  std::vector< BmcEvent > current;
  std::vector< ar::BasicBlock* > in_stack;
  dfs_paths(body->entry_block(), target, current, result.paths, in_stack);

  // 精确能力边界：target 的块若从某回边（循环）头前向可达（=在循环体内或
  // 循环之后），则展开路径不完整（循环体可能藏 join/create/mutex）→ UNKNOWN。
  // 循环在 target 之后（不可达 target 块）或分叉支路上的循环不影响本访问。
  std::vector< ar::BasicBlock* > headers;
  std::vector< ar::BasicBlock* > in_stack2;
  collect_loop_headers(body->entry_block(), in_stack2, headers);
  for (ar::BasicBlock* h : headers) {
    std::vector< ar::BasicBlock* > visited;
    if (cfg_reachable(h, target->parent(), visited)) {
      result.incomplete = true;
      break;
    }
  }
  return result;
}

}  // namespace bmc
}  // namespace analyzer
}  // namespace ikos
