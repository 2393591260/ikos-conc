# IKOS vs Goblint 竞态基准对比报告

> 生成于 2026-09-23。**只采集对比结果，未改动 IKOS/Goblint 任何代码。**
>
> 输入：sv-benchmarks `c/` 下 1030 个 no-data-race 任务，**两工具跑同一份源文件**（`.i` 任务优先映射到同名 `.c`；
> 30 个只有 `.i` 的任务——libvsync 锁实现、`pthread/fib_*`、`ldv-linux`——直接用 `.i`）。
>
> ⚠️ **口径说明**：IKOS 官方基线（FP=182/FN=0）跑在 `.i`（预处理后）。本报告为对齐 goblint 的 `.c` 前端，
> 统一用 `.c`，故 IKOS 的 FP 由 182 降到 169（部分 `.i` 工具链特定 FP 消失），但**新增 1 个 `.c` 伪影 FN**
> （`thread-local-value-race`，IKOS 在 `.i` 上能抓到 `definitely UNSAFE`，`.c` 上漏报）+ 23 个「两边都失败」的
> 需预处理文件（`INT_MAX`/内核头）。详见第五节。

## 一、总览

| 工具 | TP | FP | TN | FN | 未评分 | Precision | Recall |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **IKOS** | 230 | 169 | 599 | 1 | 31 | 0.576 | 0.996 |
| **Goblint** | 224 | 254 | 517 | 1 | 34 | 0.469 | 0.996 |

**核心结论**：IKOS 精度 **0.576** 显著高于 Goblint 的 **0.469**（IKOS 少 85 个假阳性），
召回率持平（各 1/1 个漏报）。IKOS 在锁集/happens-before/TLS/字符串常量建模上更准；
Goblint 的假阳性集中在未建模的锁协议与折叠内存上。

结论分布：一致 871 ／ 不一致 159 ／ Goblint 异常 34。

## 二、结论一致组（仅计数）

- **Safe == Safe**：497 个文件
- **Race == Race**：374 个文件

## 三、不一致组（重点，逐个列出）

### 3.1 Goblint 假阳性（IKOS 判 Safe 正确，Goblint 误报 Race）：101 个

