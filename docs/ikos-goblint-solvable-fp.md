# goblint 能解、IKOS 不能解的 FP（heap-board 修复后）

> 交叉验证基准：goblint（svcomp 全配置）判 SAFE、IKOS（-m32）判 RACE 的任务。
> 时间：2026-09-30。此时基线 FP=98、FN=0。
> 已回收：`fc8339c`（fresh 堆节点）5 个 list/alloc 族 + `ab07166`（变量下标堆指针黑板）2 个 weaver，
> 本表是**剩余 13 个**。
> 再回收（本表更新后）：join 句柄 3 个（`build_thread_creators` 归因 helper 里的 create + 全局 pthread_t
> 句柄写入 + `[0,S]` 区间 join），FP 98→95；join-then-create 4 个（pthread_create 处记录父线程 MUST joined
> 摘要、子线程入口继承），FP 95→91；stacksave/stackrestore 排除（VLA 清理的 ⊤ 栈指针写）3 个，FP 91→88。
> 当前**剩余 6 个**。

## 按机制分 6 组

| # | 机制 | 数量 | 文件 | IKOS 可追? |
|---|---|---|---|---|
| 1 | ⊤ points-to（全局指针+变量下标） | 3 | weaver/chl-poker-hand-subst、popl20-bad-commit-1/2 | 中 |
| 2 | join 句柄（helper 里 create + 全局句柄 + 条件 create） | ~~2~~ **0（已回收）** | ~~ldv-races/race-1_1-join、race-1_3-join~~（另收 race-1_2-join） | ✅ |
| 3 | thread-id 域（线程 id 当数组下标） | 1 | pthread-race-challenges/per-thread-array-init | 大 |
| 4 | 不相交分区 / 跨槽共享 | 1 | goblint-regression/28-race_reach_92-evilcollapse_racing | 大 |
| 5 | 自旋锁 / __VERIFIER_atomic_acquire-release | 2 | pthread-ext/29_conditionals_vs、43_NetBSD_sysmon_power_sliced | ❌ 红线 |
| 6 | ~~伪原子段并发（reorder_5）~~ | ~~1~~ **0（已回收）** | ~~pthread/reorder_5~~ | ✅ |

## 说明

- 第 2 组根因（已修，3 处）：(1) `build_thread_creators` 只把 create 归因到 main/thread-entry，helper
  里的 create 被跳过 → `_thread_creators[child]` 空 → create-HB 失效；(2) 全局 `pthread_t` 句柄不写 site id
  （handle-init 只认 `LocalMemoryLocation`）→ join 读到 [0,0]；(3) 条件 create 使句柄区间 [0,S] 非 singleton
  → join 落空。修法 = helper 反向可达性归因（地址被取 → ⊤ 全入口）、全局句柄写 site id、`[0,S]` 区间 join ub。
- 第 6 组已拆开：`bigshot_s/bigshot_s2/singleton/singleton_with-uninit-problems` 是 **join-then-create**
  缺口（父线程 join T 后再 create U，T 已终止，但子入口 joined 摘要为空），已修——pthread_create 处记录父线程
  MUST joined 摘要（按 child+create_site 覆盖、读侧跨 site 取交集）、子入口继承。剩 `reorder_5` 是另一种根因
  （setThread/checkThread 并发访问 a/b，靠 `__VERIFIER_atomic` 段同步），单独排查。
- 第 5 组不可做：`__VERIFIER_atomic_acquire/release` 是 IKOS 红线下的刻意保守
  （当原子会吞 `40_barrier_vf-b` 真竞争），goblint 的 SAFE 以其自身 soundness 为代价。
- 第 1 组（weaver）与已完成的 fresh-heap-node 是**相邻机制**（都是堆身份精确化），
  是最先攻的方向。
