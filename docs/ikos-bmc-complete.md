# IKOS BMC 完整设计文档（从零到 sound gate 体系）

> **阅读前提**：本文把「事件模型 → 展开算法 → SMT 编码 → sound 证明 → gate 体系」讲完整，供学习与后续开发。
> **严格标注**：每节开头标【铁证】= 已读源码/实跑验证；【待验证】= 我的设计提案，未落地，正确性待 A/B 证实。
> 数据基准：2026-10-08，修复 1 FP + 2 FN + A1 局部值编码之后（TP=52 / FP=0 / TN=709 / FN=0 / err=1）。
> ⚠️ 此前「TP=80」含 30 个靠自由变量**不 sound** 确认的竞争，A0 gate 已退回 UNKNOWN（见 §11）。
> 测试集 = SV-COMP 2026 no-data-race 全量（`svcomp_race_test.py`，默认 /home/ruan/sv-benchmarks-2026，~1029 任务）。

---

## 0. 目标与红线（【铁证】，来自项目约定）

- **目标**：接在抽象解释（over-approx，保 FN=0）之后，对每个 demote 成 UNKNOWN 的候选竞争对，用 Z3 精确判定
  「这两个访问是否真的能并发」。**SAT ⟹ 报 FALSE（找回 TP）；UNSAT / 无法证明 ⟹ 保持 UNKNOWN。永不报 TRUE**（证 SAFE 交给抽象解释）。
- **红线 FN=0**：sound 优先于 precision，宁多报 FP 不漏报 FN（当前也守 FP=0，但见 §11：FP=0 是实测结果，
  不是「欠近似」自动保证——BMC 数据上过近似，靠 A0 gate 主动封自由变量才守住）。
- **有界红线（【待验证】后续开发约束）**：只做逐对、带上限的编码，不碰全程序 SMT；任何一片让某文件超几十秒即打回。
- **内存模型**：目标 = SV-COMP no-data-race 的 pthread 程序，Dartagnan 官方以 `acyclic hb as sc`（SC）建模（§1.1），
  IKOS 编码同为 SC。弱内存（RC11/TSO/acquire-release 原子）不在本设计范围——统一走 `has_unmodeled_sync` 门保守 UNKNOWN，不引 FP。

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

**核心语义：race ⟺ conflicting ∧ hb-相邻**（`isAdjacentInHb = hb(m,w) ∧ clock_hb(w)==clock_hb(m)+1`）。
「相邻」指在 hb 的某个线性扩展里两事件连续（中间无其他事件），**不是**「两事件之间没有 hb」——可比且无
中间元素的两事件同样相邻。IKOS `check_race` 对齐的正是这个「相邻」判定（不是 2027 版 `sw` 的「两端必须原子」）。

> 注：这里的 hb 是 Dartagnan CAT 里的 `po | com | rmw`（含 rf/fr/co 值流），不能直接等同于 POSIX/C11 的
> happens-before；assume/值约束是执行约束、不是事件二元排序关系。IKOS 的编码只覆盖这条 hb 的一个子集
> （po+mutex+spawn/join，无 com），见 §8 缺口。

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

- **SAFE 方向 sound**：抽象解释是 over-approx，SAFE 结论可靠（FN=0）；「潜在竞争」不直接报 FALSE，先 demote。
- **FALSE 方向需主动守 FP=0**：BMC 结构上欠近似（有界路径）、**数据上过近似**（未建模值建成自由 SMT 变量，
  会凭空造出程序没有的执行）——所以「欠近似 ⇒ FP=0」**不成立**。FP=0 靠 A0 gate 主动封自由变量守住，是
  实测结果而非架构自动保证（见 §11）。
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
6. **assume 约束**：路径上每条 Comparison 编码。**A0/A1 值来源 gate**：常量 + `__VERIFIER_nondet_*` 结果
   （自由符号 + 类型范围）可编码；变量 operand 若追溯到 load / 实参 / 未知调用 / phi → `unencodable` → 不确认；
   指针 / 浮点 → 不确认。每路径独立 cache（同入口函数两实例的局部值互不相关）。
7. **同址门**：两访问 `base` 相同（否则提前返回不确认）+ **字节范围重叠**（`off_a < off_b+width_b ∧ off_b < off_a+width_a`）；
   变量下标 `var_id!=0` → 不确认。
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
| 8 | A0/A1 值来源 gate | 变量 operand 追溯到 load/实参/未知调用/phi → 不确认；变量下标 `var_id` / base 提取失败 → 不确认（自由变量造执行，见 §11）|