| 文件 | 期望 | IKOS | Goblint | 根因初判 |
| :--- | :--- | :--- | :--- | :--- |
| `c/pthread-race-challenges/atomic-gcc.c` | Safe | Safe | Race | C11 atomic / acquire-release 缺 HB |
| `c/pthread-divine/tls_basic.c` | Safe | Safe | Race | TLS 指针传播（__thread int* 逃逸） |
| `c/pthread-divine/tls_destructor_worker.c` | Safe | Safe | Race | TLS 指针传播（__thread int* 逃逸） |
| `c/pthread-race-challenges/thread-local-pthread-value.c` | Safe | Safe | Race | TLS 指针传播（__thread int* 逃逸） |
| `c/pthread-race-challenges/thread-local-value-cond.c` | Safe | Safe | Race | TLS 指针传播（__thread int* 逃逸） |
| `c/weaver/clever.wvr.c` | Safe | Safe | Race | weaver 域压缩/对象身份丢失 |
| `c/weaver/fibonacci.wvr.c` | Safe | Safe | Race | weaver 域压缩/对象身份丢失 |
| `c/weaver/mult-comm.wvr.c` | Safe | Safe | Race | weaver 域压缩/对象身份丢失 |
| `c/weaver/mult-dist.wvr.c` | Safe | Safe | Race | weaver 域压缩/对象身份丢失 |
| `c/weaver/mult-flipped-dist.wvr.c` | Safe | Safe | Race | weaver 域压缩/对象身份丢失 |
| `c/weaver/security.wvr.c` | Safe | Safe | Race | weaver 域压缩/对象身份丢失 |
| `c/weaver/test-context1.wvr.c` | Safe | Safe | Race | weaver 域压缩/对象身份丢失 |
| `c/weaver/test-hard1.wvr.c` | Safe | Safe | Race | weaver 域压缩/对象身份丢失 |
| `c/pthread-C-DAC/pthread-demo-datarace-1.c` | Safe | Safe | Race | ⊤ points-to / 共享计数器归约 |
| `c/pthread-C-DAC/pthread-demo-datarace-3.c` | Safe | Safe | Race | ⊤ points-to / 共享计数器归约 |
| `c/pthread-C-DAC/pthread-numerical-integration.c` | Safe | Safe | Race | ⊤ points-to / 共享计数器归约 |
| `c/pthread/stateful01-1.c` | Safe | Safe | Race | ⊤ points-to / 共享计数器归约 |
| `c/pthread/stateful01-2.c` | Safe | Safe | Race | ⊤ points-to / 共享计数器归约 |
| `c/weaver/bench-exp1x3.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/bench-exp2x3.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/bench-exp2x4.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/bench-exp2x6.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/bench-exp2x9.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/bench-exp3x3-opt.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/bench-exp3x3.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/parallel-parallel-sum-1.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/parallel-simple-equiv.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/popl20-bad-buffer-mult-alt.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/popl20-bad-buffer-mult-alt2.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/popl20-bad-buffer-mult-alt3.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/popl20-bad-commit-1.wvr-bad.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/popl20-bad-commit-2.wvr-bad.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/popl20-bad-counter-queue2.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/popl20-bad-threaded-sum-2.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/popl20-commit-1.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/popl20-commit-2.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/popl20-counter-fun.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/popl20-more-array-sum-alt2.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/popl20-more-array-sum2.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/popl20-more-buffer-mult.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/popl20-more-buffer-mult2.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/popl20-more-buffer-mult3.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/popl20-more-buffer-series.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/popl20-more-buffer-series2.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/popl20-more-buffer-series3.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/popl20-more-max-array-hom.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/popl20-more-max-array.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/popl20-more-min-array-hom.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/popl20-more-min-array.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/popl20-more-min-le-max.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/popl20-more-nonblocking-counter-alt2.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/popl20-mult-4.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/popl20-mult-equiv.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/popl20-nonblocking-cntr-alt.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/popl20-nonblocking-cntr.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/popl20-prod-cons.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/popl20-prod-cons3.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/popl20-proofs-counter-add-4-semi-Q67.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/popl20-send-receive-alt.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/popl20-simple-queue.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/popl20-threaded-sum-3.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/test-easy1.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/test-easy7.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/test-easy8.wvr.c` | Safe | Safe | Race | ⊤ points-to / 对象身份丢失（weaver） |
| `c/libvsync/semaphore.i` | Safe | Safe | Race | 信号量未建模（sem_*） |
| `c/goblint-regression/09-regions_02-list_nr.c` | Safe | Safe | Race | 数组/结构体/堆节点 region 折叠 |
| `c/goblint-regression/09-regions_04-list2_nr.c` | Safe | Safe | Race | 数组/结构体/堆节点 region 折叠 |
| `c/goblint-regression/09-regions_11-arraylist_nr.c` | Safe | Safe | Race | 数组/结构体/堆节点 region 折叠 |
| `c/goblint-regression/09-regions_13-arraycollapse_nr.c` | Safe | Safe | Race | 数组/结构体/堆节点 region 折叠 |
| `c/goblint-regression/09-regions_17-arrayloop_nr.c` | Safe | Safe | Race | 数组/结构体/堆节点 region 折叠 |
| `c/goblint-regression/09-regions_19-nested_nr.c` | Safe | Safe | Race | 数组/结构体/堆节点 region 折叠 |
| `c/goblint-regression/09-regions_21-arrayloop2_nr.c` | Safe | Safe | Race | 数组/结构体/堆节点 region 折叠 |
| `c/goblint-regression/09-regions_22-nocollapse.c` | Safe | Safe | Race | 数组/结构体/堆节点 region 折叠 |
| `c/goblint-regression/09-regions_24-evilcollapse_nr.c` | Safe | Safe | Race | 数组/结构体/堆节点 region 折叠 |
| `c/pthread/twostage_3.c` | Safe | Safe | Race | 数组/结构体/堆节点 region 折叠 |
| `c/libvsync/hclhlock.i` | Safe | Safe | Race | 无锁队列/CAS 协议（ticket/Treiber/elimination/MCS） |
| `c/pthread-complex/safestack_relacy.c` | Safe | Safe | Race | 无锁队列/CAS 协议（ticket/Treiber/elimination/MCS） |
| `c/weaver/chl-collitem-subst.wvr.c` | Safe | Safe | Race | 跨函数 region 传播丢失（weaver chl/unroll） |
| `c/weaver/chl-collitem-symm.wvr.c` | Safe | Safe | Race | 跨函数 region 传播丢失（weaver chl/unroll） |
| `c/weaver/chl-collitem-trans.wvr.c` | Safe | Safe | Race | 跨函数 region 传播丢失（weaver chl/unroll） |
| `c/weaver/chl-exp-term-subst.wvr.c` | Safe | Safe | Race | 跨函数 region 传播丢失（weaver chl/unroll） |
| `c/weaver/chl-exp-term-symm.wvr.c` | Safe | Safe | Race | 跨函数 region 传播丢失（weaver chl/unroll） |
| `c/weaver/chl-file-item-subst.wvr.c` | Safe | Safe | Race | 跨函数 region 传播丢失（weaver chl/unroll） |
| `c/weaver/chl-file-item-symm.wvr.c` | Safe | Safe | Race | 跨函数 region 传播丢失（weaver chl/unroll） |
| `c/weaver/chl-file-item-trans.wvr.c` | Safe | Safe | Race | 跨函数 region 传播丢失（weaver chl/unroll） |
| `c/weaver/chl-match-subst.wvr.c` | Safe | Safe | Race | 跨函数 region 传播丢失（weaver chl/unroll） |
| `c/weaver/chl-match-symm.wvr.c` | Safe | Safe | Race | 跨函数 region 传播丢失（weaver chl/unroll） |
| `c/weaver/chl-match-trans.wvr.c` | Safe | Safe | Race | 跨函数 region 传播丢失（weaver chl/unroll） |
| `c/weaver/chl-poker-hand-subst.wvr.c` | Safe | Safe | Race | 跨函数 region 传播丢失（weaver chl/unroll） |
| `c/weaver/chl-poker-hand-symm.wvr.c` | Safe | Safe | Race | 跨函数 region 传播丢失（weaver chl/unroll） |
| `c/weaver/chl-poker-hand-trans.wvr.c` | Safe | Safe | Race | 跨函数 region 传播丢失（weaver chl/unroll） |
| `c/weaver/chl-simpl-str-subst.wvr.c` | Safe | Safe | Race | 跨函数 region 传播丢失（weaver chl/unroll） |
| `c/weaver/chl-simpl-str-symm.wvr.c` | Safe | Safe | Race | 跨函数 region 传播丢失（weaver chl/unroll） |
| `c/weaver/chl-simpl-str-trans.wvr.c` | Safe | Safe | Race | 跨函数 region 传播丢失（weaver chl/unroll） |
| `c/weaver/chl-sre-subst.wvr.c` | Safe | Safe | Race | 跨函数 region 传播丢失（weaver chl/unroll） |
| `c/weaver/chl-sre-symm.wvr.c` | Safe | Safe | Race | 跨函数 region 传播丢失（weaver chl/unroll） |
| `c/weaver/chl-sre-trans.wvr.c` | Safe | Safe | Race | 跨函数 region 传播丢失（weaver chl/unroll） |
| `c/weaver/chl-time-subst.wvr.c` | Safe | Safe | Race | 跨函数 region 传播丢失（weaver chl/unroll） |
| `c/weaver/chl-time-symm.wvr.c` | Safe | Safe | Race | 跨函数 region 传播丢失（weaver chl/unroll） |
| `c/weaver/chl-time-trans.wvr.c` | Safe | Safe | Race | 跨函数 region 传播丢失（weaver chl/unroll） |
| `c/weaver/parallel-misc-2-unrolled-atomic.wvr.c` | Safe | Safe | Race | 跨函数 region 传播丢失（weaver chl/unroll） |

