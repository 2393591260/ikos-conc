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
