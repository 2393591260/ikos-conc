# IKOS Race FP 分类清单（更新版 · commit 342db36）

> 本文档为 2026-09-23 会话的 FP 攻坚完整记录：从基线 227 一路到当前 182，含每步修复、
> 修正后的根因分类、以及已证实「不可 sound 解决」的边界。

## 一、基线演进

| 提交 | 改动 | FP | 性质 |
|---|---|---|---|
| 0ec2137（基线） | 阶段3开 / 阶段2降级关 | 227 | — |
| 27caa90 | 栈数组句柄 load phantom（`resolve_points_to` 的 PointerShift-on-栈 pin） | 194（**-33**） | 精度修复 |
| 0f2ced8 | cond-var 语义建模（wait-queue + signal epoch + MAY digest） | 194（0） | 纯 sound |
| 2da2c0a | TLS 识别（`is_thread_local`）+ 编译器字符串常量排除 | 182（**-12**） | 精度修复 |
| 342db36 | sem_* 排除（信号量对象合成写） | 182（0） | sound 正确性 |

**当前**：`--analyses=race --concurrency=auto`，1030 个 no-data-race 任务：

| 指标 | 值 |
|---|---|
| TP | **235** |
| FP | **182** |
| TN | 604 |
| FN | **0**（红线） |
| err / unknown | 9 / 0 |
| Precision / Recall | 0.564 / 1.000 |

---

## 二、当前 182 FP 分类（按根因三档）

### A. 完全未建模的原语（真·缺语义）— ~1

| 缺失原语 | 规模 | 代表文件 | 说明 |
|---|---|---|---|
| **信号量** `sem_wait/sem_post/sem_init` | 1 | `pthread-race-challenges/semaphore-posix` | 引擎零处理；FP 是 `data=nondet()` 无保护的自竞态。**二值建模已被证 unsound**（`semaphore-posix-race` 反例：二值 + 裸 `sem_post` 会漏报），sound 建模需 count 跟踪，代价不成比例 → **放弃** |

> **barrier 是分类误判**：全局搜索确认 sv-benchmarks 中**无任何文件真正调用 `pthread_barrier_wait`**（`.i` 里的匹配全是 glibc 头文件声明噪声）。`value-barrier`/`40_barrier_vf` 是 cond-var 手写 barrier（归 B 档 cond-var），`elimination_backoff_stack` 是 `__VERIFIER_atomic` 无锁（归 B 档无锁），`sssc12` 是纯 mutex。**故「barrier 建模」无对象，撤销。**

### B. 半建模（有桩，缺 happens-before / 协议）— ~45

| 缺的语义 | 规模 | 代表文件 | 根因 |
|---|---|---|---|
| **cond-var signal→wait HB** | ~20 | `thread-join-counter-*`、`pthread-complex/bounded_buffer`、`pthread/queue` | signal→wait 是 **MAY 边**（伪唤醒 + 多 waiter）。已建 wait-queue/digest，但按 FN=0 红线**刻意不用于竞争短路** |
| **C11 acquire/release** | ~10 | `pthread-atomic/{dekker,lamport,peterson,szymanski,read_write_lock,time_var_mutex}`、`pthread-theta/unwind2-*`（`atomic_flag` 自旋锁） | 只豁免 atomic-atomic 对，不建 release→acquire HB |
| **无锁队列/CAS 协议** | ~17 | `libvsync/*`(10)、`elimination_backoff_stack`、`workstealqueue_mutex-2`、`safestack_relacy`、`pthread-ext/26_stack_cas*`、`48_ticket_lock`、`04/08_cas` | ticket/MCS/CLH 队列锁、Treiber 栈——互斥是「取唯一号/前驱节点」的协议性质 |

### C. 精度 / 前端问题（**非缺语义**）— ~126

