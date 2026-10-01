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

/// 把锁/互斥量的指针 Value 解析到 (base global, const_off) 合成身份。变量偏移坍缩
/// 到 base（保守）；碰撞只会多丢 TP、不 FP。
inline std::uint64_t mutex_id_of_value(ar::Value* v) {
  std::int64_t const_off = 0;
  bool variable_off = false;
  std::uint64_t base = 0;
  for (int depth = 0; depth < 16; ++depth) {
    if (auto* gv = core::dyn_cast< ar::GlobalVariable >(v)) {
      base = reinterpret_cast< std::uint64_t >(gv);
      break;
    }
    auto* iv = core::dyn_cast< ar::InternalVariable >(v);
    if (iv == nullptr) {
      break;
    }
    ar::Statement* def = unique_def_bmc(iv);
    if (auto* ps = core::dyn_cast_or_null< ar::PointerShift >(def)) {
      // 累加常量字节偏移（`&m[3]` vs `&m[4]` 是不同锁）；变量下标（`m[i]`）
      // → 身份不确定，坍缩到 base（保守，只丢 TP 不 FP）。
      for (std::size_t i = 0; i < ps->num_terms(); ++i) {
        auto term = ps->term(i);
        auto* c = core::dyn_cast< ar::IntegerConstant >(term.second);
        if (c == nullptr) {
          variable_off = true;
          break;
        }
        const_off += term.first.to_z_number().to< std::int64_t >() *
                     c->value().to_z_number().to< std::int64_t >();
      }
      if (variable_off) {
        break;
      }
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
  if (base == 0) {
    return reinterpret_cast< std::uint64_t >(v);
  }
  if (variable_off) {
    return base;  // 变量偏移：坍缩到 base
  }
  // (base, const_off) 组合。碰撞只会让两把不同锁看起来同锁 → 多丢 TP、不 FP（sound）。
  std::uint64_t h = base;
  h ^= static_cast< std::uint64_t >(const_off) + 0x9e3779b97f4a7c15ULL +
       (h << 6) + (h >> 2);
  return h;
}

/// pthread_mutex_lock/unlock 的锁身份 = arg0。
inline std::uint64_t mutex_id(ar::CallBase* call) {
  if (call->arg_begin() == call->arg_end()) {
    return 0;
  }
  return mutex_id_of_value(*call->arg_begin());
}

/// 是否 pthread_cond_wait（含 AR 内禀 ar.pthread.cond.wait）。
inline bool is_cond_wait(ar::CallBase* call) {
  ar::FunctionPointerConstant* cst =
      core::dyn_cast< ar::FunctionPointerConstant >(call->called());
  if (cst == nullptr || cst->function() == nullptr) {
    return false;
  }
  const std::string& nm = cst->function()->name();
  return nm.find("pthread_cond_wait") != std::string::npos ||
         nm.find("pthread.cond.wait") != std::string::npos;
}

/// pthread_cond_wait(&cond, &mutex) 的 mutex 身份 = arg1（arg0 是 cond）。
inline std::uint64_t cond_wait_mutex_id(ar::CallBase* call) {
  auto arg = call->arg_begin();
  if (arg == call->arg_end()) {
    return 0;
  }
  ++arg;
  if (arg == call->arg_end()) {
    return 0;
  }
  return mutex_id_of_value(*arg);
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
    // pthread_cond_wait(&cond, &mutex)：POSIX 原子 release mutex → 阻塞 →
    // re-acquire。竞态判定等价于 unlock(mutex) 后 lock(mutex)（net 不变，但释放
    // 期间别的线程能拿锁）。signal/broadcast 无 HB（数据由 mutex 保护，与
    // SV-COMP 语义一致，见 condvar / 13-privatized_67）。
    if (auto* call = core::dyn_cast< ar::CallBase >(s)) {
      if (is_cond_wait(call)) {
        std::uint64_t m = cond_wait_mutex_id(call);
        current.push_back(BmcEvent{s, EvKind::Unlock, m});
        current.push_back(BmcEvent{s, EvKind::Lock, m});
      }
    }
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

/// 一个基本块是否含「能排序访问」的同步调用：mutex lock/unlock、create/join、
/// __VERIFIER_atomic_begin/end。pthread_mutex_init 等非排序调用不算——它们不影响
/// 竞态判定，只算「良性」循环体（如 05-lval_ls_01 的 init 循环）。
inline bool block_has_sync(ar::BasicBlock* bb) {
  for (auto sit = bb->begin(), send = bb->end(); sit != send; ++sit) {
    if (auto* call = core::dyn_cast< ar::CallBase >(*sit)) {
      EvKind kind;
      if (classify_mutex_call(call, kind)) {
        return true;  // mutex lock/unlock + __VERIFIER_atomic_begin/end
      }
      if (is_cond_wait(call)) {
        return true;  // cond_wait 释放/重获 mutex（同步）
      }
      ar::FunctionPointerConstant* cst =
          core::dyn_cast< ar::FunctionPointerConstant >(call->called());
      if (cst != nullptr && cst->function() != nullptr) {
        const std::string& nm = cst->function()->name();
        if (nm.find("pthread_create") != std::string::npos ||
            nm.find("pthread_join") != std::string::npos) {
          return true;  // create/join（HB 建边，未展开则漏）
        }
      }
    }
  }
  return false;
}

/// header 所在循环体（与 header 同环的块 + header 本身）是否含同步调用。
inline bool loop_has_sync(ar::Code* body, ar::BasicBlock* header) {
  if (block_has_sync(header)) {
    return true;
  }
  for (ar::BasicBlock* bb : *body) {
    if (bb == header) {
      continue;
    }
    std::vector< ar::BasicBlock* > fwd, back;
    if (cfg_reachable(header, bb, fwd) && cfg_reachable(bb, header, back)) {
      if (block_has_sync(bb)) {
        return true;
      }
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

  // 精确能力边界：target 的块若从某回边（循环）头前向可达，则要看循环会不会
  // 影响访问（incomplete → UNKNOWN）：
  //   - 访问在循环体内（target 块能回到 header）：迭代 0 可达需 N≥1（界分析未做）
  //     → 保守 incomplete。
  //   - 访问在循环之后：只有循环体含同步（mutex/create/join）才可能排序访问 →
  //     incomplete；良性循环体（init/算术，无同步）不影响竞态 → 照常判。
  std::vector< ar::BasicBlock* > headers;
  std::vector< ar::BasicBlock* > in_stack2;
  collect_loop_headers(body->entry_block(), in_stack2, headers);
  for (ar::BasicBlock* h : headers) {
    std::vector< ar::BasicBlock* > visited;
    if (!cfg_reachable(h, target->parent(), visited)) {
      continue;
    }
    std::vector< ar::BasicBlock* > back;
    if (cfg_reachable(target->parent(), h, back) || loop_has_sync(body, h)) {
      result.incomplete = true;
      break;
    }
  }
  return result;
}

}  // namespace bmc
}  // namespace analyzer
}  // namespace ikos
