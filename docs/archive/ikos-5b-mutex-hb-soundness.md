# mutex-HB soundness 设计（Phase 1 结论：抽象 thread-modular 下不可 sound 落地）

> 结论先行：**纯 mutex-HB（LockRelease 式「上次 release」epoch）在 IKOS 的抽象 thread-modular 架构下无法 sound 落地**。它不是「难」，是被架构排除——它需要路径敏感的交错信息，而抽象解释天生没有。

## 1. 形式化：mutex-HB 边什么时候是 sound 的

HB 边 `unlock(m) in T1 → lock(m) in T2` 是 **must 边**（可用于压制竞争），当且仅当：

> 在**所有**执行中，T2 的 `lock(m)` 都在 T1 的 `unlock(m)` **之后**。

分两类：

- **结构性成立（sound）**：T1 的 unlock 和 T2 的 lock 在一条**既有的 HB 链**上，例如 `unlock(m); pthread_create(T2)` → T2 的 lock 必然在 T1 的 unlock 之后（经 create 边）。
- **交错性成立（需路径敏感）**：T1、T2 独立并发，m 是唯一同步，T2 的 lock 是否在 T1 的 unlock 之后**取决于交错**。

## 2. 反例：抽象下「所有 unlock 先于所有 lock」会漏报（FN）

```c
// T1 与 T2 由 main 独立 create，并发
T1:  data = 42;   unlock(m);
T2:  lock(m);     read = data;
```

存在执行：T2 的 `lock(m)` **先**于 T1 的 `unlock(m)`。此时 T1 写 data 与 T2 读 data **并发 = 真竞争**。

抽象分析里若宣称「`unlock(m) → lock(m)`」的 HB 边，就会得出「T1 写 HB T2 读 → 无竞争」，**吞掉这个真竞争 = FN = 破红线**。

**结论（定理）**：无交错信息的抽象分析中，「mutex HB 边」要么是结构性成立的（= 已有的 join/create HB，无新增），要么是交错性成立的（= 不可 sound 证明）。**不存在第三种 sound 的 mutex-HB**。

## 3. 为什么 CPAchecker 能做，IKOS 不能

CPAchecker 的 `LockRelease`（每锁记「上次 release 的线程」）**依赖 ThreadingCPA 的路径敏感交错枚举**——它枚举所有交错，所以「上次 release 是哪个线程」是**精确的**，HB 边只加在真实先后的交错上。

IKOS 是**抽象解释**（各线程分开分析 + 黑板 join），**没有交错**。「上次 release」在抽象里退化成「可能是任意线程」，一旦并进去就是 §2 的 FN。

**差距不在 LockRelease 这个机制（它很简单），在「路径敏感交错」这个引擎能力。** 而这是 IKOS thread-modular 的架构底色，不是 checker 层能补的。

## 4. privatization 还额外需要 blocking 建模

即使绕开上面的定理，`privatized_40-traces-ex-6_true` 的 soundness 还依赖一个**case 分析**：

- Case A（main 的 D 临界区先）：main 持 A 不放 → t_fun 的 `lock(A)` **阻塞** → t_fun 的 `g=2` **不可达** → 无竞争。
- Case B（t_fun 的 D 临界区先）：靠 mutex-HB 边（即 §2 里不可 sound 的那条）。

Case A 需要**阻塞语义**（lock 会阻塞）——抽象分析把 lock 只当「加进锁集」、从不阻塞，t_fun 的 `g=2` 会被当成可达。所以即便有 mutex-HB，也缺 Case A 这一半。

## 5. 结论与后续

1. **纯 mutex-HB 在 IKOS 不可 sound 落地**——根因是「抽象解释没有交错」，不是实现难度。Phase 1 到此为止，**不建议进入实现**。
2. **privatization 双重不可行**：既需交错（Case B），又需阻塞（Case A），两者都超出抽象 thread-modular 的能力。
3. **已有的 join/create HB 已经是「结构性 mutex-HB」的全部**：`unlock → create → lock` 这类链，create 边已经覆盖，无需重复。
4. **对 #5b 的最终定性**：它和锁无关算法（dekker 等）一样，属于「**路径敏感引擎才有的能力**」——CPAchecker 能、goblint（`mHP` + 区域域）能，IKOS 的抽象解释**不能**。

**净收益**：这份设计在写任何代码前就排除了一个会破红线的实现，并把「#5b 不可行」的根因（缺交错/阻塞，而非缺一个 HB 时钟）固化下来了。后续不必再在 mutex-HB 上投入。
