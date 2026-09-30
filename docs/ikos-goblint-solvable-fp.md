# goblint 能解、IKOS 不能解的 FP（fresh-heap-node 修复后）

> 交叉验证基准：goblint（svcomp 全配置）判 SAFE、IKOS（-m32）判 RACE 的任务。
> 时间：2026-09-30。此时基线 FP=100、FN=0。
> 上一轮 `fc8339c`（fresh 堆节点）已回收 20 个 goblint 解里 5 个 list/alloc 族，
> 本表是**剩余 15 个**。

## 按机制分 6 组

| # | 机制 | 数量 | 文件 | IKOS 可追? |
|---|---|---|---|---|
| 1 | ⊤ points-to（全局指针+变量下标） | 5 | weaver/chl-poker-hand-subst、loop-tiling-eq、popl20-bad-commit-1/2、popl20-prod-cons-eq | 中 |
| 2 | join 句柄 ⊤（tids[] 数组读 ⊤） | 2 | ldv-races/race-1_1-join、race-1_3-join | 中 |
| 3 | thread-id 域（线程 id 当数组下标） | 1 | pthread-race-challenges/per-thread-array-init | 大 |
| 4 | 不相交分区 / 跨槽共享 | 1 | goblint-regression/28-race_reach_92-evilcollapse_racing | 大 |
| 5 | 自旋锁 / __VERIFIER_atomic_acquire-release | 2 | pthread-ext/29_conditionals_vs、43_NetBSD_sysmon_power_sliced | ❌ 红线 |
| 6 | 其他（atomic 段 + malloc 逃逸混合） | 4 | pthread/bigshot_s、bigshot_s2、reorder_5、singleton_with-uninit-problems | 待查 |

## 说明

- 第 5 组不可做：`__VERIFIER_atomic_acquire/release` 是 IKOS 红线下的刻意保守
  （当原子会吞 `40_barrier_vf-b` 真竞争），goblint 的 SAFE 以其自身 soundness 为代价。
- 第 1 组（weaver）与已完成的 fresh-heap-node 是**相邻机制**（都是堆身份精确化），
  是最先攻的方向。
- 第 6 组根因未逐个钉死，需单独排查。