### 3.2 IKOS 假阳性（Goblint 判 Safe 正确，IKOS 误报 Race）：14 个

| 文件 | 期望 | IKOS | Goblint | 根因初判 |
| :--- | :--- | :--- | :--- | :--- |
| `c/pthread-atomic/read_write_lock-2.c` | Safe | Race | Safe | C11 acquire/release 缺 HB / 无锁互斥算法 |
| `c/weaver/parallel-barrier-loop.wvr.c` | Safe | Race | Safe | ⊤ points-to / 对象身份丢失（weaver） |
| `c/weaver/parallel-barrier.wvr.c` | Safe | Race | Safe | ⊤ points-to / 对象身份丢失（weaver） |
| `c/pthread-ext/43_NetBSD_sysmon_power_sliced-pthread.c` | Safe | Race | Safe | 内核/驱动无锁协议（RCU/中断屏蔽/互斥锁变体） |
| `c/goblint-regression/28-race_reach_90-arrayloop2_racing.c` | Safe | Race | Safe | 数组/结构体/堆节点 region 折叠 |
| `c/goblint-regression/28-race_reach_92-evilcollapse_racing.c` | Safe | Race | Safe | 数组/结构体/堆节点 region 折叠 |
| `c/pthread/queue.c` | Safe | Race | Safe | 数组/结构体/堆节点 region 折叠 |
| `c/pthread/queue_longer.c` | Safe | Race | Safe | 数组/结构体/堆节点 region 折叠 |
| `c/pthread/queue_longest.c` | Safe | Race | Safe | 数组/结构体/堆节点 region 折叠 |
| `c/pthread/queue_ok.c` | Safe | Race | Safe | 数组/结构体/堆节点 region 折叠 |
| `c/pthread/queue_ok_longer.c` | Safe | Race | Safe | 数组/结构体/堆节点 region 折叠 |
| `c/pthread/queue_ok_longest.c` | Safe | Race | Safe | 数组/结构体/堆节点 region 折叠 |
| `c/pthread-ext/04_incdec_cas.c` | Safe | Race | Safe | 无锁队列/CAS 协议（ticket/Treiber/elimination/MCS） |
| `c/pthread-ext/29_conditionals_vs.c` | Safe | Race | Safe | 路径敏感条件锁 |

