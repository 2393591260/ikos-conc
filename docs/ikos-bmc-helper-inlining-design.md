# BMC helper 内联 + 值域可行性 —— 第二期设计文档

> 目标：找回「CPAchecker 能解、IKOS 报 UNKNOWN」的那批 TP（重点是 helper 内联类）。
> 状态：**一期（helper 内联）已落地**（commit fa3c4aa，TP 64→85 / FP=0 / FN=0）；
> 二期（值域可行性）与「精确 create/join HB」仍未做，见 §10。
> 红线：**FN=0 / FP=0**（sound 优先于 precision）。

---

## 1. 背景与现状（为什么做这个）

全量实测：IKOS（demote + BMC）在**今年滚动集（1030 任务）**上 TP=64 / FP=0 / TN=707 / FN=0，还有 **258 个 UNKNOWN**。

把这 258 个 UNKNOWN 拿去给 CPAchecker（`svcomp27--datarace`）跑，**28 个 CPAchecker 能解**（20 个报 FALSE=真竞争、8 个报 TRUE=真安全）。

### blocker 分桶（逐类实测）

| blocker | 文件数 | 代表 | 根因 |
|---|---|---|---|
| **helper 内联** | ~11 | pthread-atomic 10（`__VERIFIER_atomic_dec_a` 等）+ 40_barrier（`Barrier2`） | 竞争访问在**辅助函数**里，不在线程入口 |
| 循环 | ~7 | pthread-lit 4 + join counter 3 | busy-wait/协议循环 → 展开器标 incomplete |
| thread-id 别名 | ~4 | pthread-lit | 同函数多线程实例混淆 |
| 其它 | ~10 | pthread-ext 剩余 + goblint | 分散 |

**本期只攻「helper 内联」这一类（~11 个 TP）。**

### helper 内联的根因（代码实锤）

`data_race.cpp` 析构里的 BMC 门：

```cpp
if (fa->name() == a0.thread_id && fb->name() == b0.thread_id) {
  bmc_confirmed = bmc::confirm_race(fa, a0.stmt, fb, b0.stmt);
}
```

它要求**访问语句直接在线程入口函数里**。而展开器 `unroll_to(func, target)` 只从 `func` 入口沿 CFG 往后走，**遇到函数调用（`Barrier2()`）不跟进**。于是访问在 helper 里时 `fa->name()`（=`"Barrier2"`）≠ `thread_id`（=`"thr1"`）→ 门失败 → UNKNOWN。

例（40_barrier）：
```c
void Barrier2() { count++; if(count==3) COND=1; ... }   // 访问在这
void* thr1(void* arg) { Barrier2(); return 0; }          // 线程入口只调 helper
```

---

## 2. 技术路线总览

两步，分两期落地：

- **一期：helper 内联（结构）** —— 让展开器跟进函数调用，把 helper 里的访问 + 锁事件收进路径；放宽 `confirm_race` 的门。
- **二期：值域可行性（FP 控制）** —— 给展开器接上 `int_to_interval`，在分支处跳过不可达路径，消掉条件写 FP。

两期都守住 FP=0/FN=0，任一破就回退。

---

## 3. 一期：helper 内联（结构层）

### 3.1 现成可复用的机制（已确认）

- **解析被调函数**：`call->called()` → `dyn_cast<ar::FunctionPointerConstant>` → `function()` —— BMC 的 `classify_mutex_call` 已经在用。
- **拿被调函数 CFG**：`func->body_or_null()`（`ar::Code*`，含 entry_block + 基本块）。
- **递归 DFS**：`dfs_paths(bb, target, current, out, in_stack)` 本身就是递归 CFG 遍历，扩展成「跟进调用」很自然。

### 3.2 改动点

| 文件 | 函数 | 改动 |
|---|---|---|
| `analyzer/include/ikos/analyzer/analysis/bmc/unroll.hpp` | `dfs_paths` | 遇到 `CallBase` 且 callee 有函数体时，**递归展开 callee 的 CFG**（收集 Load/Store/Lock/Unlock 事件 + 找 target）；加 `depth` 参数，上限 4~8 |
| 同上 | `unroll_to` | 传初始 depth=0；`incomplete` 判定保持不变（callee 里的回边同样触发 incomplete） |
| `analyzer/include/ikos/analyzer/analysis/bmc/bmc.hpp` | `confirm_race` | 门从「访问必须在 thread 入口」放宽为「访问在 thread 入口、或在其（有限深度的）helper 调用链里」 |