| 问题 | 规模 | 代表文件 | 根因 |
|---|---|---|---|
| **⊤ points-to / 对象身份丢失** | ~34 | `weaver/popl20-*`(25)、`weaver/parallel-*`(6)、`weaver/{array-eq,loop-tiling}`(3) | 域压缩下对象身份丢；phase-2 已证不可降级 unknown（会丢 TP） |
| **数组/结构体/堆节点 region 折叠** | ~38 | `goblint-regression` 的 list/array(16)、`pthread/{queue,bigshot,singleton,twostage,sync01}`(12)、`ldv-races/race-2/3-container_of`(7)、`pthread-deagle/arithmetic_prog`(2)、`pthread-C-DAC/finding-k-matches`(1) | 内存域折叠成单 region，offset/instance 哈希只补常量偏移 |
| **跨函数 region 传播丢失** | ~27 | `weaver/chl-*`(19)、`weaver/unroll-*`(8) | region/points-to 跨 inlining 边界丢 |
| **spawn 实参单射性（关系推理）** | ~11 | `per-thread-array/index/struct-*` | 线程函数单次分析 + spawn 实参是循环变量，**区间域表达不了「各线程写各自槽」** |
| **TLS 指针传播**（`__thread int*`） | ~1-3 | `thread-local-value-dynamic` | `__thread` 标量已修；`__thread 指针 → 线程私有堆` 未修 |
| **内核/驱动无锁协议** | ~16 | `pthread-driver-races/*`(8)、`ldv-linux-3.14-races/*`(6)、`pthread-ext/43_NetBSD_sysmon`(2) | `mutex_lock_interruptible`/RCU/中断屏蔽 |

> 注：C 档各子类有重叠（如 pthread-race-challenges 的 per-thread-array 文件同时含「spawn 单射性」与「堆数组句柄 phantom」两个根因）。

---

## 三、本 session 关键发现（修正了旧分类的误判）

1. **旧「2.1 锁指针别名（59）」是系统性误判** → 实为「main 的 `pthread_join(&t_ids[i])` 句柄 load 变 TOP，⊤ 作为 meet 单位元与所有 @global 写配对」。goblint 的 44 个 `28-race_reach_*` 本质是**同一个 bug**（与锁无关），文件名里的 cond/trylock/funptr 全是干扰项。已修（-33）。
2. **堆数组句柄 phantom 是 2.1 的堆变体**：pthread-race-challenges 用 `safe_malloc`（堆）建 tids，`pthread_join(tids[i])` 的句柄 load points-to 变 TOP。**根因是 sv-benchmarks 工具链特定的 points-to 丢失，本地 1:1 复刻无法复现**（5 组对照实验排除 malloc/free 内建、abort noreturn、assume 约束、clang -E、`__alloc_size__`），属 **weaver ⊤ 类底层架构问题**。
3. **信号量 FP 是「双根因」，且二值建模 unsound**：① 信号量对象被 `check_extern_call_effects` 合成写（已修 sem_* 排除）、③ `data=nondet()` 无保护自竞态。**「二值 flag」（`sem_init` 计数≤1 → `sem_wait`=加锁）被证 unsound**：`semaphore-posix-race`（计数=1 但 main 裸 `sem_post` 使 count 顶到 2）会漏报真竞态。根因是 `sem_post` 不对称于 `sem_wait`（任意线程可无前置 wait 就 post）。sound 需 count 跟踪，代价不成比例。
4. **「barrier ~10 文件」是分类误判**：全局搜索无任何文件真实调用 `pthread_barrier_wait`（`.i` 匹配全是 glibc 声明噪声）；`value-barrier` 是 cond-var 手写 barrier、`elimination_backoff_stack` 是 `__VERIFIER_atomic` 无锁、`sssc12` 是纯 mutex。
5. **TLS 有两种机制**：`__thread` 标量（已修：AR 加 `is_thread_local` 标志 + checker 过滤）+ `__thread int*` 指针传播（未修）。
6. **`__PRETTY_FUNCTION__` 编译器字符串常量**是跨文件 FP 源，排除后连带清掉 9 个文件（`unwind2-*`、`40_barrier_vf`、`safestack_relacy` 等——它们的 FP 其实是 `reach_error` 字符串竞态）。
7. **cond-var 的 sound 建模（wait-queue/digest）不消除任何 FP**（符合设计预期）：signal→wait 的 MAY 边不能短路，唯一 sound 保护是「wait 重获 mutex」的锁纪律。

---

## 四、下一步建议（sound + 杠杆排序）

> 截至本 session 收尾，**checker 层的低成本 sound 精度补丁已全部做完或证伪**。剩余 FP 均需「内存域级重构」或「算法级识别」，属下一工程阶段，不再在本 session 硬啃。