### 3.3 互相漏报（各 1 个）：2 个

| 文件 | 期望 | IKOS | Goblint | 说明 |
| :--- | :--- | :--- | :--- | :--- |
| `c/pthread-race-challenges/thread-local-value-race.c` | Race | Safe | Race | **IKOS FN**（TLS 指针逃逸，`.i` 上能抓，`.c` 伪影） |
| `c/pthread-race-challenges/thread-join-binomial-race-2.c` | Race | Race | Safe | **Goblint FN**（IKOS 正确抓到） |

### 3.4 IKOS 异常而 Goblint 有结论：8 个

| 文件 | 期望 | IKOS | Goblint | 说明 |
| :--- | :--- | :--- | :--- | :--- |
| `c/pthread-lit/fk2012.c` | Race | Timeout | Race | IKOS 超时 |
| `c/weaver/parallel-lamport.wvr.c` | Safe | Unknown | Safe | IKOS Unknown（weaver ⊤ 类，`.c` 下变 unknown） |
| `c/weaver/parallel-min-max-1.wvr.c` | Safe | Unknown | Safe | IKOS Unknown（weaver ⊤ 类，`.c` 下变 unknown） |
| `c/weaver/popl20-bad-counter-queue.wvr.c` | Safe | Unknown | Safe | IKOS Unknown（weaver ⊤ 类，`.c` 下变 unknown） |
| `c/weaver/popl20-min-max-dec.wvr.c` | Safe | Unknown | Safe | IKOS Unknown（weaver ⊤ 类，`.c` 下变 unknown） |
| `c/weaver/popl20-min-max-inc-dec.wvr.c` | Safe | Unknown | Safe | IKOS Unknown（weaver ⊤ 类，`.c` 下变 unknown） |
| `c/weaver/popl20-min-max-inc.wvr.c` | Safe | Unknown | Safe | IKOS Unknown（weaver ⊤ 类，`.c` 下变 unknown） |
| `c/weaver/popl20-simple-array-sum.wvr.c` | Safe | Unknown | Race | IKOS Unknown（weaver ⊤ 类，`.c` 下变 unknown） |

