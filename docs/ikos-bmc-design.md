# IKOS BMC 模块设计 —— AR → SMT 映射（demote 后找回真竞争）

> **一句话目标**：接在 `--demote-race-to-unknown` 之后，对每个 demote 成 UNKNOWN 的抽象竞争对，
> 用 Z3 精确判定「这两个访问是否真的能并发」。SAT ⟹ 报 FALSE（找回 TP）；UNSAT / 无法证明 ⟹
> 保持 UNKNOWN。**永远不报 TRUE**（证明 SAFE 交给抽象解释）。
>
> **当前状态的权威描述**：见 `ikos-race-theory-mapping.md` §2.1.17（7 道 sound gate）与 §2.4
> （两层判定架构）。本文档聚焦**设计依据 + 演进史 + 未做项**，不重复 gate 清单。
>
> 落地基线：**TP=85 / FP=0 / TN=709 / FN=0 / unknown=235 = 1503 分**（冻结集，`IKOS_BMC_RECOVER=1`）。

---

## 1. 理论前提：SV-COMP 用 SC，不是弱内存模型

- SV-COMP no-data-race 写死 `cat/svcomp.cat`（Dartagnan-SVCOMP.sh 里 `cat/svcomp.cat`），**不是 RC11**。
  所以 IKOS 的 BMC **只需要「SC 下的 race 检测」，不需要 acquire/release/fence/弱内存序**——难度远低于直觉。
- `svcomp.cat` 的 happens-before：`hb = po | (com & int) | (rmw^-1? ; (com & ext))`。
  即 HB = 程序序 ∪ 线程内通信 ∪ 跨线程同步（mutex 的 unlock→lock、create/join 折进 `po`）。
- **推论**：BMC 的 HB 编码只需「po + mutex 互斥 + create/join」，不必建模 rmw 链、不必建模弱序。

## 2. 复用 checker 已有产物（不要重写）

BMC **不是**从零扫 AR，而是**接管 checker 析构里被判为「潜在竞争」的那一对访问**（`_accesses` 里的两个 AccessRecord），
只对它们精确判定，查询面窄到 O(1) 对：

| checker 现成的 | BMC 用不用 | 为什么 |
|---|---|---|
| 访问事件 / 线程归属 / 竞争对语句 | ✅ 用 | 定位这两个 load/store 语句 + 所属线程入口 |
| `resolve_points_to`（抽象 points-to） | ❌ 不用 | 它是**过近似**（per-thread malloc 多对象坍缩成同 cell）；BMC 要精确地址相等 |
| lockset（抽象锁集） | ❌ 不用 | 抽象锁集不精确；BMC 在 SMT 里重算同步 |

## 3. 事件模型 + 判定

```
内存事件：ar::Load → Read；ar::Store → Write
同步事件：pthread_mutex_lock/unlock → Lock/Unlock（锁对象 = arg0，mutex_id 语法同一性）
          __VERIFIER_atomic_begin/end → Lock/Unlock(PSEUDO_ATOMIC_LOCK)
          pthread_cond_wait(&c,&m) → Unlock(m)+Lock(m)（POSIX release→重获；signal/broadcast 无 HB）
          pthread_create/join → Spawn/Join（HB 边，**尚未编码**，见 §8）

编码（encode.hpp / check_race）：
  po：路径内相邻事件 clock 递增
  mutex 互斥：同一把锁 Unlock→Lock 二选一布尔
  竞争查询：两个访问能否在 SC 线性化里相邻（clock 差 1，两方向各查一次）→ SAT = 竞争
```

「相邻 ⟺ 竞争」已用 `scripts/bmc_race_encoding.cpp` 原型验证（异锁 SAT、同锁 UNSAT）。

## 4. soundness 的精确条件（对照 Dartagnan + 逐 FP 文件核实）

一个健全的 BMC 必须**同时**具备：

1. **精确别名 = 地址编码 + 相等判定**：把每次访问的**地址**编码成 SMT 值，`sameAddress = equal(addr_a, addr_b)`。
   **不能**复用抽象 points-to 的过近似。这是消除 per-thread malloc / container_of / thread-id 类 FP 的唯一办法。
2. **完整 HB = 所有同步原语**：mutex（含永久锁）、cond（signal→wait）、sem、barrier、spin、create/join、原子
   acquire/release，全部编码成 HB 边。
3. **race = sameAddress ∧ 一个写 ∧ 跨线程 ∧ 非原子 ∧ 相邻**。

**第一期只做了「mutex 的 unlock→lock 边」**，缺 (1) 整个地址编码、(2) cond/sem/barrier/spin/永久锁。
缺了的这些原语**不是「编不出」，而是「编了就破坏欠近似 soundness」**——所以用 gate 把含它们的文件
整体挡成 UNKNOWN（见 theory-mapping §2.1.17 的 `has_unmodeled_sync` 门）。

## 5. 工程分期

- **第一期（已落地：全局变量 + mutex + 伪锁 + cond_wait）**：只处理两访问都是**同一个 GlobalMemoryLocation**
  （DynAlloc 排除——per-thread malloc 同入口多实例坍缩成同 cell，放开误报）；同步只 mutex + 伪锁 + cond_wait。
  sem/barrier/spin/C11 原子 → 保守 UNKNOWN。展开器 DAG-only，遇回边标 `incomplete` → UNKNOWN。