| 方向 | 规模 | 性质 |
|---|---|---|
| **field-sensitive region**（内存域成员/节点敏感） | ~110 | 唯一有真实杠杆，但是**内存域重构**（cell 抽象拆分），第一刀（语法化 region）已实测净倒退并回退 |
| **无锁互斥算法识别**（dekker/peterson/libvsync） | ~17 | 需完整算法识别 |

**明确不碰 / 已证不可 cheap sound 解决**：

- **acquire/release HB**（无对象：sv-benchmarks 无 `memory_order_release` 消息传递；pthread-atomic 的 dekker/peterson 是互斥算法，属「无锁互斥」难类，需完整算法识别）
- **二值信号量建模**（unsound：`sem_post` 不对称，裸 post 会顶破 count，`semaphore-posix-race` 是反例）
- **barrier 建模**（无对象：sv-benchmarks 无真实 `pthread_barrier_wait` 调用）
- 堆数组句柄 phantom（底层工具链 points-to 丢失，本地无法复现闭环）
- cond-var signal-only 排序 FP（MAY 边，sound 建模无法消除）
- spawn 实参单射性（区间域表达不了关系，需关系域或专用模式识别）
- ⊤ points-to 降级 unknown（phase-2 已证会丢 TP）
- 数组折叠的「符号锁保留」问题（第一刀实测发现：符号锁在 CFG join/widening 被丢出 must-锁集，与 region 派生解耦，独立专项）

---

## 五、field-sensitive region 第一刀（数组折叠）— **已尝试并回退**

### 5.1 子类划分（7 个文件实测）

field-sensitive region 是**四个不同子类**的伞，不是单一特性：

| 子类 | 代表 | 根因 | 状态 |
|---|---|---|---|
| ① 数组元素折叠 | `28-race_reach_90-arrayloop2`、`92-evilcollapse` | `c.slot[j]` 折叠成 `@c` | 实测其竞态含「空锁集」访问，**非** region 派生问题 |
| ② 堆节点折叠 | `28-race_reach_81-list` | 链表节点无 per-node 身份 | 不碰 |
| ③ container_of offset ⊤ | `ldv-races/race-2_1` | 偏移减法失去精度 | 不碰 |
| ④ ⊤ points-to | `weaver/popl20-*` | 全局指针值丢失 | 不碰 |

### 5.2 第一刀（region_of → symbolic_index_of）实测结论：**净倒退，已回退**

按方案把数据侧 `region_of` 从 `region_collect`（追跨槽 heap 指针 + 域内 formal 绑定，被 `region_tainting_sound` 门禁）换成语法化 `symbolic_index_of`，并移除门禁。全量基准实测：

- **FP 182 → 192（+10 回归）**，TP=235 不变、FN=0。
- **获益 0 个**：目标文件 `arrayloop2`/`evilcollapse` 的竞态**未消除**——深挖发现它们的 `@c` 竞态涉及**空锁集的访问**（符号锁在 CFG join/widening 被丢出 must-锁集），`self_locked` 无法短路；真正的死因是**锁准入门/锁集保留**，不是 region 派生。
- **回归 10 个**：`06-symbeq_*_nr`、`09-regions_*_nr`（arraylist_nr、arraycollapse_nr、nested_nr、arrayloop2_nr、nocollapse、evilcollapse_nr 等）——这些原本依赖 `region_collect` 的**域内 region 绑定**（语法不可见），换语法化后 region 丢失 → 变回 FP。

**结论**：`region_collect` 的域内 region 恢复其实在**消 FP**（值 10 个），不是「过度激进需要门禁」；语法化 `symbolic_index_of` 拿不回这些 region。第一刀方向错误，**已 revert**。数组折叠类真正的死因是「符号锁从 must-锁集丢失」（锁准入门/锁集保留问题），与 region 派生解耦。

### 5.3 后续（明确不碰，本 session 不再硬啃）

- ② 堆节点 per-node region（需内存域/shape 分析）
- ③ container_of offset 精度
- ④ weaver ⊤ points-to（phase-2 已证 demotion unsound）
- 数组折叠的「符号锁保留」修复（需先搞清 must-锁集为何丢符号锁，独立专项）