### 3.3 内联逻辑（伪代码）

```cpp
void dfs_paths(bb, target, current, out, in_stack, depth) {
  if (depth > MAX_DEPTH) return;            // 防递归爆炸
  // ... 现有回边检测 in_stack ...

  for (stmt in bb) {
    if (is_cond_wait(stmt)) { /* unlock+lock */ }
    if (classify_event(stmt, ev)) current.push_back(ev);
    if (stmt == target) { out.push_back(current); return; }

    // 新增：跟进有函数体的直接调用
    if (auto* call = dyn_cast<CallBase>(stmt)) {
      if (auto* fpc = dyn_cast<FunctionPointerConstant>(call->called())) {
        if (ar::Function* callee = fpc->function()) {
          if (ar::Code* cbody = callee->body_or_null()) {
            dfs_paths(cbody->entry_block(), target, current, out, in_stack, depth+1);
          }
        }
      }
    }
  }
  // ... 现有后继遍历 ...
}
```

> 注意：`classify_event` 现在对非锁调用返回 false（忽略）。内联后，非锁但有函数体的调用会被「跟进」，而无函数体的（extern / 原子单调用）仍忽略。

### 3.4 一期风险与回退

- **风险**：helper 里的**条件写**（`if(count==3) COND=1`）——展开器路径敏感，两条分支都走，可能把「数据依赖不可行」的写当成可达 → FP。
- **但实测分析**：这批 11 个是 TP（真竞争），条件写是**真实可行**的（`count==3`、`a>b` 都真能成立），不是 munge 那种假条件。所以对这批 TP 大概率不引入 FP。
- **真 FP 风险在 SAFE 文件**（有 helper 且 helper 里有不可达条件写的安全程序）——这靠二期的值域可行性防，一期先用 A/B 全量兜底：**FP>0 立即回退**。

---

## 4. 二期：值域可行性（FP 控制）

### 4.1 现成机制

`int_to_interval(variable)`（数值域的取值区间）本会话已验证可用。全局变量的「值」在 cell 里：`mem_factory->get_global(gv)` 拿地址、再查 cell 变量，不是直接查全局指针变量。

### 4.2 改动点

| 文件 | 改动 |
|---|---|
| `unroll.hpp` | `dfs_paths` 增加一个「分支可行性判定」回调：遇到 `ar::Comparison`（分支条件）时，查询条件变量的区间，**区间排除条件值 → 跳过该分支**（不展开） |
| `bmc.hpp` / `data_race.cpp` | 把 checker 已算好的 fixpoint 摘要（`inv`）传进 `confirm_race` / `unroll_to`，供分支判定用 |

### 4.3 逻辑

```
分支 if(cond) ...：
  itv = int_to_interval(cond 里的变量)
  若 itv 排除 cond 成立的值 → 这条分支不可达 → 不展开
  否则 → 保守展开（两边都走）
```

> 关键：`int_to_interval` 是 flow-sensitive 的**过近似**，用它判「不可达」是 sound 的（区间排除 ⟹ 真不可达）；区间包含时保守保留，只丢 TP 不引 FP。

---

## 5. soundness 论证

BMC 只报 FALSE（确认竞争），靠「HB 编码完整 + 别名精确」。

- **内联**是结构层：跟进调用 = 把 callee 的访问/锁事件纳入路径，不改变 HB/别名的判定口径。若 callee 里的锁/访问被正确收集，`check_race` 的判定仍 sound。
- **条件写 FP** 的根源是「把不可达分支当可达」——二期用 `int_to_interval` 跳过不可达分支后，展开出的每条路径都是（值域意义上的）可能路径 → 消 FP。
- **深度上限**只影响「能跟进几层」，跟不进就保守（该 helper 的访问看不到 → 不确认 → UNKNOWN），**不引 FN/FP**。

---

## 6. 门与回退条件

**全部门**：

1. 一期上线后 A/B 全量（滚动 + 冻结）断言 **FP=0 且 FN=0**；
2. 定向回归：这 11 个 helper 文件变 FALSE；已确认的 64 个 TP 一个不少；`04-mutex_01-simple_rc` 仍确认。

**回退条件（任一触发即回退）**：

