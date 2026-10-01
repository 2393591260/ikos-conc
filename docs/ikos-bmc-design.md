# IKOS BMC 模块设计 —— AR → SMT 映射

> 目标：接在 `--demote-race-to-unknown` 之后，对每个 demote 成 UNKNOWN 的抽象竞争对，
> 用 Z3 精确判定「这两个访问是否真的能并发（真竞争）」。SAT ⟹ 报 FALSE（找回 TP）；
> UNSAT ⟹ 保持 UNKNOWN（有界内无竞争）。永远不报 TRUE（证明 SAFE 交给抽象解释）。

## 0. 复用 checker 已有的提取（不要重写）

`DataRaceChecker` 已经做了大部分事件提取，BMC 模块**先复用它的产物**：

| 需要的东西 | 现有代码 | 说明 |
|---|---|---|
| 访问事件（load/store） | `check_load` / `check_store`（data_race.cpp:2072/2129） | 已解析 `load->operand()`/`store->pointer()` → points-to、offset、thread、locks |
| 线程归属 | `current_thread_id`（data_race.cpp:1480） | 沿 call-context 链上溯到线程入口 |
| 锁集 | `snapshot_locks` / lockset 域 | 抽象锁集（**BMC 不用它，见 §4**） |
| 竞争对 | `_accesses`（AccessRecord 列表）+ 析构里的 pairwise 判定 | demote 后的 UNKNOWN 对就在这里 |

**关键结论**：BMC 模块**不是**从零扫描 AR，而是**接管 checker 析构里被判为「潜在竞争」的那一对访问**（`_accesses` 里的两个 AccessRecord），只对它们做精确判定。查询面窄到 O(1) 对，不用全程序展开所有对。

## 1. 事件模型

BMC 需要两类事件，都从 AR 静态可得：

```
内存事件：
  ar::Load   →  Read 事件，地址 = load->operand()
  ar::Store  →  Write 事件，地址 = store->pointer()

同步事件（建立 hb）：
  ar::CallBase 且 callee 名含 "pthread_mutex"/"pthread.mutex"：
     含 "lock"   → Lock(acquire)  事件，锁对象 = arg0
     含 "unlock" → Unlock(release) 事件，锁对象 = arg0
  pthread_create → Spawn 事件（create-HB）
  pthread_join   → Join 事件（join-HB）
```

每个事件带：全局 id、所属线程（`current_thread_id`）、地址/锁对象、读写类型。

## 2. 程序序（po）

`ar::BasicBlock::_successors` 给 CFG 后继。同一线程内，事件沿 CFG 的先后就是 po。

- **MUST 边**：直线代码段里的 po（无分支时必然成立）。
- **分支/循环**：po 变成 CFG 路径关系，要发 `exec(e)`（事件是否被执行）布尔变量 + 控制流约束（见 §5 的「工程分期」）。

## 3. happens-before（hb）与竞争判定（核心，已用原型验证）

沿用 svcomp.cat 的 SC 语义：`hb = po ∪ 线程内通信 ∪ mutex/原子同步`。

```
对每个事件 e：clock(e) : int        # SC 线性化里的位置（拓扑序）
对每条 MUST hb 边 (x,y)：clock(x) < clock(y)

竞争查询（针对 checker 给的那对 w,m，w=写、m=另一访问，跨线程、同址）：
  SAT(  clock(w) == clock(m)+1  ∨  clock(m) == clock(w)+1 )
```

**「相邻 ⟺ 竞争」已用 `scripts/bmc_race_encoding.cpp` 实测验证**：异锁 SAT、同锁 UNSAT。

## 4. BMC 用哪些、不用哪些（关键架构判断）

| checker 现成的 | BMC 用不用 | 为什么 |
|---|---|---|
| `resolve_points_to`（抽象 points-to） | ❌ 不用 | 它是过近似（per-thread malloc 的多个对象坍缩成同 cell）；BMC 要精确地址相等 |
| lockset（抽象锁集） | ❌ 不用 | 抽象锁集不精确；BMC 要在 SMT 里重算同步 |
| `current_thread_id` | ✅ 用 | 线程归属是静态事实 |
| 竞争对的访问语句（`AccessRecord.stmt`） | ✅ 用 | 定位这两个 load/store 语句 |
| 地址 | ✅ **编码成 SMT 值 + 判相等** | Dartagnan `sameAddress = mustAlias ? true : equal(addr_a, addr_b)` |

## 5. soundness 的精确条件（2026-10 调研修正，取代之前的草率结论）

一个健全的 BMC 必须**同时**具备（对照 Dartagnan 源码 + 逐个 FP 文件核实）：

1. **精确别名 = 地址编码 + 相等判定**：把每次访问的**地址**编码成 SMT 值，`sameAddress = equal(addr_a, addr_b)`。**不能**复用抽象 points-to 的过近似。这是消除 per-thread malloc / container_of / thread-id 类 FP 的唯一办法。
2. **完整 HB = 所有同步原语**：mutex（acquire/release **资源语义**，含「永久锁」time_var_mutex 那种 lock 后不 unlock）、cond（signal→wait）、sem（post→wait）、barrier、spin、create/join、原子 acquire/release，全部编码成 HB 边。
3. race = sameAddress ∧ 一个写 ∧ 跨线程 ∧ 非原子 ∧ 相邻（已验证）。

