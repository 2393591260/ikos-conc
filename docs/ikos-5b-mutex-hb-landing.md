# #5b 互斥传递 HB —— IKOS 落地方案调研

> 目标：判断在 IKOS 里补「互斥传递 happens-before」的可行性，并给出落地点（哪一层、动哪些文件）。

## 1. 语义（#5b 到底要建什么边）

标准 **mutex happens-before**（与 Eraser+HB、ThreadSanitizer 同源）：

> `unlock(m)` in 线程 T1 → 下一个 `lock(m)` in 线程 T2，建立 HB 边。

判定器已有的「公共锁」检查（lockset 交集）只覆盖「两访问持同一把锁」。#5b 要的是：**两访问持不同锁、甚至无锁，但被某把 mutex 的临界区串行关系隔开**。

### 两个 #5b 文件的具体需求

**（a）`privatized_40-traces-ex-6_true.c` —— 纯 mutex HB（可做）**

```c
t_fun: lock D → lock A → g=1 → unlock A → g=2 → unlock D
main:  lock D → lock A → unlock D → 读 g（持 A）
```

竞争对：t_fun 的 `g=2`（持 {D}）vs main 的读（持 {A}）——无公共锁。HB 论证：D 临界区把两者串行（t_fun 的 `g=2` 在 D 临界区内、main 的读在 D 临界区后），case 分析可证无竞争。**只需 `unlock D → lock D` 这一条 mutex HB 边**。

**（b）`time_var_mutex.i` —— 标志式 HB（难，暂不建议）**

```c
allocator:  lock m_inode → lock m_busy → busy=1 → unlock m_busy → inode=1 → block=1 → unlock m_inode
de_alloc:   lock m_busy → if(busy==0){ block=0 } → unlock m_busy
```

`block` 由 m_inode（allocator）和 m_busy（de_alloc）两把不同锁保护，靠 **`busy` 标志值**（`busy==0` 读 ⟹ `busy=1` 写尚未发生）传递 HB。这需要「**标志值关联**」——`busy==0` 的分支条件与 `busy=1` 的写建立 HB，比纯 mutex HB 又难一档（接近条件变量建模）。**本方案不覆盖，单独立项。**

## 2. goblint / CPAchecker 怎么做（参照）

- **goblint**：`mutexEventsAnalysis.ml` 只负责在 lock/unlock 点**发出 `Events.Lock`/`Events.Unlock` 事件**；race 判定在 `domains/access.ml` 的 `group_may_race`，它把「各分析的 may_race 谓词」组合起来（lockset / mhp / mutex-HB / region 各自提供谓词，**全部通过才算不竞争**）。mutex HB 是**独立的一路谓词**，不塞进 lockset。
- **CPAchecker**：ThreadingCPA 维护 lock/unlock 事件 + happens-before，`Locks` 域承担 mutex HB。

**共同点**：mutex HB 是「lock/unlock 事件流」上的 happens-before 关系，独立于 lockset，race 判定时作为一条「不竞争」判据并入。

## 3. IKOS 现状与落地点

IKOS 现有 HB 只有 join-HB（`joined_threads` digest）+ create-edge-HB（`spawned` digest），**完全没有 mutex HB**。判定在 `data_race.cpp` 析构时离线配对。

| 层 | 文件 | 现状 | 要加什么 |
|---|---|---|---|
| 锁集域 | `core/.../lockset/lockset_domain.hpp` | MUST mutex/read/joined + MAY spawned/signaled/awaited，无时钟 | **新增 HB 时钟 digest**（见 §4） |
| mutex 语义 | `execution_engine/concurrent_semantics.hpp` `exec_pthread_mutex_lock`(L399) / `exec_pthread_mutex_unlock`(L723) | 只 `add_lock`/`remove_lock` | lock 时**读 epoch 并入时钟**，unlock 时**写 epoch** |
| 黑板 | `core/.../concurrent_global_env.hpp` | 全局变量值、join summary、堆指针 | **每 mutex 一个 epoch**（跨线程共享） |
| 记录 | `checker/data_race.cpp` `check_load`/`check_store` | snapshot locks/joined/spawned | **snapshot 当前 HB 时钟** |
| 判定 | `checker/data_race.cpp` 析构配对 | lockset 交集 + join/create HB | **新增「A HB B 则不竞争」分支** |

## 4. 表示（HB 时钟）与 soundness 红线

mutex HB 的经典表示是 **vector clock**（每线程一个分量）：

- 每把 mutex `m` 记 `epoch[m]`（上次 unlock 时的时钟，存黑板）。
- `lock(m)`：`clock[T] := clock[T] ⊔ epoch[m]`。
- `unlock(m)`：`epoch[m] := clock[T]`（T 的时钟写入 m）。
- 访问时 snapshot `clock[T]`。
- 配对判定：`clock[A] ⊑ clock[B]`（A HB B）⟹ 不竞争。

**soundness（FN=0）的难点不在实现、在于 thread-modular 抽象下的时钟语义**：

- IKOS 的 thread-modular 是**各线程分开抽象分析 + 黑板 join**，没有具体交错。vector clock 要在这种抽象下**保守过近似**（宁可「不可比」多报 FP，不可「错误可比」漏报 FN）。
- 这是本方案**最大的风险点**，不是代码量问题。

**保守方向**（守住 FN=0）：HB 只做「必然先后」的下近似——只有能从 lock/unlock 事件链**证明** A 必然先于 B 时才算 HB，否则放行竞争（多 FP）。与现有 join/create HB 的「must 下近似」同源，符合 CLAUDE.md 的对偶性总述。

## 5. 工程量与结论

- **工程量**：一个 HB 时钟 digest + 黑板 epoch 表 + lock/unlock 更新 + 判定分支，**约等于现有 join-HB 的量级**（数百行 + 一条保守方向论证）。不是几十行的 patch。
- **回收**：纯 mutex HB 至少回收 `privatized_40-traces-ex-6_true`（1 个确定），理论文档标 #5b 约 9 例（其余多为标志式/锁耦合变体，本方案不覆盖，需另立）。
- **红线**：必须在 lock/unlock 事件链上做「must 下近似」的 HB，落地前用真竞争样例 A/B 验 FN=0。

## 6. 建议（go / no-go）

**go 的条件**：按「通用 mutex-HB 机制」立项（不是单文件 patch），先做 §4 的保守方向论证 + 探针验证 `unlock D → lock D` 边在 IKOS 的 fixpoint 里确实可测，再写代码。

**若时间紧**：这个 1-file 的纯 mutex HB 性价比偏低（108 个 FP 里只确定回收 1 个），可**降级为 backlog**，优先 B 桶 thread-id 域（22 个）或提交前的 Release/归档收尾。

**明确 no-go**：`time_var_mutex`（标志式 HB）和 `29_conditionals_vs`（自旋锁，红线下的刻意保守，见 §7）。

## 7. 附：三个「不能碰/暂不碰」的定性

| 文件 | 定性 | 原因 |
|---|---|---|
| `29_conditionals_vs` | **红线刻意保守** | `classify_atomic_intrinsic` 明注释：`__VERIFIER_atomic_acquire/release` 是手写 RACY 标志，当原子会吞掉 `40_barrier_vf-b` 真竞争（FN） |
| `time_var_mutex` | 标志式 HB | 需 busy 标志值关联，接近条件变量建模 |
| dekker/peterson/lamport/szymanski | acquire/release HB | C11 `_Atomic`，需内存序建模（#3），短期不可行 |
