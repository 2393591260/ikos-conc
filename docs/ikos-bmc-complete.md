# IKOS BMC 完整设计文档（从零到 sound gate 体系）

> **阅读前提**：本文把「事件模型 → 展开算法 → SMT 编码 → sound 证明 → gate 体系」讲完整，供学习与后续开发。
> **严格标注**：每节开头标【铁证】= 已读源码/实跑验证；【待验证】= 我的设计提案，未落地，正确性待 A/B 证实。
> 数据基准：2026-10-07，steps 1-4 之后（TP=80 / FP=0 / TN=709 / FN=0 / err=1）。

---

## 0. 目标与红线（【铁证】，来自项目约定）

- **目标**：接在抽象解释（over-approx，保 FN=0）之后，对每个 demote 成 UNKNOWN 的候选竞争对，用 Z3 精确判定
  「这两个访问是否真的能并发」。**SAT ⟹ 报 FALSE（找回 TP）；UNSAT / 无法证明 ⟹ 保持 UNKNOWN。永不报 TRUE**（证 SAFE 交给抽象解释）。
- **红线 FN=0**：sound 优先于 precision，宁多报 FP 不漏报 FN（但当前也守 FP=0）。
- **有界红线（【待验证】后续开发约束）**：只做逐对、带上限的编码，不碰全程序 SMT；任何一片让某文件超几十秒即打回。

---

## 1. 理论依据：Dartagnan SV-COMP 2026 的 race 判定（【铁证】，读源码）

Dartagnan 官方版 = `7caef5c`/标签 4.3.1（SV-COMP 2026），其 no-data-race 判定**不是** cat 文件里的
`dr/conflict`（那是 2026-06 才加的 SV-COMP 2027 版），而是**内置 `PropertyEncoder.encodeDataRaces`**。

### 1.1 svcomp.cat（6 行，【铁证】）

```
let com = (rf | fr | co)
let hb = po | (com & int) | (rmw^-1? ; (com & ext))
acyclic hb as sc
```

- `rf` = read-from（读从某写取值）；`fr` = from-read；`co` = coherence（同址写全序）。
- `com & int` = 线程内通信；`com & ext` = 跨线程通信；`rmw^-1?` = 可选 RMW 反边前缀。
- `acyclic hb as sc` = hb 无环 = 顺序一致性。

### 1.2 race 充要条件（【铁证】，PropertyEncoder.encodeDataRaces）

对每对线程 t1≠t2、t1 里每个非 init 非原子 WRITE `w`、t2 里每个非 init 非原子 MEMORY `m`：

1. `canRace` = `mo==null || mo==NONATOMIC`（**原子/RMW 访问根本不进竞态对**）。
2. 跳过 `w.RMW && m.RMW`、`!alias.mayAlias(m,w)`（不同址）。
3. `isConflictingPair = execution(m,w) && sameAddress(m,w)`。
4. `isAdjacentInHb = hb(m,w) && clock_hb(w)==clock_hb(m)+1`（**hb 里相邻**）。
5. `hasRace |= conflicting && adjacent`。

**核心语义：相邻 ⟺ 无 hb 排序 ⟺ 竞态。** IKOS `check_race` 的「相邻判定」对齐的正是这条（不是 2027 版 `sw` 的「两端必须原子」）。

### 1.3 Dartagnan 为何慢（【铁证】+ 复杂度分析）

全程序 SMT：所有线程所有事件编码成一个公式，`encodeDataRaces` 遍历 `t1×t2×e1×e2`。SAT 是 NP-hard，
锁无关队列/多线程/无界循环 → 公式指数爆炸 → 超时（官方 ERR=349）。**完备性（能证 SAFE + 能证 RACE）就是慢的根源。**

---

## 2. 架构：两层判定（【铁证】，来自项目约定 + 代码）

```
抽象解释（over-approx，快）           BMC（under-approx，只对候选对，有界）
  证明 SAFE（TN=+2）                   确认 RACE（TP=+1）
  报「潜在竞争」→ demote 成 UNKNOWN     对每个候选对精确判定 SAT/UNSAT
        └────────────── 候选对 ──────────────┘
```

- **sound 保证**：抽象解释的「潜在竞争」不直接报 FALSE，先 demote；只有 BMC 确认（SAT）才报 FALSE。
  所以 FP=0（BMC 是欠近似，确认的都是真竞态）。
