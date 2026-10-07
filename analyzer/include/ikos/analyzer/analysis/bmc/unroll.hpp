// BMC 模块 —— 路径敏感 CFG 展开器 + 事件提取（第一版）
//
// 从线程入口函数沿 CFG 路径敏感地枚举到目标访问的所有路径，提取：
//   - 内存事件：ar::Load（读）/ ar::Store（写）
//   - 同步事件：pthread_mutex_lock（Lock）/ unlock（Unlock）
//
// 第一版只处理 DAG（直线代码 + 分支，无回边）；循环（回边）后续用 bound 展开。

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <ikos/ar/semantic/code.hpp>
#include <ikos/ar/semantic/statement.hpp>
#include <ikos/analyzer/analysis/execution_engine/symbolic_index.hpp>
#include <ikos/core/support/cast.hpp>

namespace ikos {
namespace analyzer {
namespace bmc {

/// 事件种类。
enum class EvKind { Read, Write, Lock, Unlock, Assume, Create, Join };

/// __VERIFIER_atomic_begin/end 的合成全局锁 id（与 abstract checker 的
/// PSEUDO_ATOMIC_LOCK 同值）。伪原子段 = 一把所有线程共享的锁。
constexpr std::uint64_t PSEUDO_ATOMIC_LOCK = 0xA7E0AD1CULL;

/// 一个 BMC 事件（内存访问或锁同步）。
struct BmcEvent {
  ar::Statement* stmt = nullptr;  // 对应的 AR 语句
  EvKind kind = EvKind::Read;
  std::uint64_t mutex = 0;  // Lock/Unlock 的锁对象语法同一性（arg0 指针）；内存事件为 0
  // 符号地址（SMT 同址门）：base = 全局数组指针身份；const_off = 常量字节偏移；
  // var_id/coeff = 单个变量下标项 `coeff * var`（var_id==0 ⟹ 纯常量偏移）。
  // 提取失败时保持 base==0（调用方保守不确认）。
  std::uint64_t base = 0;
  std::int64_t const_off = 0;
  std::uint64_t var_id = 0;
  std::int64_t coeff = 0;
  // Create/Join 的线程身份：Create = 线程函数指针（reinterpret_cast）；Join =
  // 句柄身份（mutex_id_of_value 同款解析）。仅 Create/Join 事件用，其余为 0。
  std::uint64_t thread = 0;