## 四、Goblint 异常组（ParseError/Timeout/Unknown/OOM）

共 **34** 个：ParseError 29 ／ Timeout 5 ／ Unknown 0 ／ OOM 0

| 文件 | 期望 | IKOS | Goblint | 原因 |
| :--- | :--- | :--- | :--- | :--- |
| `c/goblint-regression/04-mutex_36-trylock_nr.c` | Safe | Error | ParseError | Fatal error: exception Goblint_lib__Maingoblint.FrontendError("Errormsg.Error") |
| `c/pthread-driver-races/char_generic_nvram_nvram_unlocked_ioctl_write_nvram.c` | Safe | Error | ParseError | Fatal error: exception Goblint_lib__Maingoblint.FrontendError("preprocessor terminated with exit code 1: 'cpp' '--std=gn |
| `c/pthread-driver-races/char_generic_nvram_read_nvram_nvram_unlocked_ioctl.c` | Safe | Error | ParseError | Fatal error: exception Goblint_lib__Maingoblint.FrontendError("preprocessor terminated with exit code 1: 'cpp' '--std=gn |
| `c/pthread-driver-races/char_generic_nvram_read_nvram_write_nvram-race.c` | Race | Error | ParseError | Fatal error: exception Goblint_lib__Maingoblint.FrontendError("preprocessor terminated with exit code 1: 'cpp' '--std=gn |
| `c/pthread-driver-races/char_generic_nvram_read_nvram_write_nvram.c` | Safe | Error | ParseError | Fatal error: exception Goblint_lib__Maingoblint.FrontendError("preprocessor terminated with exit code 1: 'cpp' '--std=gn |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_change_pc8736x_gpio_configure.c` | Safe | Error | ParseError | Fatal error: exception Goblint_lib__Maingoblint.FrontendError("preprocessor terminated with exit code 1: 'cpp' '--std=gn |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_change_pc8736x_gpio_current-race.c` | Race | Error | ParseError | Fatal error: exception Goblint_lib__Maingoblint.FrontendError("preprocessor terminated with exit code 1: 'cpp' '--std=gn |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_change_pc8736x_gpio_current.c` | Safe | Error | ParseError | Fatal error: exception Goblint_lib__Maingoblint.FrontendError("preprocessor terminated with exit code 1: 'cpp' '--std=gn |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_change_pc8736x_gpio_get.c` | Safe | Error | ParseError | Fatal error: exception Goblint_lib__Maingoblint.FrontendError("preprocessor terminated with exit code 1: 'cpp' '--std=gn |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_change_pc8736x_gpio_set-race.c` | Race | Error | ParseError | Fatal error: exception Goblint_lib__Maingoblint.FrontendError("preprocessor terminated with exit code 1: 'cpp' '--std=gn |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_change_pc8736x_gpio_set.c` | Safe | Error | ParseError | Fatal error: exception Goblint_lib__Maingoblint.FrontendError("preprocessor terminated with exit code 1: 'cpp' '--std=gn |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_configure_pc8736x_gpio_current.c` | Safe | Error | ParseError | Fatal error: exception Goblint_lib__Maingoblint.FrontendError("preprocessor terminated with exit code 1: 'cpp' '--std=gn |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_configure_pc8736x_gpio_get.c` | Safe | Error | ParseError | Fatal error: exception Goblint_lib__Maingoblint.FrontendError("preprocessor terminated with exit code 1: 'cpp' '--std=gn |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_configure_pc8736x_gpio_set.c` | Safe | Error | ParseError | Fatal error: exception Goblint_lib__Maingoblint.FrontendError("preprocessor terminated with exit code 1: 'cpp' '--std=gn |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_current_pc8736x_gpio_get.c` | Safe | Error | ParseError | Fatal error: exception Goblint_lib__Maingoblint.FrontendError("preprocessor terminated with exit code 1: 'cpp' '--std=gn |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_current_pc8736x_gpio_set-race.c` | Race | Error | ParseError | Fatal error: exception Goblint_lib__Maingoblint.FrontendError("preprocessor terminated with exit code 1: 'cpp' '--std=gn |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_current_pc8736x_gpio_set.c` | Safe | Error | ParseError | Fatal error: exception Goblint_lib__Maingoblint.FrontendError("preprocessor terminated with exit code 1: 'cpp' '--std=gn |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_get_pc8736x_gpio_set.c` | Safe | Error | ParseError | Fatal error: exception Goblint_lib__Maingoblint.FrontendError("preprocessor terminated with exit code 1: 'cpp' '--std=gn |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_open_pc8736x_gpio_change.c` | Safe | Error | ParseError | Fatal error: exception Goblint_lib__Maingoblint.FrontendError("preprocessor terminated with exit code 1: 'cpp' '--std=gn |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_open_pc8736x_gpio_configure.c` | Safe | Error | ParseError | Fatal error: exception Goblint_lib__Maingoblint.FrontendError("preprocessor terminated with exit code 1: 'cpp' '--std=gn |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_open_pc8736x_gpio_current.c` | Safe | Error | ParseError | Fatal error: exception Goblint_lib__Maingoblint.FrontendError("preprocessor terminated with exit code 1: 'cpp' '--std=gn |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_open_pc8736x_gpio_get.c` | Safe | Error | ParseError | Fatal error: exception Goblint_lib__Maingoblint.FrontendError("preprocessor terminated with exit code 1: 'cpp' '--std=gn |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_open_pc8736x_gpio_set.c` | Safe | Error | ParseError | Fatal error: exception Goblint_lib__Maingoblint.FrontendError("preprocessor terminated with exit code 1: 'cpp' '--std=gn |
| `c/pthread-race-challenges/per-thread-array-join-counter-2.c` | Safe | Race | ParseError | Fatal error: exception Cilfacade.TypeOfError(typeOffset: Index on a non-array (0, union __anonunion_pthread_mutex_t_3354 |
| `c/pthread-race-challenges/per-thread-array-join-counter-race-2.c` | Race | Race | ParseError | Fatal error: exception Cilfacade.TypeOfError(typeOffset: Index on a non-array (0, union __anonunion_pthread_mutex_t_3354 |
| `c/pthread-race-challenges/per-thread-array-join-counter-race-3.c` | Race | Race | ParseError | Fatal error: exception Cilfacade.TypeOfError(typeOffset: Index on a non-array (0, union __anonunion_pthread_mutex_t_3354 |
| `c/pthread-race-challenges/per-thread-array-join-counter-race-4.c` | Race | Race | ParseError | Fatal error: exception Cilfacade.TypeOfError(typeOffset: Index on a non-array (0, union __anonunion_pthread_mutex_t_3354 |
| `c/pthread-race-challenges/per-thread-array-join-counter-race.c` | Race | Race | ParseError | Fatal error: exception Cilfacade.TypeOfError(typeOffset: Index on a non-array (0, union __anonunion_pthread_mutex_t_3354 |
| `c/pthread-race-challenges/per-thread-array-join-counter.c` | Safe | Race | ParseError | Fatal error: exception Cilfacade.TypeOfError(typeOffset: Index on a non-array (0, union __anonunion_pthread_mutex_t_3354 |
| `c/goblint-regression/04-mutex_05-lockfuns.c` | Safe | Safe | Timeout | timeout |
| `c/pthread-complex/workstealqueue_mutex-1.c` | Race | Race | Timeout | timeout |
| `c/pthread-complex/workstealqueue_mutex-2.c` | Safe | Race | Timeout | timeout |
| `c/pthread-race-challenges/thread-join-binomial-race-3.c` | Race | Race | Timeout | timeout |
| `c/pthread-race-challenges/thread-join-binomial-race.c` | Race | Race | Timeout | timeout |

> 注：其中 23 个 ParseError 是**两边都失败**的 `.c` 伪影（见下节），非 Goblint 独有。真正 Goblint 独有异常：
> 6 个 `per-thread-array-join-counter*`（CIL `TypeOfError`，IKOS 正常给出 Race/Safe）+ 5 个 Timeout。

## 五、双方都失败（`.c` 工具链伪影，需 `.i` 预处理）

共 23 个：IKOS=Error 且 Goblint=ParseError。根因一致——这些 `.c` 依赖预处理展开的宏/内核头，
直接编译失败。典型：`char_generic_nvram*`/`char_pc8736x_gpio*`（Linux 驱动头）、`04-mutex_36-trylock_nr`（`INT_MAX` 来自 `<limits.h>`）。

| 文件 | 期望 | IKOS | Goblint |
| :--- | :--- | :--- | :--- |
| `c/goblint-regression/04-mutex_36-trylock_nr.c` | Safe | Error | ParseError |
| `c/pthread-driver-races/char_generic_nvram_nvram_unlocked_ioctl_write_nvram.c` | Safe | Error | ParseError |
| `c/pthread-driver-races/char_generic_nvram_read_nvram_nvram_unlocked_ioctl.c` | Safe | Error | ParseError |
| `c/pthread-driver-races/char_generic_nvram_read_nvram_write_nvram-race.c` | Race | Error | ParseError |
| `c/pthread-driver-races/char_generic_nvram_read_nvram_write_nvram.c` | Safe | Error | ParseError |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_change_pc8736x_gpio_configure.c` | Safe | Error | ParseError |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_change_pc8736x_gpio_current-race.c` | Race | Error | ParseError |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_change_pc8736x_gpio_current.c` | Safe | Error | ParseError |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_change_pc8736x_gpio_get.c` | Safe | Error | ParseError |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_change_pc8736x_gpio_set-race.c` | Race | Error | ParseError |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_change_pc8736x_gpio_set.c` | Safe | Error | ParseError |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_configure_pc8736x_gpio_current.c` | Safe | Error | ParseError |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_configure_pc8736x_gpio_get.c` | Safe | Error | ParseError |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_configure_pc8736x_gpio_set.c` | Safe | Error | ParseError |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_current_pc8736x_gpio_get.c` | Safe | Error | ParseError |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_current_pc8736x_gpio_set-race.c` | Race | Error | ParseError |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_current_pc8736x_gpio_set.c` | Safe | Error | ParseError |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_get_pc8736x_gpio_set.c` | Safe | Error | ParseError |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_open_pc8736x_gpio_change.c` | Safe | Error | ParseError |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_open_pc8736x_gpio_configure.c` | Safe | Error | ParseError |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_open_pc8736x_gpio_current.c` | Safe | Error | ParseError |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_open_pc8736x_gpio_get.c` | Safe | Error | ParseError |
| `c/pthread-driver-races/char_pc8736x_gpio_pc8736x_gpio_open_pc8736x_gpio_set.c` | Safe | Error | ParseError |

## 六、下一步参考

1. **Goblint 优势区**（3.2 的 14 个 IKOS FP）：`queue*`（6 个）、`28-race_reach_90/92`、`read_write_lock-2`、
   `04_incdec_cas`、`29_conditionals_vs`、`43_NetBSD`、`parallel-barrier*`（2 个）。Goblint 用**分区数组域 + region 分析**
   证明 queue/list 安全，这是 IKOS 的「数组/堆节点 region 折叠」类 FP（对应 FP 分类文档 C 档 ~38 个）。
2. **IKOS 优势区**（3.1 的 101 个 Goblint FP）：主要靠锁集/happens-before/TLS 建模 + 更精确的 points-to。
   其中 Goblint FP 的文件若与 IKOS FP 分类文档重叠，可优先复用既有根因。
3. **信号量/无锁/CAS** 类（`semaphore`、`04_incdec_cas`、`04/08_cas`、`libvsync`）两边都无解或都误报，
   属「未建模原语」难类，无 cheap sound 方案（见 FP 分类文档）。
4. `.i` 口径复跑：若要完全对齐 SV-COMP 官方口径（消除第五节 23 个伪影 + 1 个 FN），下一步应把 IKOS 基线
   （`svcomp_race_test.py` 的 `.i` 枚举）与 Goblint 的 `.i` 跑法（`--enable ana.sv-comp.functions`）对齐后重跑。

---

*原始数据：`docs/ikos-vs-goblint-race-comparison.csv`（file, expected, IKOS, Goblint, goblint_reason）。*