- **完备性缺失的后果**：硬程序两头都「可能」→ UNKNOWN（0 分），**不是 FP/FN**。soundness 与完备性正交。

---

## 3. BMC 全流程（【铁证】，读 bmc.hpp / data_race.cpp）

`confirm_race(funcA, stmtA, funcB, stmtB, resolve_indirect)` 依次：

1. `has_unmodeled_sync(funcA/funcB)` → 有未建模同步则 false。
2. `unroll_to(funcA, stmtA)`、`unroll_to(funcB, stmtB)` → 两条路径展开。
3. `rA.incomplete || rB.incomplete` → 循环没展开干净则 false。
4. held 锁门 → 双方都持同一把 held 锁则 skip。
5. 每条路径对 `check_race(pathA, pathB)` → SAT 则 true。

上游（data_race.cpp）先过 `bmc_program_sound`（全程序 `has_unmodeled_sync`）、`same_global`（别名门）、
`transitive_ordered`（隔代门），再选 anchor 对（当前 `pair_indices.front()`）。

---

## 4. 事件模型（【铁证】，读 unroll.hpp）

```cpp
enum class EvKind { Read, Write, Lock, Unlock, Assume, Create, Join };
struct BmcEvent {
  ar::Statement* stmt;      // AR 语句
  EvKind kind;
  uint64_t mutex;           // Lock/Unlock 锁身份（arg0 语法同一性）
  uint64_t base; int64_t const_off; uint64_t var_id; int64_t coeff;  // 符号地址
  uint64_t thread;          // Create=线程函数指针 / Join=句柄
};
```

`classify_event`：Load→Read、Store→Write、Comparison→Assume、mutex→Lock/Unlock（`__VERIFIER_atomic_begin/end`
→ PSEUDO_ATOMIC_LOCK）、pthread_create→Create、pthread_join→Join、cond_wait→Unlock+Lock。
地址提取失败 / 锁身份提取失败 → 保持 base==0 → 上游保守不确认。

---

## 5. 展开算法（【铁证】，读 unroll.hpp）

- `dfs_paths`：路径敏感 DFS，到 target 停止；多后继各拷贝一份 `current`（防死支事件残留）。
- 死路径终止：`ar::Unreachable`、`libc.abort/exit`（子串匹配，含 `ar.` 前缀）。
- **常量界循环展开**：`constant_loop_bound`（body `i<N` + exit `i>=N` 成对 + `is_induction_var` 只认局部 SSA
  自增 `v=v±const`）→ 完整展开 N 次（`MAX_UNROLL=8`）。nondet/未知界 → 停（老行为 UNKNOWN）。
- **helper 内联**：`inline_ok`（有体 + 非 `bmc_skip_inline` + 无循环 + 无 begin/end 段）→ 递归展开 callee，
  `MAX_INLINE_DEPTH=8`。间接调用走 FPA 候选集。
- **资源上限**：`MAX_PATHS=256`、`MAX_PATH_EVENTS=1024`，超限标 incomplete。
- **incomplete 判定**：target 从 nondet 循环头可达，且（target 能回到头 或 `loop_has_sync`）→ incomplete。
  `loop_has_sync` → `function_has_sync_recursive` 递归查 helper 里 mutex/cond_wait/create/join。

---

## 6. SMT 编码（【铁证】，读 encode.hpp）

`check_race(pathA, pathB)`：

1. **clock 变量**：每个事件一个整数 clock。
2. **po**：路径内相邻事件 `clock[i] < clock[i+1]`。
3. **spawn HB**：`Create(B) < B 首事件`。
4. **join HB**：`joined 线程末事件 < Join`（过约束，只丢 TP 不引 FP）。
5. **mutex 互斥**：共享锁 `Unlock_A → Lock_B` 或 `Unlock_B → Lock_A`（二选一布尔）。
6. **assume 约束**：路径上每条 Comparison 编码（整数比较；指针/浮点比较 → `unencodable` → 不确认）。
7. **同址门**：两访问 `base` 相同 + `const_off + coeff*var` 相等。
8. **竞争查询**：两访问能否相邻（`clock 差 1`，两方向各查一次）→ SAT = 竞态。

---

## 7. sound gate 全清单（【铁证】，steps 1-4 之后）