- **第二期（未做）**：地址 SMT 编码（`address(e)` + `equal`）支持堆对象/容器指针/thread-id 槽位；cond/sem/barrier 建模；
  mutex 完整资源语义（永久锁）。
- **第三期（未做）**：从「判一对」扩展到「判所有 demote 对」；求解器缓存、增量求解。

## 6. helper 内联（一期落地，commit fa3c4aa）

**根因**：原 BMC 门要求「访问语句直接在线程入口函数里」（`fa->name() == a0.thread_id`），而展开器
`unroll_to(func, target)` 只沿 CFG 走、**不跟进函数调用**。于是访问在 helper 里（如 40_barrier 的
`Barrier2()`、pthread-atomic 的 `__VERIFIER_atomic_dec_a`）时，`fa->name()`（helper 名）≠ `thread_id`
（`thr1`）→ 门失败 → UNKNOWN。

**修复**：`dfs_paths` 遇「有函数体的直接调用」时递归展开 callee 的 CFG（`depth < MAX_INLINE_DEPTH=8`），
把 helper 里的 Load/Store/Lock/Unlock 事件 + target 收进路径；`confirm_race` 的门从「访问必须在 thread 入口」
放宽为「访问在 thread 入口、或在其有限深度的 helper 调用链里」（`bundle->function_or_null(thread_id)` 解析）。

**内联暴露的 4 类 FP 与对应 gate**（实测逐文件核实，每个 gate 只做「不报 FALSE、退 UNKNOWN」）：

| FP 机制 | 代表文件 | gate |
|---|---|---|
| 单调用原子函数内联（`__VERIFIER_atomic_dec_a` 等裸访问） | pthread-atomic | `bmc_skip_inline`（跳过 `__VERIFIER_atomic_*`/`__atomic_*`/`__sync_*`） |
| helper 带循环（DAG-only 展开器不精确） | — | `function_has_loop`（跳过含回边的 helper） |
| 无锁协议（原子区外快照读 × 原子区内写，线性化签名） | elimination_backoff_stack | `function_has_atomic_section`（跳过含 `__VERIFIER_atomic_begin/end` 的 helper） |
| 隔代 create/join（A→C→B 跨线程链，两线程 po+mutex 看不到中间线程 C 的 join） | race-4_1-thread_local_vars | 隔代祖先门（`_thread_ancestors` − `_thread_creators`） |

## 7. 落地结果（实测 A/B 全量）

| 里程碑 | TP | FP | TN | FN | 得分 |
|---|---|---|---|---|---|
| demote 基线（BMC 关） | 0 | 0 | 706 | 0 | 1412 |
| demote + BMC 一期 | 64 | 0 | 706 | 0 | 1476 |
| **+ helper 内联（fa3c4aa）** | **85** | **0** | 706 | 0 | 1497 |
| **+ cast 包裹线程函数（resolve_thread_function）** | **85** | **0** | **709** | **0** | **1503** |

- `resolve_thread_function`：解开 `(void*(*)(void*)) myPartOfCalc` 的 Bitcast 包裹（`build_thread_creators`
  与 `exec_pthread_create` 对齐），+3 TN（pthread-numerical-integration 的 numberOfIntervals 不再误判）。

## 8. 尚未做（后续路线，按杠杆排序）

1. **精确 create/join HB（区间时钟）**：现在的「隔代祖先门」是保守近似，会误挡「隔代真竞争」
   （main 与孙子线程在 create 与 join 之间的竞争，cost ~1 TP）。真正的解法是在 `encode.hpp` 给每个
   线程加区间时钟 `S_T < E_T` + spawn/join 边，才能既不漏报也不误报。直接父子竞争不受当前门影响。
2. **无锁协议建模**：elimination_backoff_stack 的 race-free 是**线性化定理**，不是 happens-before，
   锁集/HB（goblint 同款伪锁）证明不了。当前用保守 gate 挡成 UNKNOWN，不建模。
3. **值域可行性（原二期）**：`int_to_interval` 跳不可达分支防「条件写」FP。当前一期没暴露条件写 FP
   （A/B FP=0），暂不需要；若后续更激进的内联暴露它，再做。

## 9. 🚩 能力边界 = 潜在独特优势

第一期 BMC 对「join-数组-循环」保守报 UNKNOWN（`pthread-numerical-integration`）。但这类用例**恰好暴露
IKOS 相对现有工具的两个独特优势**：

| 难点 | 现有工具现状（已实测） | IKOS 的优势 |
|---|---|---|
| double 浮点运算 | Dartagnan 直接 CRASH；goblint/UAutomizer/CPAchecker 都 UNKNOWN/error | IKOS 用 APRON（多面体/octagon）天然支持 double |
| join 句柄是数组 + 变量下标（循环） | 现有工具要么不建模 join-HB、要么数组摘要不够 → 全 UNKNOWN | IKOS 是抽象解释框架，天然适合上 Cousot & Logozzo POPL'11 分区数组域 |

当前记为 UNKNOWN 是保守 gate 的副作用；一旦上数组摘要域，可同时攻下「thread-id 桶」和 join-数组用例，
是登顶 no-data-race 榜的关键杠杆。参见 memory `join-array-double-unique-advantage`。

---

*本文由 `ikos-bmc-design.md`（一期设计）、`ikos-bmc-helper-inlining-design.md`（二期/helper 内联）、
`ikos-bmc-learning-notes.md`（Dartagnan CAT 讲义）三份合并而成；CAT 讲义的原始推导过程已随 git 历史保留。*
