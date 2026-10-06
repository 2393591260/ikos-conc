# IKOS 跑 Goblint 竞态回归测试结果

> 生成于 2026-09-24。用 IKOS（`--analyses=race --concurrency=auto`）跑 goblint 自己的竞态回归测试集，
> 期望判定取自 goblint 测试的 `// RACE!`/`// NORACE` 注释 + `_rc`/`_nr` 文件名后缀。
> 只观测结果，未改任何代码。

> **后续修复（commit 2346b75）**：本报告的 25 个 FN 里，「未初始化互斥锁指针」类（2 个）已修复——
> clang 把 `pthread_mutex_t *m` 折叠成 `pthread_mutex_lock(undef)`，`exec_intrinsic_call` 的通用 uninit 检查
> 把 `undef` 打成 BOTTOM → 整条路径死 → 0 个竞态检查 → 静默 SAFE。修复 = 把 pthread lock/unlock 提前到该检查之前派发。
> 结果：`04-mutex_31-uninitialized`、`04-mutex_63-unknown_unlock` 翻转为 Race，**FN 25→23、TP 109→111、Recall 0.813→0.828、FP 19→19（零回归）**；sv-benchmarks 官方基线不变（TP=235 FP=182 FN=0）。

> **后续修复（freeze-undef）**：通用 `undef`（未初始化标量被 mem2reg 折叠）→ BOTTOM 是 under-approximation，
> 根治 = ikos-pp 加 `--freeze-undef` pass，在 mem2reg 前把 i32/i64 alloca 冻成 `__ikos_nondet_*`（单一 nondet 值），
> race 分析时由 `ikos` wrapper 自动开启。结果：`52-confid`、`12-equ_proc`、`33-alloc_region`、`05-assume-unknown`
> 共 **4 个 FN 翻转**，**FN 23→19、TP 111→115、Recall 0.828→0.858**；**FP 19→22（+3 回归）**——全是 `10-synch`
> 的 `19/21/22`，根因是「create iff join」需要**路径敏感（goblint `threadflag`）**，IKOS 尚无，属已知待办；
> sv-benchmarks FN=0 保持、但 `nsc-ircc` 因额外 nondet 开销从 FP 变超时（err 9→10，性能非正确性）。

> **后续修复（thread-instance identity + join HB，commit 4daa7d5）**：把线程身份从「函数名」升级为「create 调用点的稳定 site id」，
> 并让 `pthread_join` 读回 create 写进 pthread_t 的实例值；join 边跳过条件改为「已 join **b 的全部实例**」+ 传递闭包（`_join_summary`）。
> 同时 `exec_unknown_call` 的 `may_write_params` 补上「取址局部（LocalVariable）指针实参」的 forget（`foo(&id)` 会改写 handle）。
> 结果：`10-synch_28-join-array`（只 join tids[0]，原把 tids[1] 也当 joined → FN）、`51-threadjoins_07-trivial-unknowntid`
> （未知 tid join 原兜底「join all」造假 HB 边 → FN）两例翻转为 Race，**FN 19→17、TP 115→117、FP 22→22（零回归）**。
> 仍剩 3 个 join 相关 FN 未修：`10-synch_29/30`（goblint `unknown(f)`「spawn 未知次数」内建，非 pthread，需单独建模）、
> `51-threadjoins_08-klever-multiple`（klever 多线程 join）。

## 一、测试集与范围

goblint 测试集位于 `goblint-analyzer/tests/regression/`（~80 个组，覆盖所有分析）。竞态相关组共 9 个，
本报告跑其中 **7 个**（plain pthread + `RACE!`/`NORACE` 约定）：

| 组 | 含义 | 文件数 |
| :--- | :--- | ---: |
| 04-mutex | 互斥锁竞态 | 99 |
| 05-lval_ls | lval 锁集 | 25 |
| 06-symbeq | 符号锁等价 | 51 |
| 09-regions | region 竞态 | 41 |
| 10-synch | 线程同步/join | 27 |
| 51-threadjoins | 线程 join | 11 |
| 53-races-mhp | races + mhp | 8 |