1. A/B 全量 FP > 0（任何新误报）；
2. A/B 全量 FN > 0（任何新漏报）；
3. 定向回归失败（11 个没变 FALSE，或 64 个丢了一个）。

---

## 7. 验证计划

1. **单元**：先拿 40_barrier 单文件，插探针确认展开器正确跟进 `Barrier2`、收集到 `count++` 等访问 + 锁事件。
2. **定向**：11 个 helper 文件 + 64 个已确认 TP 的回归。
3. **全量**：`svcomp_race_test.py`（滚动 + 冻结）断言 FP=0/FN=0。
4. **见证**：确认 FALSE 的文件，`svcomp_witness.py` 生成见证，CPAchecker 复核（沿用本会话验证出的正确调用）。

---

## 8. 分期与里程碑

| 期 | 内容 | 验收 |
|---|---|---|
| 一期 | helper 内联 + 放宽门 | 11 个 helper 文件变 FALSE；A/B FP=0/FN=0 |
| 二期 | 值域可行性（跳过不可达分支） | 若一期暴露条件写 FP，二期消掉；A/B FP=0/FN=0 |

---

## 9. 关键文件速查

| 文件 | 职责 |
|---|---|
| `analyzer/include/ikos/analyzer/analysis/bmc/unroll.hpp` | `dfs_paths` 跟进调用、分支可行性回调、`unroll_to` |
| `analyzer/include/ikos/analyzer/analysis/bmc/bmc.hpp` | `confirm_race` 门放宽 |
| `analyzer/src/checker/data_race.cpp` | BMC gate（析构）、传 fixpoint 摘要 |
| `core/include/ikos/core/domain/machine_int/abstract_domain.hpp` | `int_to_interval`（值域可行性） |

---

## 10. 一期落地结果（实测，commit fa3c4aa）

**A/B 全量**（`IKOS_BMC_RECOVER=1 svcomp_race_test.py --demote`）：

| 集 | TP | FP | TN | FN | 得分 |
|---|---|---|---|---|---|
| 基线（demote+BMC，无内联） | 64 | 0 | 706 | 0 | 1476 |
| **一期（内联+门）** | **85** | **0** | **706** | **0** | **1497** |
| 滚动集（1030） | 85 | 0 | 707 | 0 | 1499 |

> 注意：不是设计文档里预期的 90——两条保守门各吃掉几个 TP（见下）。90 是
> 「内联+原子/循环两门」的裸数，但那个状态下 FP=2。

### 内联暴露的 4 类 FP 与对应 gate（实测逐文件核实）

| FP 机制 | 代表文件 | gate |
|---|---|---|
| 单调用原子函数内联（`__VERIFIER_atomic_dec_a` 等裸访问） | pthread-atomic | `bmc_skip_inline`（跳过 `__VERIFIER_atomic_*`/`__atomic_*`/`__sync_*`） |
| helper 带循环（DAG-only 展开器不精确） | — | `function_has_loop`（跳过含回边的 helper） |
| **无锁协议**（原子区外快照读 × 原子区内写，线性化签名） | elimination_backoff_stack | `function_has_atomic_section`（跳过含 `__VERIFIER_atomic_begin/end` 的 helper） |
| **隔代 create/join**（A→C→B 跨线程链，两线程 po+mutex 看不到中间线程 C 的 join） | race-4_1-thread_local_vars | 隔代祖先门（`_thread_ancestors` − `_thread_creators`） |

### 尚未做（后续）

1. **精确 create/join HB（区间时钟）**：现在的「隔代祖先门」是保守近似，会误挡
   「隔代真竞争」（main 与孙子线程在 create 与 join 之间的竞争）。真正的
   Stage 2 是在 `encode.hpp` 给每个线程加区间时钟 `S_T < E_T` + spawn/join 边，
   才能既不漏报也不误报。直接父子竞争不受当前门影响，隔代真竞争被挡（cost ~1 TP）。
2. **无锁协议建模**：elimination_backoff_stack 的 race-free 是线性化定理，不是
   happens-before，锁集/HB（goblint 同款伪锁）证明不了。当前用保守 gate 挡成
   UNKNOWN，不建模。参考 Dartagnan `rmw = amo | lxsx` 只认真原子 RMW。
3. **值域可行性（原二期）**：`int_to_interval` 跳不可达分支防「条件写」FP。当前
   一期没暴露条件写 FP（A/B FP=0），暂不需要；若后续更激进的内联暴露它，再做。