**第一版只做了「mutex 的 unlock→lock 边」**，缺 (1) 整个地址编码、(2) cond/sem/barrier/spin/永久锁 —— 这就是实测 21 个 FP 的来源（cond 变量 3 个、信号量 1 个、每线程 malloc 别名 8 个、container_of 2 个、永久锁 1 个、其余若干）。

## 6. 工程分期（修正后）

**第一期（最小健全子集：全局变量 + mutex + create/join）**
- 只对 checker 报的那一对访问，做路径敏感有限展开（展开界 K）。
- **别名**：只处理两访问都是**同一个 GlobalMemoryLocation**（全局变量地址唯一标识，不用 SMT 编码地址；堆/容器指针 → 保守 UNKNOWN）。
- **同步**：mutex + **create/join**（create-HB、join-HB 都要建模——pthread-numerical-integration 靠 join 同步 `area`）。且要求**锁配对平衡**（time_var_mutex 那种「lock 不 unlock」→ 保守 UNKNOWN）。cond/sem/barrier/spin/原子 → 检测到就保守 UNKNOWN。
- **循环（DAG-only 能力边界）**：展开器只做 DAG（直线+分支）；访问块的**前向路径上有回边（循环）**——即访问块从某循环头可达——就把路径标记为 `incomplete` → 保守 UNKNOWN（被跳过的循环体里可能藏 join/create/mutex，pthread-numerical-integration 的 join 循环正是如此）。循环在访问**之后**（不可达访问块）或分叉支路上、且不影响本访问的，不拦截。**这是「能力边界」，不是 bug**——后续加 bound 展开 + join-HB 建模可攻克，见 §9。
- 编码：po + mutex 同步（互斥析取）+ create/join 边 + clock + 相邻查询（已实现 `encode.hpp`，create/join 边待加）。

**第二期（加精确别名 + 更多同步）**
- 地址 SMT 编码（`address(e)` + `equal`）→ 支持堆对象、容器指针、thread-id 槽位。
- cond（signal→wait）、sem（post→wait）、barrier 建模。
- mutex 的完整资源语义（处理永久锁）。

**第三期（工程化）**
- 从「只判一对」扩展到「判所有 demote 对」。
- 求解器缓存、增量求解。

## 7. 集成点

在 `DataRaceChecker` 的析构里，`--demote-race-to-unknown` 把竞争对判成 UNKNOWN 之后，**对每个满足第一期健全门（全局变量 + 平衡 mutex + 无其它同步）的 UNKNOWN 对调用 BMC**：

```
对每个 demote 成 UNKNOWN 的 (a, b)，先过健全门：
  门 = 同 GlobalMemoryLocation ∧ 锁平衡 ∧ 无 cond/sem/barrier/spin/原子
  过门 → BMC.check_race → SAT 报 FALSE（+1），UNSAT 保持 UNKNOWN
  不过门 → 保持 UNKNOWN
```

result：BMC 只「找回 TP」，从不「证明 SAFE」，且健全门保证报 FALSE 的必是真竞争 → **FN=0 且 FP=0**。

## 8. 已摸清的点（原 §7 的解答）

1. **精确别名**：IKOS 没有独立 alias 分析。正确做法是 Dartagnan 的 `sameAddress`——第一期用全局变量的语法同一性，第二期把地址编码成 SMT 值判相等。
2. **路径敏感展开**：IKOS 无现成 unroller，已手写 `bmc/unroll.hpp`（DAG DFS + 回边检测）。
3. **mutex 对象身份**：语法 arg0 指针同一性（`mutex_id`，已实现）；但需补「锁配对平衡」检测处理永久锁。

## 9. 🚩 能力边界 = 潜在独特优势（重点标记，后续攻关）

第一期 BMC 对「join-数组-循环」保守报 UNKNOWN（`pthread-numerical-integration` 是唯一剩 FP）。
但这个用例**恰好暴露了 IKOS 相对现有工具的两个独特优势**，值得重点攻关：

| 难点 | 现有工具现状（已实测） | IKOS 的优势 |
|---|---|---|
| **double 浮点运算** | **Dartagnan 直接 CRASH**（`UnsupportedOperationException: Unsupported type in double`）；goblint / UAutomizer / CPAchecker 都 UNKNOWN 或 error | IKOS 用 APRON（多面体/octagon）**天然支持 double**，数值域不缺 |
| **join 句柄是数组 + 变量下标（循环）** | 现有工具要么不建模 join-HB、要么数组摘要不够 → 全 UNKNOWN，**没有工具能解** | IKOS 是**抽象解释**框架，天然适合上 **Cousot & Logozzo POPL'11 分区数组域（partitioned array summarization）**，正是「thread-id 槽位桶（~22 FP）+ join-数组句柄」这类「循环 + 数组 + 句柄」问题的正解 |

结论：这不是「我能力不够所以放弃」，而是「**别人根本没资格碰这个题（double 就崩了），我有两条别人没有的路（double + 数组摘要域）**」。当前记为 UNKNOWN 是保守 gate 的副作用；一旦上数组摘要域，可同时攻下「thread-id 桶 ~22 FP」和这个 join-数组用例，是登顶 no-data-race 榜的关键杠杆。参见 memory `join-array-double-unique-advantage`。