**排除 2 个组**：
- `28-race_reach`（54）：用 `racemacros.h` 的 `access`/`assert_racefree` 宏把「逻辑竞态」藏在 `__global_lock` 后面，
  IKOS 只看到 C 级锁保护 → 全部判 Safe，与 goblint 的语义不兼容。
- `71-doublelocking`（20）：double-lock/unlock 错误检测，非数据竞态。

## 二、总成绩

- 测试文件 **262** 个，期望明确 **233** 个（另有 29 个无明确期望，跳过）
- **TP=109 FP=19 TN=66 FN=25**，未评分（编译失败/超时/Unknown）=14
- **Precision = 0.852，Recall = 0.813**

> 对比：IKOS 在 sv-benchmarks no-data-race 上 recall=0.996（FN=1），但在 goblint 自己的测试上 recall 掉到 **0.813（FN=25）**。
> goblint 的测试刻意覆盖了锁失败/部分 join/未知锁指针/符号锁/region 分配等**边角**，这些是 IKOS 当前漏报的。

## 三、按组明细

| 组 | 总 | TP | FP | TN | FN | 未评分 | 期望不明 |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 04-mutex | 99 | 41 | 5 | 26 | 12 | 9 | 6 |
| 05-lval_ls | 25 | 14 | 1 | 5 | 4 | 0 | 1 |
| 06-symbeq | 51 | 19 | 7 | 10 | 2 | 0 | 13 |
| 09-regions | 41 | 19 | 0 | 14 | 1 | 5 | 2 |
| 10-synch | 27 | 6 | 4 | 7 | 3 | 0 | 7 |
| 51-threadjoins | 11 | 5 | 1 | 2 | 3 | 0 | 0 |
| 53-races-mhp | 8 | 5 | 1 | 2 | 0 | 0 | 0 |

## 四、漏报（FN）——重点：IKOS 漏掉的竞态

共 **25** 个真漏报（期望 Race，IKOS 判 Safe）+ **7** 个编译失败（IKOS=Error，归未评分）。

> **关键机制：25 个 FN 几乎全部是「0 个竞态检查」**——IKOS 根本没生成任何访问对就静默返回 SAFE。
> 这与 sv-benchmarks 上的 recall=0.996 形成反差：goblint 的测试用未初始化指针/extern 调用/部分 join
> 把 points-to 或线程关系打成 TOP/未知，IKOS 的竞态检查器在这些状态下退化成「不检查」。

### 4.1 根因分类汇总

| 根因 | 数量 |
| :--- | ---: |
| pthread_join 返回指针语义 → 漏报 | 3 |
| 字段分布缺失 → 漏报 | 2 |
| sem.lock.fail：IKOS 假设 pthread_mutex_lock 恒成功，goblint 建模失败分支 → 漏报 | 1 |
| extern foo(funptr) 可能 spawn 函数指针线程，IKOS 不建模 unknown_function.spawn → 漏报 | 1 |
| 函数指针/结构体间接调用 → 漏报 | 1 |
| 未初始化互斥锁指针 → points-to 退化 → **0 检查**静默漏报（soundness 隐患） | 1 |
| 未初始化 uk + extern rantom() → points-to 退化 TOP → **0 检查**静默漏报 | 1 |
| extern 通过函数指针调用 → 漏报 | 1 |
| unlock(未知互斥锁) → **0 检查**静默漏报 | 1 |
| TLS 逃逸（__thread 指针逃逸到全局）→ 漏报 | 1 |
| 字段分布（struct 字段敏感）缺失 → 漏报 | 1 |
| 非线程安全函数（extern）→ 漏报 | 1 |
| pthread_join 第二参数（返回指针）语义 + __goblint_assume → 漏报 | 1 |
| 符号锁等价（symb_locks/var_eq）→ 漏报 | 1 |
| 符号锁（循环索引锁）→ 漏报 | 1 |
| 堆 region 分配（next 指针翻转）→ 漏报 | 1 |
| 只 join 部分线程（tids[0] 而非 tids[1]）→ 漏报 | 1 |
| 多线程创建、只 join 部分 → 漏报 | 1 |
| thread 分析边角 → 漏报 | 1 |
| threadJoins + 未知线程 ID → 漏报 | 1 |
| 未知线程 ID join → 漏报 | 1 |
| 多线程 join（klever）→ 漏报 | 1 |

