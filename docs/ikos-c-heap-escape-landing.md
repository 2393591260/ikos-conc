# C 桶（context-sensitive malloc / 堆逃逸）IKOS 落地 soundness 设计

> 目标：把 goblint region 域（已 100% 确认 = 「fresh 分配 → 未发布前不竞争」）在 IKOS 里 sound 地落地。本文只做理论推演，不动代码。

## 1. 已确认的 goblint 机制（trace 铁证）

`--trace access` 观测到：
- t1 的 `init(p)` 写 `p->datum` → region `no region`（fresh bullet）；
- t2/t3 的 `p->datum` 访问 → region `{A, B}`（已发布）；
- `may_race` 里 `None, _ -> false`（**no region = fresh = 不竞争**）。

**核心语义**：`malloc` 结果是 **fresh（线程私有，其它线程不可达）**，直到被存进共享位置才「发布」。发布前访问它不竞争；发布后按正常判定。这是**纯抽象解释**（bullet + 不相交分区），非路径敏感。

## 2. IKOS 里的 sound 规则

> **一个 DynAlloc 若「未被其它线程可达」（未发布），它就是线程私有 → 其访问不跨线程竞争。**

这是标准逃逸分析的推论，sound 性无争议。关键是「未发布」如何判定。

## 3. ⚠️ 关键约束：必须 flow-sensitive，不能用全局 `_escaped_locs`

**事实**（已查 `function_fixpoint.hpp`）：checker 在 fixpoint **收敛后**一次性 `run_checks`，拿到的是每个语句的**收敛态 `inv`**。

**推论**：全局 `_escaped_locs`（`concurrent_global_env.hpp` 的单调累加集）在 checker 运行时已经是**全传播后**的状态——t1 自己的 `insert(p,A)` 已经把 p 标记逃逸，于是 `init(p)` 的访问也看到 p「已逃逸」。

- 若用全局 `_escaped_locs` 判「未逃逸 → 线程私有」→ 因为逃逸是全传播的，**几乎不会有任何 DynAlloc 被判为未逃逸** → 不产生 FP 收益（白改）。
- 更糟的方向：若反过来「已逃逸才共享」，会把**发布前的访问也误判为共享**（保守方向，只多 FP 不 FN，但没收益）。

所以 **「发布」必须 flow-sensitive**：在每个访问点，用**该语句的 `inv`** 里的 points-to 图判定「这个 DynAlloc 此刻是否从共享位置可达」。

## 4. 「发布」的 flow-sensitive 定义

在访问点（有 `inv`）上，DynAlloc `d` 是**已发布** iff `d` 从以下任一「共享根」经 points-to 图**可达**：

1. 全局变量（global memory location）；
2. 已逃逸的栈局部 / 线程实参（`record_spawn_arg` / `join_global_pointer` 已标）；
3. 一个**已发布**的堆节点的字段（`A->next = p`：A 已发布 ⟹ p 也发布）。

这是标准的「堆可达性逃逸闭包」。它是 sound 的过近似：只可能**多标**已发布（保守，多 FP），绝不**漏标**（不 FN）。

**注意**：IKOS 现有 `_escaped_locs` 只覆盖第 1、2 条，**缺第 3 条**（堆字段发布）——这正是 list2_racefree 的 `A->next = p` 这条路径。第 3 条是本次必须新加的核心。

## 5. 落地形态（两个候选）

**方案 A：抽象域加「published 集」**（与 goblint 最接近）
- 给 `value::AbstractDomain` 或 checker 加一个 flow-sensitive 的 `published : set<DynAlloc>` 维度；
- `malloc` → 不加入；`store ptr = val` 且 `ptr` 是共享根或已发布节点的字段 → `val` 的 DynAllocs 加入 published；
- join = 并集（MAY），单调，收敛。
- **优点**：与 goblint 的 bullet/物化直接对应，语义清晰。
- **代价**：要动数值域/执行引擎的 Store/join，涉及面较大。

**方案 B：checker 里按 `inv` 现算可达性**（不改域，只改 checker）
- 在 `check_load/store` 时，对每个 DynAlloc 目标，用当前 `inv` 的 points-to 图做一次「从共享根可达」的图搜索；
- **优点**：不动数值域，改动集中在 checker。
- **代价**：每次访问做可达性搜索（图遍历），且 points-to 图要从 `inv` 里现提取（IKOS 的 points-to 是「指针变量 → 目标集」，反向「谁指向 d」要现建），实现也不轻。

## 6. soundness 论证（红线 FN=0）

1. 「发布」闭包（第 4 节）是 sound 的过近似：**漏标发布 = FN，多标发布 = FP**。闭包三条齐全则不漏标。
2. 「未发布 → 线程私有 → 不竞争」是逃逸分析的经典结论：未发布的堆节点只有分配线程持有其指针，其它线程无从访问。
3. flow-sensitive 的 `inv` 是收敛态，单调；published 集单调（MAY 并集），收敛不破 sound。
4. **剩余风险点**：第 3 条（堆字段发布）的可达性传播是否完整——若某个发布路径（如 `struct { ... ; void* p; }` 存进全局、再经多层字段解引用）没被闭包覆盖，就是 FN。落地时必须把「堆字段发布」的触发点（Store 到 heap field）与 goblint 的 `assign`/`add_set` 逐条对齐核验。