  BmcEvent() = default;
  BmcEvent(ar::Statement* s, EvKind k, std::uint64_t m = 0)
      : stmt(s), kind(k), mutex(m) {}
};

/// 间接调用的候选目标解析器（函数指针）：data_race.cpp 用 FunctionPointerAnalysis
/// 结果填充，把 `call %fp()` 的 fp 解析成可能的目标函数集合。默认空 = 不解析间接
/// 调用（保守，只丢 TP 不引 FP）。
using IndirectCalleeResolver =
    std::function< std::vector< ar::Function* >(ar::CallBase*) >;

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

/// 路径敏感 def 解析：单 def 直接返回；phi（多 def）选「当前 DFS 路径 in_stack
/// 经过的前驱块」的 def —— 把 nondet 指针（04-mutex_23 的 unlock(m)，m 在
/// #2=bitcast @mutex2 / #3=bitcast @mutex1 两个前驱块各 def 一次）按路径解析到
/// 具体那把锁，而不是坍缩成裸指针假锁。找不到（无 def / phi 但路径块都不含 def）
/// 返回 nullptr（保守，沿用旧坍缩行为，只丢 TP 不引 FP）。
inline ar::Statement* unique_def_bmc_on_path(
    ar::InternalVariable* iv, const std::vector< ar::BasicBlock* >& in_stack) {
  ar::Code* code = iv->code();
  if (code == nullptr) {
    return nullptr;
  }
  std::vector< ar::Statement* > defs;
  for (ar::BasicBlock* bb : *code) {
    for (ar::Statement* s : *bb) {
      if (s->result_or_null() == iv) {
        defs.push_back(s);
      }
    }
  }
  if (defs.size() == 1) {
    return defs[0];
  }
  if (defs.empty()) {
    return nullptr;
  }
  // phi：选 def 所在块在当前 DFS 路径（in_stack）上的那个。
  for (ar::Statement* s : defs) {
    for (ar::BasicBlock* b : in_stack) {
      if (s->parent() == b) {
        return s;
      }
    }
  }
  return nullptr;
}

/// 把锁/互斥量的指针 Value 解析到 (base global, const_off) 合成身份。变量偏移坍缩
/// 到 base（保守）；碰撞只会多丢 TP、不 FP。传入 in_stack（非 null）时 phi 按路径
/// 敏感解析（见 unique_def_bmc_on_path）；null（默认）沿用旧的「phi=叶子」坍缩。
inline std::uint64_t mutex_id_of_value(
    ar::Value* v, const std::vector< ar::BasicBlock* >* in_stack = nullptr) {
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
    ar::Statement* def = in_stack != nullptr
                             ? unique_def_bmc_on_path(iv, *in_stack)
                             : unique_def_bmc(iv);
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
inline std::uint64_t mutex_id(
    ar::CallBase* call, const std::vector< ar::BasicBlock* >* in_stack = nullptr) {
  if (call->arg_begin() == call->arg_end()) {
    return 0;
  }
  return mutex_id_of_value(*call->arg_begin(), in_stack);
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

/// 提取一次内存访问指针的符号字节偏移：沿 def-chain 穿透 PointerShift / 值保持
/// cast，累加常量项（stride*const）与至多一个变量下标项（stride*var）。下标只认
/// 直接 InternalVariable；仿射（i+1）/多变量/phi 等不干净的形态返回 false（base
/// 保持 0，调用方保守不确认，只丢 TP 不引 FP）。
inline bool symbolic_byte_offset(ar::Value* ptr, BmcEvent& ev) {
  for (int depth = 0; depth < 16; ++depth) {
    if (auto* gv = core::dyn_cast< ar::GlobalVariable >(ptr)) {
      ev.base = reinterpret_cast< std::uint64_t >(gv);
      return true;
    }
    auto* iv = core::dyn_cast< ar::InternalVariable >(ptr);
    if (iv == nullptr) {
      return false;
    }
    ar::Statement* def = unique_def_bmc(iv);
    if (auto* ps = core::dyn_cast_or_null< ar::PointerShift >(def)) {
      for (std::size_t i = 0; i < ps->num_terms(); ++i) {
        auto term = ps->term(i);
        std::int64_t stride = term.first.to_z_number().to< std::int64_t >();
        if (auto* ic = core::dyn_cast< ar::IntegerConstant >(term.second)) {
          ev.const_off +=
              stride * ic->value().to_z_number().to< std::int64_t >();
        } else if (auto* viv =
                       core::dyn_cast< ar::InternalVariable >(term.second)) {
          if (ev.var_id != 0 &&
              ev.var_id != reinterpret_cast< std::uint64_t >(viv)) {
            return false; // 多变量下标：不干净
          }
          ev.var_id = reinterpret_cast< std::uint64_t >(viv);
          ev.coeff += stride;
        } else {
          // 仿射下标（i+1、2*i 等）：resolve_linear_index 解析成 coeff*var+const。
          LinearIndex li;
          if (!resolve_linear_index(term.second, li)) {
            return false;
          }
          if (ev.var_id != 0 && ev.var_id != li.var_id) {
            return false; // 多变量下标：不干净
          }
          ev.var_id = li.var_id;
          ev.coeff += stride * li.coeff;
          ev.const_off += stride * li.const_term;
        }
      }
      ptr = ps->pointer();
      continue;
    }
    if (auto* un = core::dyn_cast_or_null< ar::UnaryOperation >(def)) {
      switch (un->op()) {
        case ar::UnaryOperation::Bitcast:
        case ar::UnaryOperation::PtrToUI:
        case ar::UnaryOperation::UIToPtr:
          ptr = un->operand();
          continue;
        default:
          return false;
      }
    }
    return false;
  }
  return false;
}

/// 判断一个 CallBase 是否是 assume 语义调用（assume_abort_if_not / __VERIFIER_assume）。
/// 它们「assume 实参为真」（假分支 abort/不可达），不能当普通调用跳过。
inline bool is_assume_call(ar::CallBase* call) {
  auto* cst = core::dyn_cast< ar::FunctionPointerConstant >(call->called());
  if (cst == nullptr || cst->function() == nullptr) {
    return false;
  }
  const std::string& nm = cst->function()->name();
  return nm == "assume_abort_if_not" || nm == "__VERIFIER_assume";
}

/// 取 assume 实参的「正向比较」（cond 为真时成立的比较）。实参是某比较的结果，
/// 前端用 add_comparison 拆成真/假两块：真块里 `Comparison(pred,l,r); %c=true`、
/// 假块里 `Comparison(inverse(pred),l,r); %c=false`。先穿透 cast/变量赋值到达比较
/// 结果变量，再找真块里的正向 Comparison；找不到返回 nullptr（保守，不新增约束）。
inline ar::Comparison* forward_comparison_of(ar::Value* arg) {
  for (int depth = 0; depth < 16; ++depth) {
    auto* iv = core::dyn_cast< ar::InternalVariable >(arg);
    if (iv == nullptr) {
      return nullptr;
    }
    ar::Code* code = iv->code();
    if (code == nullptr) {
      return nullptr;
    }
    std::vector< ar::Statement* > defs;
    for (ar::BasicBlock* bb : *code) {
      for (ar::Statement* s : *bb) {
        if (s->result_or_null() == iv) {
          defs.push_back(s);
        }
      }
    }
    if (defs.empty()) {
      return nullptr;
    }
    if (defs.size() == 1) {
      // 单 def：cast / 变量赋值 → 继续 peel。
      ar::Statement* d = defs[0];
      if (auto* as = core::dyn_cast< ar::Assignment >(d)) {
        arg = as->operand();
        continue;
      }
      if (auto* un = core::dyn_cast< ar::UnaryOperation >(d)) {
        switch (un->op()) {
          case ar::UnaryOperation::ZExt:
          case ar::UnaryOperation::SExt:
          case ar::UnaryOperation::UTrunc:
          case ar::UnaryOperation::STrunc:
          case ar::UnaryOperation::Bitcast:
          case ar::UnaryOperation::PtrToUI:
          case ar::UnaryOperation::UIToPtr:
            arg = un->operand();
            continue;
          default:
            return nullptr;
        }
      }
      return nullptr;
    }
    // 多 def = 比较结果（真/假块）：找「iv = true」块里的正向比较。
    for (ar::BasicBlock* bb : *code) {
      for (auto it = bb->begin(); it != bb->end(); ++it) {
        ar::Statement* s = *it;
        if (s->result_or_null() != iv) {
          continue;
        }
        if (auto* as = core::dyn_cast< ar::Assignment >(s)) {
          auto* cst = core::dyn_cast< ar::IntegerConstant >(as->operand());
          if (cst != nullptr && !cst->value().is_zero()) {
            for (auto jt = bb->begin(); jt != it; ++jt) {
              if (auto* cmp = core::dyn_cast< ar::Comparison >(*jt)) {
                return cmp;
              }
            }
          }
        }
      }
    }
    return nullptr;
  }
  return nullptr;
}

/// 把一个语句分类成 BMC 事件（内存/锁/假设/无关）。
inline bool classify_event(
    ar::Statement* stmt, BmcEvent& ev,
    const std::vector< ar::BasicBlock* >* in_stack = nullptr) {
  if (auto* load = core::dyn_cast< ar::Load >(stmt)) {
    ev = BmcEvent{stmt, EvKind::Read};
    symbolic_byte_offset(load->operand(), ev);
    return true;
  }
  if (auto* store = core::dyn_cast< ar::Store >(stmt)) {
    ev = BmcEvent{stmt, EvKind::Write};
    symbolic_byte_offset(store->pointer(), ev);
    return true;
  }
  if (core::isa< ar::Comparison >(stmt)) {
    // 路径条件（assume 约束）：比较在 AR 里就是 refine 语句，路径上每条
    // Comparison 都成立，收集起来在 encode 阶段编码成 SMT 约束。
    ev = BmcEvent{stmt, EvKind::Assume};
    return true;
  }
  if (auto* call = core::dyn_cast< ar::CallBase >(stmt)) {
    EvKind kind;
    if (classify_mutex_call(call, kind)) {
      std::uint64_t mid =
          is_verifier_atomic(call) ? PSEUDO_ATOMIC_LOCK : mutex_id(call, in_stack);
      ev = BmcEvent{stmt, kind, mid};
      return true;
    }
    // pthread_create/pthread_join → Create/Join 事件（spawn/join HB 锚点）。
    ar::FunctionPointerConstant* cst =
        core::dyn_cast< ar::FunctionPointerConstant >(call->called());
    if (cst != nullptr && cst->function() != nullptr) {
      const std::string& nm = cst->function()->name();
      if (nm.find("pthread_create") != std::string::npos) {
        // arg2 = 线程函数（FunctionPointerConstant）。函数指针变量/间接线程函数
        // 提取失败 → 不产生 Create 事件（保守，只丢 TP）。
        auto it = call->arg_begin();
        ++it;
        ++it;
        if (it != call->arg_end()) {
          if (auto* fpc = core::dyn_cast< ar::FunctionPointerConstant >(*it)) {
            if (fpc->function() != nullptr) {
              ev = BmcEvent{stmt, EvKind::Create};
              ev.thread = reinterpret_cast< std::uint64_t >(fpc->function());
              return true;
            }
          }
        }
      } else if (nm.find("pthread_join") != std::string::npos) {
        // arg0 = pthread_t 句柄（mutex_id_of_value 同款解析到稳定身份）。
        if (call->arg_begin() != call->arg_end()) {
          ev = BmcEvent{stmt, EvKind::Join};
          ev.thread = mutex_id_of_value(*call->arg_begin(), in_stack);
          return true;
        }
      }
    }
  }
  return false;
}

/// 内联展开的最大调用深度（helper 内 helper 的递归上限，防爆炸）。
constexpr int MAX_INLINE_DEPTH = 8;

/// 内联时要跳过（不跟进）的函数：单调用原子（__VERIFIER_atomic_* / __atomic_* /
/// __sync_*）和 assume_abort_if_not。前者是 BMC 没建模的锁无关语义（跟进会把
/// 原子函数体里的裸访问暴露出来 → 误判），后者是纯假设调用。与 bmc.hpp 的
/// is_unmodeled_atomic_call 前缀对齐。
inline bool bmc_skip_inline(const std::string& nm) {
  if (nm.size() >= 18 && nm.compare(0, 18, "__VERIFIER_atomic_") == 0) {
    return true;
  }
  if (nm.size() >= 9 && nm.compare(0, 9, "__atomic_") == 0) {
    return true;
  }
  if (nm.size() >= 7 && nm.compare(0, 7, "__sync_") == 0) {
    return true;
  }
  return nm == "assume_abort_if_not";
}

/// 函数体是否含回边（循环）。helper 带循环时展开器（DAG-only）不能精确展开，
/// 跟进只会得到不完整路径 → 跳过不内联（保守 UNKNOWN）。
inline bool has_loop_dfs(ar::BasicBlock* bb,
                         std::vector< ar::BasicBlock* >& on_path,
                         std::vector< ar::BasicBlock* >& visited) {
  for (ar::BasicBlock* b : on_path) {
    if (b == bb) {
      return true;  // 回边
    }
  }
  for (ar::BasicBlock* b : visited) {
    if (b == bb) {
      return false;
    }
  }
  visited.push_back(bb);
  on_path.push_back(bb);
  for (auto succ = bb->successor_begin(), succ_end = bb->successor_end();
       succ != succ_end; ++succ) {
    if (has_loop_dfs(*succ, on_path, visited)) {
      on_path.pop_back();
      return true;
    }
  }
  on_path.pop_back();
  return false;
}

inline bool function_has_loop(ar::Function* f) {
  ar::Code* body = f->body_or_null();
  if (body == nullptr || !body->has_entry_block()) {
    return false;
  }
  std::vector< ar::BasicBlock* > on_path, visited;
  return has_loop_dfs(body->entry_block(), on_path, visited);
}

/// 函数体是否含 `__VERIFIER_atomic_begin/end`（原子区标记）。内联一个「把
/// __VERIFIER_atomic 段包在 helper 里」的函数（如 atomic_c_cas / Push）会把
/// 原子区里的写暴露出来，与调用方原子区外的快照读配对成「伪锁内写 vs 伪锁外读」
/// ——这是无锁协议（elimination_backoff_stack）的线性化签名，BMC 的伪锁模型
/// 建不了模，只能保守 UNKNOWN。所以这类 helper 不内联。
inline bool function_has_atomic_section(ar::Function* f) {
  ar::Code* body = f->body_or_null();
  if (body == nullptr) {
    return false;
  }
  for (ar::BasicBlock* bb : *body) {
    for (ar::Statement* s : *bb) {
      if (auto* call = core::dyn_cast< ar::CallBase >(s)) {
        ar::FunctionPointerConstant* cst =
            core::dyn_cast< ar::FunctionPointerConstant >(call->called());
        if (cst != nullptr && cst->function() != nullptr) {
          const std::string& nm = cst->function()->name();
          if (nm == "__VERIFIER_atomic_begin" ||
              nm == "__VERIFIER_atomic_end") {
            return true;
          }
        }
      }
    }
  }
  return false;
}

/// 一个函数体能否被 BMC 内联（有体 + 非单调用原子/assume + 无循环 + 不含原子区）。
inline bool inline_ok(ar::Function* f) {
  if (f == nullptr || f->body_or_null() == nullptr) {
    return false;  // extern（pthread_* / 无体）
  }
  if (bmc_skip_inline(f->name())) {
    return false;  // 原子单调用 / assume
  }
  if (function_has_loop(f)) {
    return false;  // 带循环的 helper
  }
  if (function_has_atomic_section(f)) {
    return false;  // helper 里包 __VERIFIER_atomic_begin/end（无锁协议）
  }
  return true;
}

/// 应被内联跟进的直接调用目标：直接函数指针调用 + 有函数体 + 非单调用原子 +
/// 无循环 + 不含原子区标记。否则返回 nullptr（不跟进，保守）。
inline ar::Function* direct_callee(ar::CallBase* call) {
  ar::FunctionPointerConstant* cst =
      core::dyn_cast< ar::FunctionPointerConstant >(call->called());
  if (cst == nullptr || cst->function() == nullptr) {
    return nullptr;
  }
  ar::Function* f = cst->function();
  return inline_ok(f) ? f : nullptr;
}

/// 常量界循环展开的上限（防 `for(i<1000000)` / 带分支循环体的 `B^N` 状态爆炸）。
/// 超过此界的常量界循环视作「未知界」→ 保守不展开（UNKNOWN）。
constexpr std::int64_t MAX_UNROLL = 8;

/// 判断 v 是否是循环归纳变量：
///   - 局部：v 有 def 形如 `v = (v op const)`（自增/自减，`for(i=0;i<N;i++)`）。
///   - 全局：v 的 def 是 `load @g`（Load of GlobalVariable），且循环体 body 里有
///     `store @g`（增量 `store @p`，fib_*-racy 的 p 是全局）。`if(i<5)` 的 i 两者都不是。
inline bool is_induction_var(ar::Value* v, ar::BasicBlock* body) {
  auto* iv = core::dyn_cast< ar::InternalVariable >(v);
  if (iv == nullptr) {
    return false;
  }
  ar::Code* code = iv->code();
  if (code == nullptr) {
    return false;
  }
  ar::GlobalVariable* loaded_gv = nullptr;  // v = load @g
  for (ar::BasicBlock* bb : *code) {
    for (ar::Statement* s : *bb) {
      if (s->result_or_null() != iv) {
        continue;
      }
      if (auto* as = core::dyn_cast< ar::Assignment >(s)) {
        // 局部：`%.0 = %11` 且 `%11 = %.0 + 1`（BinaryOperation 是 Statement，
        // 结果是 %11，Assignment 的 operand 是 %11 这个 InternalVariable）。
        auto* iv2 = core::dyn_cast< ar::InternalVariable >(as->operand());
        if (iv2 == nullptr) {
          continue;
        }
        auto* bo = core::dyn_cast_or_null< ar::BinaryOperation >(
            unique_def_bmc(iv2));
        if (bo != nullptr && (bo->left() == iv || bo->right() == iv)) {
          return true;  // v = v ± const
        }
      } else if (auto* load = core::dyn_cast< ar::Load >(s)) {
        // 全局：v = load @g
        if (auto* g = core::dyn_cast< ar::GlobalVariable >(load->operand())) {
          loaded_gv = g;
        }
      }
    }
  }
  // 全局：v = load @g 且循环体 body 里有 store @g（增量）。
  if (loaded_gv != nullptr && body != nullptr) {
    for (auto sit = body->begin(), se = body->end(); sit != se; ++sit) {
      if (auto* store = core::dyn_cast< ar::Store >(*sit)) {
        if (store->pointer() == loaded_gv) {
          return true;
        }
      }
    }
  }
  return false;
}

/// 常量界循环检测（仿 Dartagnan 的 LLVM -loop-unroll：`for(i=0;i<N;i++)` 完整展开）。
/// 扫 header 的后继块，body = 含 `Comparison(i <op> N)`（< / <=，N 常量）的块、
/// exit = 含 `i >= N` 的块；返回 N。非此形态 / 超大界 / 归纳变量不是自增 → -1。
inline std::int64_t constant_loop_bound(ar::BasicBlock* header,
                                        ar::BasicBlock*& body,
                                        ar::BasicBlock*& exit_bb) {
  body = nullptr;
  exit_bb = nullptr;
  ar::Value* ind_var = nullptr;
  std::int64_t body_bound = -1;
  std::int64_t exit_bound = -1;
  for (auto succ = header->successor_begin(), send = header->successor_end();
       succ != send; ++succ) {
    for (auto sit = (*succ)->begin(), se = (*succ)->end(); sit != se; ++sit) {
      auto* cmp = core::dyn_cast< ar::Comparison >(*sit);
      if (cmp == nullptr) {
        continue;
      }
      auto* rc = core::dyn_cast< ar::IntegerConstant >(cmp->right());
      if (rc == nullptr) {
        continue;
      }
      std::int64_t n = rc->value().to_z_number().to< std::int64_t >();
      switch (cmp->predicate()) {
        case ar::Comparison::SILT:
        case ar::Comparison::SILE:
        case ar::Comparison::UILT:
        case ar::Comparison::UILE:
          if (body == nullptr) {
            body = *succ;
            body_bound = n;
            ind_var = cmp->left();
          }
          break;
        case ar::Comparison::SIGE:
        case ar::Comparison::SIGT:
        case ar::Comparison::UIGE:
        case ar::Comparison::UIGT:
          if (exit_bb == nullptr) {
            exit_bb = *succ;
            exit_bound = n;
          }
          break;
        default:
          break;
      }
    }
  }
  // 需 body（i<N）与 exit（i>=N）成对且 N 一致，且 i 是自增归纳变量，才认定是
  // `for(i<N)` 循环——否则会把 `for(;;)` 体内的 `if(i<5){...}else{...}` 误当循环界
  // （43_NetBSD 等病态形态），去展开本不该展开的无限循环 → 状态爆炸。
  if (body == nullptr || exit_bb == nullptr || body_bound != exit_bound ||
      body_bound < 0 || body_bound > MAX_UNROLL ||
      !is_induction_var(ind_var, body)) {
    body = nullptr;
    exit_bb = nullptr;
    return -1;
  }
  return body_bound;
}

/// 路径敏感的 DFS：从 `bb` 出发，沿 CFG 后继枚举到 `target` 的所有路径。
/// `current` 是当前路径上的事件前缀；到达 `target` 时把路径记入 `out` 并停止
/// （target 之后的语句/后继与本次访问的竞争判定无关）。
/// `in_stack` 记录当前 DFS 栈上的基本块，用于检测回边（循环）并跳过——
/// 第一版不展开循环（DAG only），后续用 bound 展开。
void dfs_paths(ar::BasicBlock* bb, ar::Statement* target,
               std::vector< BmcEvent >& current,
               std::vector< std::vector< BmcEvent > >& out,
               std::vector< ar::BasicBlock* >& in_stack,
               const IndirectCalleeResolver& resolve_indirect,
               const std::set< ar::BasicBlock* >& headers,
               std::map< ar::BasicBlock*, int > iter,
               int depth = 0, bool skip_cmp = false) {
  bool is_header = headers.count(bb) != 0;
  bool on_stack = false;
  for (ar::BasicBlock* os : in_stack) {
    if (os == bb) {
      on_stack = true;
      break;
    }
  }

  bool reentered = false;  // 常量界 loop unroll 的回边重入（bb 已在栈上）
  if (on_stack && is_header) {
    // 常量界循环回边（仿 Dartagnan 的 LLVM -loop-unroll）：完整展开 N 次。
    ar::BasicBlock* body = nullptr;
    ar::BasicBlock* exit_bb = nullptr;
    std::int64_t N = constant_loop_bound(bb, body, exit_bb);
    if (N > 0) {
      while (in_stack.back() != bb) {
        in_stack.pop_back();  // 弹出 loop body 块，准备重入
      }
      if (iter[bb] < N) {
        reentered = true;  // 继续第 iter+1 次 body：掉到下面的后继循环
      } else {
        // N 次 body 已走完：只走 exit。
        if (exit_bb != nullptr) {
          std::vector< BmcEvent > succ_path = current;
          dfs_paths(exit_bb, target, succ_path, out, in_stack, resolve_indirect,
                    headers, iter, depth, true);
        }
        return;
      }
    } else {
      return;  // nondet/未知界：停止（老行为，保守 UNKNOWN）
    }
  }
  if (on_stack && !reentered) {
    return;  // 非 header 块在栈上（不可约 CFG 等复杂循环）：停（老行为，防死循环）
  }

  if (!on_stack) {
    in_stack.push_back(bb);
  }

  if (!on_stack) {
    for (auto sit = bb->begin(), send = bb->end(); sit != send; ++sit) {
      ar::Statement* s = *sit;
      // 死路径终止：ar::Unreachable，或 abort()/exit()（LibcAbort/LibcExit）——
      // 否则 abort 分支会「掉回」调用方误报竞争。
      if (core::isa< ar::Unreachable >(s)) {
        in_stack.pop_back();
        return;
      }
      if (auto* call0 = core::dyn_cast< ar::CallBase >(s)) {
        if (auto* cst =
                core::dyn_cast< ar::FunctionPointerConstant >(call0->called())) {
          if (cst->function() != nullptr) {
            const std::string& nm0 = cst->function()->name();
            if (nm0 == "libc.abort" || nm0 == "libc.exit") {
              in_stack.pop_back();
              return;
            }
          }
        }
      }
      // pthread_cond_wait(...) 处理
      if (auto* call = core::dyn_cast< ar::CallBase >(s)) {
        if (is_cond_wait(call)) {
          std::uint64_t m = cond_wait_mutex_id(call);
          current.push_back(BmcEvent{s, EvKind::Unlock, m});
          current.push_back(BmcEvent{s, EvKind::Lock, m});
        }
      }
      BmcEvent ev(nullptr, EvKind::Read);
      if (skip_cmp && core::isa< ar::Comparison >(s)) {
        // 常量界循环 body/exit 的循环条件（i<N / i>=N）：bookkeeping，不 emit
        // Assume——否则同一 phi 变量跨迭代重复 + 反向 exit 条件会矛盾 → UNSAT。
      } else if (classify_event(s, ev, &in_stack)) {
        current.push_back(ev);
      }
      // assume 语义（Dartagnan ResolveAborts → CondJump(cond, DEAD_END, continue)）：
      // assume_abort_if_not/__VERIFIER_assume 断言实参为真。把实参的正向比较也压成
      // Assume 事件，使「假分支」的正向约束与反向约束矛盾 → UNSAT（即假分支死）。
      if (auto* call2 = core::dyn_cast< ar::CallBase >(s)) {
        if (is_assume_call(call2) && call2->arg_begin() != call2->arg_end()) {
          if (ar::Comparison* cmp = forward_comparison_of(*call2->arg_begin())) {
            current.push_back(BmcEvent{cmp, EvKind::Assume});
          }
        }
      }
      if (s == target) {
        out.push_back(current);
        in_stack.pop_back();
        return;
      }
      // helper 内联：跟进有函数体、非原子、无循环的直接调用，把 helper 里的
      // 访问/锁事件收进路径（target 在 helper 里时在这里命中）。depth 上限防递归
      // 爆炸；跟不进的调用（extern/原子/循环）保守忽略 → 对应访问看不到 → UNKNOWN。
      // 间接调用（函数指针）走 resolve_indirect（FPA 候选集），每个候选各一条路径。
      if (depth < MAX_INLINE_DEPTH) {
        if (auto* call = core::dyn_cast< ar::CallBase >(s)) {
          std::vector< ar::Function* > callees;
          if (ar::Function* d = direct_callee(call)) {
            callees.push_back(d);
          } else if (resolve_indirect) {
            for (ar::Function* f : resolve_indirect(call)) {
              if (inline_ok(f)) {
                callees.push_back(f);
              }
            }
          }
          for (ar::Function* callee : callees) {
            // copy per callee（同 Stage 1 回溯坑：多候选不能用同一 current，否则
            // 上一候选的事件残留进下一候选 → 矛盾路径）。
            std::vector< BmcEvent > callee_path = current;
            dfs_paths(callee->body_or_null()->entry_block(), target, callee_path,
                      out, in_stack, resolve_indirect, headers, iter, depth + 1,
                      false);
          }
        }
      }
    }
  }

  // 后继 = 备选路径：每个后继用 `current` 的一份拷贝，避免死支的事件残留进
  // 下一条路径（否则真/假分支的相反 Comparison 会混进同一路径 → 矛盾 → UNSAT）。
  // 常量界循环 header：body 走 iter<N 次、exit 走 iter>=N 后（完整展开，无「0 次」假路径）。
  ar::BasicBlock* body = nullptr;
  ar::BasicBlock* exit_bb = nullptr;
  std::int64_t N = is_header ? constant_loop_bound(bb, body, exit_bb) : -1;
  for (auto succ = bb->successor_begin(), succ_end = bb->successor_end();
       succ != succ_end; ++succ) {
    std::vector< BmcEvent > succ_path = current;
    bool succ_skip = (N > 0) && (*succ == body || *succ == exit_bb);
    if (N > 0) {
      std::map< ar::BasicBlock*, int > succ_iter = iter;
      if (*succ == body) {
        if (iter[bb] < N) {
          succ_iter[bb] = iter[bb] + 1;
          dfs_paths(*succ, target, succ_path, out, in_stack, resolve_indirect,
                    headers, succ_iter, depth, succ_skip);
        }
      } else if (*succ == exit_bb) {
        if (iter[bb] >= N) {
          dfs_paths(*succ, target, succ_path, out, in_stack, resolve_indirect,
                    headers, iter, depth, succ_skip);
        }
      } else {
        dfs_paths(*succ, target, succ_path, out, in_stack, resolve_indirect,
                  headers, iter, depth, succ_skip);
      }
    } else {
      dfs_paths(*succ, target, succ_path, out, in_stack, resolve_indirect,
                headers, iter, depth, false);
    }
  }

  if (!on_stack) {
    in_stack.pop_back();
  }
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

/// 递归查一个函数体（含被调用的 helper）是否含 create/join（会建 HB、排序访问的同步）。
/// 只查 create/join、不查 mutex/cond_wait——helper 里的 mutex 常是「每条目锁」（如
/// cache_entry_addref 的 refs_mutex，funloop 文件），不排序目标访问，查它会误标
/// incomplete 丢 TP。depth 上限防 helper 相互调用的环。race-4_1 的 create/join 藏在
/// ath_ahb_probe 等 helper 里、被 while(1) 循环调用，直接查 block 查不到 → 必须递归。
inline bool function_has_sync_recursive(ar::Function* f, int depth = 0) {
  if (f == nullptr || depth > 8) {
    return false;
  }
  ar::Code* body = f->body_or_null();
  if (body == nullptr) {
    return false;  // extern（pthread_* 等无体）
  }
  for (ar::BasicBlock* bb : *body) {
    for (ar::Statement* s : *bb) {
      auto* call = core::dyn_cast< ar::CallBase >(s);
      if (call == nullptr) {
        continue;
      }
      auto* cst = core::dyn_cast< ar::FunctionPointerConstant >(call->called());
      if (cst == nullptr || cst->function() == nullptr) {
        continue;
      }
      const std::string& nm = cst->function()->name();
      if (nm.find("pthread_create") != std::string::npos ||
          nm.find("pthread_join") != std::string::npos) {
        return true;
      }
      if (function_has_sync_recursive(cst->function(), depth + 1)) {
        return true;  // helper 里有 create/join
      }
    }
  }
  return false;
}

/// 一个基本块是否含「能排序访问」的同步调用：mutex lock/unlock、create/join、
/// __VERIFIER_atomic_begin/end，及**被调 helper 里的同步**。pthread_mutex_init 等
/// 非排序调用不算——它们不影响竞态判定，只算「良性」循环体（如 05-lval_ls_01 的
/// init 循环）。
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
        // helper 里有同步（ath_ahb_probe → pthread_create 在 while(1) 循环里）
        if (function_has_sync_recursive(cst->function())) {
          return true;
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
inline UnrollResult unroll_to(
    ar::Function* func, ar::Statement* target,
    const IndirectCalleeResolver& resolve_indirect = {}) {
  UnrollResult result;
  ar::Code* body = func->body_or_null();
  if (body == nullptr || !body->has_entry_block()) {
    return result;
  }
  // 先收集循环头（供 dfs_paths 做常量界展开）。
  std::vector< ar::BasicBlock* > header_vec;
  std::vector< ar::BasicBlock* > in_stack2;
  collect_loop_headers(body->entry_block(), in_stack2, header_vec);
  std::set< ar::BasicBlock* > headers(header_vec.begin(), header_vec.end());

  std::vector< BmcEvent > current;
  std::vector< ar::BasicBlock* > in_stack;
  std::map< ar::BasicBlock*, int > iter;
  dfs_paths(body->entry_block(), target, current, result.paths, in_stack,
            resolve_indirect, headers, iter);

  // 路径数上限：常量界展开遇带分支循环体会 B^N 爆炸，超限保守标 incomplete。
  constexpr std::size_t MAX_PATHS = 256;
  if (result.paths.size() > MAX_PATHS) {
    result.paths.clear();
    result.incomplete = true;
    return result;
  }
  // 单路径事件数上限：嵌套/大循环体会让单条路径事件数暴涨，导致 check_race 的
  // SMT 编码巨大（内存/时间爆炸）。超限保守标 incomplete。
  constexpr std::size_t MAX_PATH_EVENTS = 1024;
  for (const auto& p : result.paths) {
    if (p.size() > MAX_PATH_EVENTS) {
      result.paths.clear();
      result.incomplete = true;
      return result;
    }
  }

  // 精确能力边界：target 的块若从某回边（循环）头前向可达，则要看循环会不会
  // 影响访问（incomplete → UNKNOWN）：
  //   - 常量界循环已在 dfs_paths 完整展开（N 次 body + exit），跳过；
  //   - 访问在循环体内（target 块能回到 header）：nondet 界保守 incomplete；
  //   - 访问在循环之后：循环体含同步（mutex/create/join）才可能排序访问 →
  //     incomplete；良性循环体（init/算术，无同步）不影响竞态 → 照常判。
  for (ar::BasicBlock* h : header_vec) {
    ar::BasicBlock* body_tmp = nullptr;
    ar::BasicBlock* exit_tmp = nullptr;
    if (constant_loop_bound(h, body_tmp, exit_tmp) > 0) {
      continue;  // 已完整展开
    }
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