### 4.2 漏报清单（FN）

| 文件 | 根因 |
| :--- | :--- |
| `04-mutex/13-failed_locking.c` | sem.lock.fail：IKOS 假设 pthread_mutex_lock 恒成功，goblint 建模失败分支 → 漏报 |
| `04-mutex/26-ptrrace_default.c` | extern foo(funptr) 可能 spawn 函数指针线程，IKOS 不建模 unknown_function.spawn → 漏报 |
| `04-mutex/29-funstruct_rc.c` | 函数指针/结构体间接调用 → 漏报 |
| `04-mutex/31-uninitialized.c` | 未初始化互斥锁指针 → points-to 退化 → **0 检查**静默漏报（soundness 隐患） |
| `04-mutex/52-confid_rc.c` | 未初始化 uk + extern rantom() → points-to 退化 TOP → **0 检查**静默漏报 |
| `04-mutex/56-extern_call_by_ptr_rc.c` | extern 通过函数指针调用 → 漏报 |
| `04-mutex/63-unknown_unlock_rc.c` | unlock(未知互斥锁) → **0 检查**静默漏报 |
| `04-mutex/83-thread-local-storage-escape.c` | TLS 逃逸（__thread 指针逃逸到全局）→ 漏报 |
| `04-mutex/90-distribute-fields-type-1.c` | 字段分布（struct 字段敏感）缺失 → 漏报 |
| `04-mutex/91-distribute-fields-type-2.c` | 字段分布缺失 → 漏报 |
| `04-mutex/92-distribute-fields-type-deep.c` | 字段分布缺失 → 漏报 |
| `04-mutex/94-thread-unsafe_fun_rc.c` | 非线程安全函数（extern）→ 漏报 |
| `05-lval_ls/20-race-null-void.c` | pthread_join 第二参数（返回指针）语义 + __goblint_assume → 漏报 |
| `05-lval_ls/21-race-null-type.c` | pthread_join 返回指针语义 → 漏报 |
| `05-lval_ls/22-race-null-void-deep.c` | pthread_join 返回指针语义 → 漏报 |
| `05-lval_ls/23-race-null-type-deep.c` | pthread_join 返回指针语义 → 漏报 |
| `06-symbeq/12-equ_proc_rc.c` | 符号锁等价（symb_locks/var_eq）→ 漏报 |
| `06-symbeq/39-funloop_index_bad.c` | 符号锁（循环索引锁）→ 漏报 |
| `09-regions/33-alloc_region_rc_next_flip.c` | 堆 region 分配（next 指针翻转）→ 漏报 |
| `10-synch/28-join-array.c` | 只 join 部分线程（tids[0] 而非 tids[1]）→ 漏报 |
| `10-synch/29-multiple-created-only.c` | 多线程创建、只 join 部分 → 漏报 |
| `10-synch/30-no-crash.c` | thread 分析边角 → 漏报 |
| `51-threadjoins/05-assume-unknown.c` | threadJoins + 未知线程 ID → 漏报 |
| `51-threadjoins/07-trivial-unknowntid.c` | 未知线程 ID join → 漏报 |
| `51-threadjoins/08-klever-multiple.c` | 多线程 join（klever）→ 漏报 |

### 4.3 编译失败（IKOS=Error，未评分）

| 文件 | 原因 |
| :--- | :--- |
| `04-mutex/32-allfuns.c` | allfuns（分析所有函数）→ IKOS 编译失败 |
| `04-mutex/33-kernel_rc.c` | kernel 配置 → IKOS 编译失败 |
| `04-mutex/40-rw_lock_rc.c` | kernel 配置 → IKOS 编译失败 |
| `04-mutex/61-allfuns-globs.c` | allfuns → IKOS 编译失败 |
| `04-mutex/72-memset_arg_rc.c` | memset 参数 → IKOS 编译失败 |
| `09-regions/07-kernel_list_rc.c` | kernel 配置 → IKOS 编译失败 |
| `09-regions/14-kernel_foreach_rc.c` | kernel 配置 → IKOS 编译失败 |