## 7. 结论

- **可落地、sound 方向明确**：给 IKOS 加 flow-sensitive 的「堆节点发布/逃逸」判定，`malloc` 结果发布前线程私有、不竞争。
- **核心新增**：第 4 节第 3 条——「存进已发布堆节点的字段 → 发布」的可达性闭包（IKOS 现缺）。
- **工程量**：中等偏上（方案 A 动域、方案 B 动 checker 且要可达性搜索），soundness 论证见第 6 节。
- **建议**：先做方案 A 的最小原型（只加 published 集 + Store 触发点），拿 list2_racefree + 全量 A/B 验 FN=0 与 FP 回收，再决定是否铺开。

> 依赖的前置事实：goblint 机制已 100% 确认（见 `docs/ikos-5b-mutex-hb-landing.md` 同期的 region 域反编译 + trace 观测），本设计承接之。

## 8. ⚠️ 修正（实现原型时发现的 soundness 反例）

方案 A 的最小原型（published 集挂在线程局部的 lockset）实现到一半，推演出一个 **FN 反例**，证明**「published 必须是全局事实，不能是线程局部」**：

```c
struct s *gp;              // 全局
t1: p = malloc; gp = p;    // t1 把 p 存进全局 → p 发布
    p->datum = 1;          // 访问 p（已发布，应竞争）
t2: q = gp;                // t2 从全局读到 p
    q->datum = 2;          // 访问 p（已发布，应竞争）
```

t1 的 `p->datum=1` 与 t2 的 `q->datum=2` 访问**同一个 p、无锁 = 真竞争**。但线程局部的 published 集：
- t1 的 lockset 有 p（t1 发布了它）；
- **t2 的 lockset 是 ∅（t2 从没发布过 p）** → t2 的访问被误判「未发布 = 线程私有」→ 吞掉真竞争 = **FN**。

**根因**：「p 是否发布」是**跨线程可达性**（全局事实），任何线程发布后，所有线程都能到达。线程局部的 published 集把「本线程是否发布」错当成了「全局是否发布」。

## 9. 修正后的正确形态（goblint 的两层结构）

goblint 的 region 域是**两层**：
- **GLOBAL RegPart**（不相交分区，单调）：哪些 region 已被物化（发布）。
- **LOCAL RegMap**（每线程、flow-sensitive）：线程入口从 GLOBAL 初始化，线程内流转。

对应 IKOS 的正确落地需：
1. **GLOBAL published 集**（`ConcurrentGlobalEnv` 黑板，单调）：`join_global_pointer` / `record_spawn_arg` / 堆字段发布都往里写。
2. **LOCAL 快照**（线程入口从 GLOBAL 取，flow-sensitive）：checker 在访问点用 LOCAL 快照判「此刻是否发布」。

**这是中等偏上的特性（两层 + 可达性闭包 + flow-sensitivity），不是几行的 patch。** 原型已回退（未提交），结论固化于此。

## 10. 最终设计（比两层更简单：LOCAL「fresh」MUST 集）

深读 `thread_modular.cpp` 的 fixpoint 后，得到一个更简单且 sound 的方案，**不需要 GLOBAL published 集**：

**核心**：把「发布」反转成「fresh」，且 fresh 是 **MUST 集**（join = 交集、⊤ = ∅，见 CLAUDE.md gotcha #3）。

- 域里加一个 flow-sensitive 的 `_fresh_heap_nodes`（MUST 集）：`malloc` 的结果「确实还没发布」才在集里。
- `malloc` → 结果加入 fresh。
- store 指针进「共享位置」（全局 / 线程实参 / **非 fresh 堆节点的字段**）→ 从 fresh 移除。
- checker：fresh（在集里）→ 线程私有 → 不竞争。

**为什么这个 LOCAL 集 sound（解决了 §8 的 FN 反例）**：
```c
t1: p=malloc(→fresh); init(p)(fresh→不竞争); gp=p(存全局→移除p);
    p->datum=1(p 非 fresh→共享→查)
t2: q=gp(读全局，q 从没 malloc→不在 t2 的 fresh); q->datum=2(非 fresh→共享→查)
```
- t1 的 `p->datum=1` 和 t2 的 `q->datum=2` 都「非 fresh」→ 都当共享 → **竞争被检出**（不 FN）。
- t1 的 `init(p)` 在 `gp=p` 之前，p 仍在 fresh → 线程私有 → 抑制（不 FP）。

**关键**：fresh 是「本线程 malloc 后还没存进共享位置」——这是**线程局部、flow-sensitive 的 MUST 事实**，不是全局可达性，所以不需要 GLOBAL 集，也不会把 t2 的访问误判成 fresh（t2 从没 malloc 过 p，p 不在 t2 的 fresh 里）。

**flow-sensitivity 的达成**：fresh 集在域里（flow-sensitive），thread 入口**不从黑板初始化**（初始 ⊤=∅），线程内 `malloc` 加、store 减。Phase 4 重跑 intra-procedural fixpoint 时，init(p) 处 fresh 集 = {p}、insert 处移除 p——flow-sensitivity 自然保留，不受单调黑板污染。

**disjointness 分区（arraycollapse 的跨槽共享）是本方案不覆盖的独立难题**——list2_racefree 族只需 freshness。