**去启发式史（steps 1-4）**：acquire/release 归入 unmodeled、`function_has_sync_recursive` 补查 mutex/cond_wait、
`is_induction_var` 只留局部 SSA、held 锁门注释改写成边界。TP 96→80，FP=0/FN=0 全程保持。

**待办（gate 埋点量化）**：各 gate 的拦截率与对应潜在 TP 数尚未量化（尤其 `has_unmodeled_sync` vs `same_global`
谁挡得多）。优化前先给 7 个 gate 加 env-gated 埋点，按「拦 TP 数 / 实现成本」排序，优先动低成本的。

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

**最终边界（【待验证】估算）**：加 rf/co + 别名 + N 线程，TP 可从 52（sound 基线）追到 ~140，仍保持有界快；
但最后 ~35 个需要 Dartagnan 全程序 SMT 完备性的 TP，IKOS 的欠近似架构**追不到**（数学上快≠完备）。
**这不影响反超**：IKOS 靠抽象解释的 TN 优势（709 vs 505），sound 基线（TP=50）分数 1468 仍 > Dartagnan 1186。

---

## 10. 设计决策史（从 ikos-bmc-design.md 并入，避免文档冗余）

### 10.1 核心设计决策：不重写 checker，复用它的产物（【铁证】）

BMC 不是从零扫 AR，而是接管抽象解释判为「潜在竞争」的那一对访问：
- **用 checker 的**：访问事件 / 线程归属 / 竞争对语句（定位 load/store + 线程入口）。
- **不用** `resolve_points_to`（过近似，per-thread malloc 多对象坍缩成同 cell）——BMC 要精确地址相等。
- **不用** 抽象锁集（不精确）——BMC 在 SMT 里重算同步。

### 10.2 helper 内联暴露的 4 类 FP（gate 的历史根因）（【铁证】）

| FP 机制 | 代表文件 | 对应 gate |
|---|---|---|
| 单调用原子函数内联（裸访问） | pthread-atomic | `bmc_skip_inline` |
| helper 带循环（DAG 展开不精确） | — | `function_has_loop` |
| 无锁协议（线性化非 hb） | elimination_backoff_stack | `function_has_atomic_section` |
| 隔代 create/join（2 线程看不到中间线程） | race-4_1 | 隔代祖先门（`_thread_ancestors`−`_thread_creators`）|

### 10.3 独特优势（比 Dartagnan 强的地方）（【铁证】）

| 难点 | Dartagnan 现状 | IKOS 优势 |
|---|---|---|
| double 浮点 | 直接 CRASH | APRON（多面体/octagon）天然支持 double |
| join 句柄是数组 + 变量下标 | 全 UNKNOWN | 抽象解释天然适合上数组摘要域 |

---

## 11. 已复现并修复的 soundness 缺陷（2026-10-08，Codex 审查）

Codex 审查（`docs/ikos-bmc-review-2026-10-08.md`）复现了 1 FP + 2 FN，均已修复并提交：

| 用例 | 问题 | 根因 | 修复 commit |
|---|---|---|---|
| `exclusive-flag.c` | **FP**：无竞争程序报 FALSE | 自由变量：两线程读同一 shared var 各取不同值，造出程序没有的执行 | `4910339`（A0 gate）|
| `published-before-init.c` | **FN**：真竞争报 SAFE | `private_init` 未证对象未发布即删候选 | `df63329`（禁用）|
| `overlapping-widths.c` | **FN**：真竞争报 SAFE | 偏移过滤无访问宽度 | `df63329`（width+范围重叠）|

**核心教训**：「BMC 是欠近似 ⇒ FP=0」证伪。BMC 结构上欠近似（有界路径）、数据上过近似（自由变量），
两种近似方向同时存在，不能靠模块名称判断。FP=0 是 A0 gate 主动封自由变量后的**实测结果**，不是架构保证。

**代价**：TP 80→50（掉 30 = 原靠自由变量不 sound 确认的值守卫/变量下标/标志式竞争）。恢复需 A1 局部值编码
（rf/co + 算术 + 实参/返回值绑定），不能再用自由变量。复现脚本 `docs/review-repros/bmc-2026-10-08/run.py`。

---

*铁证来源：Dartagnan `7caef5c` 源码（svcomp.cat / PropertyEncoder.encodeDataRaces / VisitorLlvm）、
IKOS `unroll.hpp` / `encode.hpp` / `bmc.hpp` / `data_race.cpp`、实跑 Dartagnan 4.3.1 与 IKOS A/B。*