## 五、误报（FP）——IKOS 多报的竞态

共 **19** 个（期望 Safe，IKOS 判 Race）。按根因：

| 文件 | 根因 |
| :--- | :--- |
| `04-mutex/58-pthread-lock-return.c` | pthread_mutex_lock 返回值（IKOS 建模 lock 失败）→ 误报 |
| `04-mutex/66-free_direct_nc.c` | free 语义（ana.race.free）→ 误报 |
| `04-mutex/67-free_indirect_nr.c` | free 语义 → 误报 |
| `04-mutex/78-type-array.c` | 类型数组 → 误报 |
| `04-mutex/99-volatile.c` | volatile（goblint ana.race.volatile 排除，IKOS 未排除）→ 误报 |
| `05-lval_ls/19-idxunknown_unlock_precise.c` | 未知索引 unlock → 误报 |
| `06-symbeq/17-type_nr.c` | 符号锁（symb_locks/var_eq）→ 误报 |
| `06-symbeq/20-mult_accs_nr.c` | 符号锁 → 误报 |
| `06-symbeq/26-symb_lockfuns.c` | 符号锁（lock 函数指针）→ 误报 |
| `06-symbeq/27-symb_no_lockfuns.c` | 符号锁 → 误报 |
| `06-symbeq/33-symb_accfun.c` | 符号锁 → 误报 |
| `06-symbeq/38-chrony-name2ipaddress.c` | 符号锁 + malloc wrapper → 误报 |
| `06-symbeq/43-type_nr_disjoint_types.c` | 符号锁（不相交类型）→ 误报 |
| `10-synch/13-two_threads_nr.c` | join/mhp 边角 → 误报 |
| `10-synch/15-join_other_nr.c` | join 其它线程 → 误报 |
| `10-synch/16-join_loop_nr.c` | 循环 join → 误报 |
| `10-synch/20-race-2_1-container_of.c` | container_of 偏移 → 误报 |
| `51-threadjoins/09-join-main.c` | join main → 误报 |
| `53-races-mhp/04-not-created2.c` | mhp（未创建线程）→ 误报 |

## 六、结论与下一步

1. **FN=25 是本次最有价值的发现**：goblint 自己的测试暴露了 IKOS 在 sv-benchmarks 上测不出来的漏报面。
   最值得优先看的 soundness 隐患（三类，均为「0 检查」静默漏报）：
   - **未知/未初始化指针 → points-to 退化 TOP → 0 检查**（`31-uninitialized`、`63-unknown_unlock`、`52-confid`）——
     违反 FN=0 红线，若 sv-benchmarks 出现类似构造就会静默漏报。
   - **部分 join / 线程数组**（`28-join-array` 等 6 个）：main 只 join 部分线程时，未 join 线程的访问被错误忽略。
   - **extern 可能 spawn 函数指针**（`26-ptrrace_default` 等 4 个）：IKOS 不建模 `unknown_function.spawn`。
2. **FP=19 与既有 FP 分类文档高度重合**：符号锁（06-symbeq 7 个）、container_of、volatile、free。
   其中 `volatile` 一项（`99-volatile.c`）很轻——goblint 用 `ana.race.volatile` 排除 volatile 变量，IKOS 没排除。
3. **符号锁是 FP 与 FN 的双向来源**：`06-symbeq` 组 IKOS FP=7 且 FN=2，说明 IKOS 对符号索引锁（`a[i]` 型锁数组）
   既多报又漏报，是锁准入门/锁数组建模的系统性缺口（对应 FP 分类文档 C 档「锁数组」）。
4. 7 个编译失败全部是 goblint 的 `kernel`/`allfuns` 特殊配置（Linux 内核驱动构造），需 `.i` 预处理才能喂给 IKOS。

---

*原始数据：`docs/ikos-on-goblint-tests.csv`（group, file, expected, IKOS）。*