每个 gate 都是**能力边界**（「BMC 建不了 X → UNKNOWN」），不是文件补丁：

| # | gate | 能力边界 |
|---|---|---|
| 1 | `has_unmodeled_sync` | C11 原子 / `__sync_*` / `__atomic_*` / `__VERIFIER_atomic_*`(除 begin/end) / sem / barrier / spin |
| 2 | `incomplete` | nondet 循环未展开 + helper 递归含 mutex/cond_wait/create/join |
| 3 | `transitive_ordered` | 隔代 create/join（2 线程模型看不到中间线程的 join）|
| 4 | `same_global` + offset | points-to 非 singleton / 偏移 ⊤ |
| 5 | held 锁门 | 双方持同一 held 锁（unlock 在访问后、不在路径里，互斥表达不了）|
| 6 | 资源上限 | MAX_UNROLL=8 / MAX_PATHS=256 / MAX_PATH_EVENTS=1024 |
| 7 | 地址/锁身份提取失败 | 保守不确认 |

**去启发式史（steps 1-4）**：acquire/release 归入 unmodeled、`function_has_sync_recursive` 补查 mutex/cond_wait、
`is_induction_var` 只留局部 SSA、held 锁门注释改写成边界。TP 96→80，FP=0/FN=0 全程保持。

---

## 8. 与 Dartagnan 的对照（【铁证】机制映射 + 【待验证】缺口实现）

| Dartagnan 原理 | IKOS 现状 | 缺口 |
|---|---|---|
| `hb=po\|com\|rmw`，`com=rf\|fr\|co` | po+mutex+spawn/join | ❌ com（rf/co 值流）|
| 全程序 SMT、N 线程 | 2 路径、anchor 或全对 | ❌ N 线程 |
| Andersen + `sameAddress` | singleton pts + offset + SMT 同址门 | 部分（堆对象未放开）|
| `canRace` 非原子 | `has_unmodeled_sync` 整体挡 | 等价但更粗 |
| loop-unroll + bound | constant_loop_bound(≤8) + incomplete | 等价 |
| witness GraphML | ikos2graphml | 等价 |

---

## 9. 待验证的设计提案（【待验证】，未落地，正确性待 A/B）

> 以下是「有界还原原理」的具体实现方向，**每个都是我的设计判断，不是已验证结论**。落地前必须
> 逐片 A/B：FN=0/FP=0 保持 + 有界（不超时）。

1. **rf/co 值流（有界版）**：只对**两条展开路径上**出现在目标地址的写排 `co` 全序（整数时钟）、给读选 `rf`
   （读值=写值 SMT 相等），再查相邻。驱动样例 `40_barrier_vf-b`(racy) vs `43_NetBSD`(safe) 同一套标志。
   **证据**：Dartagnan 的 `com` 机制（铁证）。**风险**：实现细节（co 全序的 SMT 规模）未验证。
2. **AI 可行性剪枝**：`dfs_paths` 展开到 `if(c)` 时，查抽象解释该点不变式，`c` 确定假 → 剪真分支。
   **sound 依据**：过近似说「确定假」则真不可达（标准技术）。**风险**：抽象域精度、与 BMC 的接口未做。
3. **解耦 anchor**：遍历组内所有对。已探针验证 +airline/floating_read ~12 TP，但 `workstealqueue_mutex-2`
   崩（dfs_paths null-deref，待 ASAN 定位）。
4. **N 线程**：2→K 路径展开（K 有上限）。
5. **堆对象精确别名**：DynAlloc 精确化（当前 per-thread malloc 多实例坍缩，故排除堆）。

**最终边界（【待验证】估算）**：加 rf/co + 别名 + N 线程，TP 可从 80 追到 ~140，仍保持有界快；
但最后 ~35 个需要 Dartagnan 全程序 SMT 完备性的 TP，IKOS 的欠近似架构**追不到**（数学上快≠完备）。
**这不影响反超**：IKOS 靠抽象解释的 TN 优势（709 vs 505）分数已 1498 > Dartagnan 1186。

---

*铁证来源：Dartagnan `7caef5c` 源码（svcomp.cat / PropertyEncoder.encodeDataRaces / VisitorLlvm）、
IKOS `unroll.hpp` / `encode.hpp` / `bmc.hpp` / `data_race.cpp`、实跑 Dartagnan 4.3.1 与 IKOS A/B。*
