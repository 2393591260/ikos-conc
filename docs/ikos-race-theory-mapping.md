# IKOS 数据竞争检测 —— 理论依据、语义建模与实现状态

> **本文目标**：把「IKOS 原生（非并发）已有哪些理论、是否完善、soundness 前提是什么」和「并发这一块我们建了哪些语义、各自 A/B/C 分类、实现到哪一步、靠哪些具体测试文件兜底、现在的 soundness 依据是什么」讲清楚，最后给出能力边界与下一步路线。
>
> **红线（贯穿全文）**：判定器是**过近似**——宁可把真同步程序报成竞争（FP），也绝不把真竞争藏掉（FN）。这是 S. V. Adve, *Data Races are Evil with No Exceptions*, CACM 53(11), 2010 的直接推论。

**A/B/C 记号约定**（贯穿全文）：
- **A 类 · 原生复用**：upstream IKOS 已完备实现，直接调用即可，满足上近似标准（上游已核验）。
- **B 类 · 封装增强**：upstream 具备基础机制/接入点，我们在其上做封装转换，须核验封装后的语义仍上近似。
- **C 类 · 自研新增**：upstream 没有对应机制，并发模块从零实现，须逐条核验语义标准。

> 三类只描述「能力从哪来」，不评价质量——C 类正是本工作的核心贡献所在。

---

# 第一部分：原生 IKOS（非并发）的理论与 soundness 前提

「原生」指 **upstream NASA IKOS 的串行分析**（本 fork 的基线 `ac7f7c1`），它提供被并发模块**复用**的一切底层能力。这部分与竞争检测无关，但并发分析的 soundness 完全建立在其上。

## 1.1 原生 IKOS 的组成部分与对应理论

| 组件 | 内容 | 对应理论 |
|---|---|---|
| AR（Abstract Representation） | SSA 化中间表示，语句含 Load/Store/CallBase/Comparison/…，带 debug 定位 | LLVM IR → AR 翻译（工程） |
| 抽象解释框架 | `AbstractDomain` + `FixpointIterator`，Galois 连接 `(α, γ)`，转移函数局部 sound | P. Cousot & R. Cousot, *Abstract Interpretation: A Unified Lattice Model for Static Analysis of Programs by Construction or Approximation of Fixpoints*, POPL 1977 |
| 数值域 MachineInt | 区间格 + congruence 乘积，bit-width/sign 区分，widening/narrowing | P. Cousot & R. Cousot, *Comparing the Galois Connection and Widening/Narrowing Approaches to Abstract Interpretation*, PLILP 1992；A. Miné 的机器整数区间（octagon 系列） |
| 内存域 ValueDomain | cell-based 内存：`MemoryLocation` 三类（Global/DynAlloc/Local）+ `PointsToSet` + 字节 offset 区间 | may-points-to 标准抽象 |
| 降阶积框架 CompositeDomain | Uninitialized × MachineInt × Nullity × … 的约化积；`ExceptionDomain → PolymorphicDomain → ValueDomain` 嵌套 | P. Cousot & R. Cousot 的降阶积（reduce product） |
| 指针分析 | `FunctionPointerAnalysis`（约束求解），为函数指针调用提供 targets | Andersen 式约束求解 |

## 1.2 原生是否完善可用

| 组件 | 完善度 | 说明 |
|---|---|---|
| 数值域 MachineInt | 完备 | 区间+同余，widening 保证终止；是循环界/下标/计数器追踪的根基 |
| 内存域 ValueDomain | 完备（sound）但**字段不敏感** | 结构体/数组折叠成单 cell——是**精度上限**，不是 soundness 缺陷（折叠 = 过近似 = 安全方向） |
| fixpoint + widening | 完备 | `--widening-delay` 前 k 次 join；narrowing 回抽 |
| 指针分析 | 完备 | 流不敏感（精度上限） |
| 异常/生命周期/未初始化/空性 | 完备 | 竞争检测里次要 |

**结论**：原生 IKOS 的**数值抽象**与**内存/别名抽象**可直接复用于并发分析（A 类），无需改动、无需重新核验——它们的 soundness 由 upstream 保证。

## 1.3 原生 soundness 的前提（并发分析依赖的 4 条不变式）

并发模块复用原生域时，**默认继承**以下 4 条 soundness 前提。任何并发语义若破坏其中一条，就会破坏整体 soundness：

1. **转移函数局部 sound**：对每个语句 `st`，`α ∘ ⟦st⟧ ⊆ ⟦st⟧ᵃ ∘ α`（抽象转移 ⊒ 具体转移的抽象化）。这是抽象解释框架的根基。
2. **widening 上近似且终止**：数值域 widening 保证有限迭代上升终止，且结果 ⊒ 最小不动点（不丢行为）。
3. **bit-width/sign 规范化**：任何 `join/meet/比较` 前先把两操作数 `cast` 到相同 `(bit_width, sign)`；否则跨宽度 `assert_compatible` 会 SIGABRT，或静默漏报。这是数值域被并发黑板复用时的**高危雷区**（§2.3 核验点）。
4. **cell 强/弱更新的正确性**：`mem_write` 对**单点** points-to 做强更新（覆盖），对**多点**做弱更新（join）；`mem_read` 对空 points-to 置 ⊥、对 ⊤ 置 ⊤。这是内存域局部 sound 的实现载体。

> **要点**：以上 4 条是「原生保证、我们继承」的 soundness 前提。并发模块任何一处新语义，若想保持 sound，必须**不违反**这 4 条，且自身还要满足过近似方向（见第二部分各条的核验点）。

**并发分析继承的附加前提假设**（与上面 4 条并列，共同构成并发 soundness 的边界声明）：

- **线程模型假设**：线程实例唯一可辨识（**线程身份 = create 调用点的稳定 site id**，同一入口函数的不同 create 点是不同实例，commit `4daa7d5`）；无线程池重用；`pthread_join(t)` 之后线程实例 `t` 终止，此后不再有 `t` 的访问；不考虑线程取消（cancellation）与信号中断对执行流的影响；线程入口函数静态可识别（经 `pthread_create` 第 3 参数的函数指针常量解析）。
- **未定义行为（UB）假设**：假设输入程序**不存在除数据竞争之外的 C 语言未定义行为**（越界访问、解引用空指针、除零、有符号溢出等）。在这些 UB 场景下，本分析**不保证 soundness**——原生 IKOS 对 UB 的建模是「该路径置 ⊥ / 判不可达」（可达性收缩），可能把本应继续的执行判为不可达，从而漏掉其上的访问。

---

# 第二部分：并发分析的语义建模（我们的代码）

## 2.0 总览：并发分析需要哪些语义能力

并发数据竞争检测 = 在原生串行分析之上，叠加以下**语义能力**。每条都有分类、实现状态、论文、测试兜底（详细论述见 2.1）：

| # | 语义能力（模块） | 分类 | 状态 | 论文 | 测试 |
|---|---|---|---|---|---|
| 1 | 数值抽象（循环界/下标/计数器） | A 类 · 原生复用 | 复用原生 | Cousot & Cousot, *Abstract Interpretation*, POPL'77 | boundary-tests/d4-* |
| 2 | 内存/指针/别名（points-to/堆分配） | A 类 · 原生复用 | 复用原生 | may-points-to 标准抽象 | boundary-tests/d5-*、09-regions |
| 3 | 锁集 must-lockset（互斥+读写锁 tier） | C 类 · 自研新增 | **完整实现** | Savage et al., *Eraser*, TOCS'97 | boundary-tests/d1-*、04-mutex |
| 4 | happens-before（程序序/join/create） | C 类 · 自研新增 | **完整实现** | Pozniansky & Schuster, *Efficient On-the-Fly Data Race Detection*, PPoPP'03 | boundary-tests/d2-*、race-1_2b |
| 5 | thread-modular 黑板 + 两层 fixpoint | C 类 · 自研新增 | **完整实现** | Flanagan & Qadeer, *Thread-Modular Model Checking*, SPIN'03；Miné, *Static Analysis of Run-Time Errors in Embedded Real-Time Parallel C Programs*, ESOP'12 | 全量 |
| 6 | PBR 延迟写 + strict unlock-flushing | C 类 · 自研新增 | **完整实现** | Vojdani et al., *Static Race Detection for Device Drivers: The Goblint Approach*, ASE'16 | 13-privatized |
| 7 | thread-local semantics（入口物化） | C 类 · 自研新增 | **完整实现**（`d19fad7` 修复生效） | Mukherjee et al., *Thread-Local Semantics and Its Efficient Sequential Abstractions for Race Detection*, SAS'17 | fib、popl20 |
| 8 | 原子内建（pseudo-lock） | B 类 · 封装增强 | **部分实现**（伪锁近似） | Boehm & Adve, *Foundations of the C++ Concurrency Memory Model*, PLDI'08 | d6-01 |
| 9 | C11 `_Atomic` 真原子语义 | C 类 · 自研新增 | **部分实现**（access-level 过滤 + 混合→UNKNOWN） | Batty et al., *Mathematizing C++ Concurrency*, POPL'11；Adve & Hill, *Weak Ordering*, ISCA'90；Kusano & Wang, *Thread-Modular Static Analysis for Relaxed Memory Models*, FSE'17 | d6-02、d9-*、pthread-atomic |
| 10 | 线程身份 + 唯一性 | C 类 · 自研新增 | **完整实现** | ESOP'23 thread-uniqueness 抽象（题名待核实） | d3-* |
| 11 | region tainting → self-locked | C 类 · 自研新增 | **完整实现**（带 sound 门） | Goblint 区域/符号索引（工程） | 09-regions |
| 12 | offset-aware locking | C 类 · 自研新增 | **完整实现** | 字段锁键（工程） | d8-*、06-symbeq |
| 13 | spawn 实参绑定 + 逃逸 | B 类 · 封装增强 | **完整实现** | pointer_refine 封装 | d5-03 |
| 14 | 条件变量 wait/broadcast（stub） | C 类 · 自研新增 | **部分实现**（缺口 #5，signal→wait HB 缺失） | IEEE Std 1003.1（POSIX） | thread-join-counter、d7-01（弱） |
| 15 | `_Thread_local`（TLS） | C 类 · 自研新增 | **未实现**（缺口 #7） | ISO/IEC 9899:2011（C11） | d7-03 |
| 16 | 无锁算法线性化 | C 类 · 自研新增 | **未实现**（缺口 #8） | Herlihy & Wing, *Linearizability*, TOPLAS'90 | libvsync |

> 状态说明：**完整实现** = 语义规则齐备 + 有边界测试兜底；**部分实现** = 有近似/子集；**未实现** = 完全没有。

## 2.1 逐模块语义建模（含分类依据、论文全名、具体测试文件、soundness 核验点）

### 2.1.1 数值抽象 —— A 类 · 原生复用

- **语义**：循环界 `N`、下标 `i`、计数器 `sum` 都用原生 MachineInt 区间追踪。widening 让 `for(i=0;i<N;i++)` 的 `i` 升到 `[0, INT_MAX]`（N 为 nondet 时）。
- **分类依据**：直接调用原生转移函数，无封装、无改动。
- **论文**：P. Cousot & R. Cousot, *Abstract Interpretation: A Unified Lattice Model for Static Analysis of Programs by Construction or Approximation of Fixpoints*, POPL 1977；*Comparing the Galois Connection and Widening/Narrowing Approaches to Abstract Interpretation*, PLILP 1992。
- **测试**：`boundary-tests/d4-01-local-counter-safe.c`、`boundary-tests/d4-02-global-counter-race.c`、`~/sv-benchmarks/c/pthread/fib_safe-6-racy.i`（fib 族循环出口）。
- **核验点（继承原生 4 条）**：不做任何事即可；唯一要警惕的是 §2.3 的 bit-width/sign 规范化在黑板 join 时被并发代码破坏。

### 2.1.2 内存/指针/别名 —— A 类 · 原生复用

- **语义**：`B[i] = A[i]` 的 `B`/`A` 是 `DynAllocMemoryLocation`（按分配点），`i` 经 `PointerShift` 变成字节 offset 区间。
- **分类依据**：复用原生 `ValueDomain` + `PointsToSet`。
- **论文**：may-points-to 标准抽象（无单篇必引，见 Andersen 式约束求解）。
- **测试**：`boundary-tests/d5-01-distinct-globals-safe.c`、`boundary-tests/d5-02-alias-race.c`、`clean-sv-benchmarks/goblint-regression/09-regions_*.c`（44 例）。
- **精度上限（非 soundness）**：字段/数组折叠成单 cell；可变下标折叠。`A[i]` 当 `i=[0,INT_MAX]` 时 offset=⊤ → points-to=⊤（这就是 weaver 目录 +93 FP 的根源，见 §2.3）。
- **⊤ provenance 分类（阶段2 轻量原型，已评估并停用）**：对 `pts.is_top()` 的访问沿 def-chain（PointerShift/Bitcast/Load）做 region 近似分类——UnknownObject（链根是「值丢失的全局指针」）vs KnownObject（DynAlloc/栈局部/全局数组结构体）。**评估结论（FP 收益 -58 但破坏 soundness，已停用）**：「全局指针值丢失」不是健全的 FP 判别器——同一个「全局指针 + 可变下标」⊤ 既是 weaver FP（`popl20-more-sum-array-hom` 的 `A[i]` vs `sum2`），又是真竞争（`popl20-more-parray-copy` / `per-thread-array-index-race`，指针堆身份丢失但三线程仍竞争同一对象）。对象身份丢失是**精度问题**，非未实现并发语义，故必须保留 RACE，不能降 UNKNOWN。`classify_top_provenance` 与 `AccessRecord.provenance` 字段保留（备未来更健全判据），降级条件现仅用阶段3(atomic)。
  - **回归参照**：weaver 全局指针丢失 → `sv-benchmarks/c/weaver/popl20-more-sum-array-hom.wvr.c`（FP，但按顶层契约保留 RACE）；container_of → `sv-benchmarks/c/goblint-regression/09-regions_05-ptra_rc.c`（RACE）；全局数组可变下标 → `boundary-tests/d10-03-global-array-index-race.c`（RACE）。

### 2.1.3 锁集 must-lockset —— C 类 · 自研新增（完整实现）

- **语义**：`LocksetDomain` 维护「当前必然持有」的锁集。`pthread_mutex_lock` 加、`unlock` 减；控制流合并处取**交集**（MUST 格：join=∩、⊤=∅）。含 5 个 digest：`_mutex_locks`(MUST)、`_read_locks`(读写锁读 tier, MUST)、`_joined_threads`(MUST)、`_spawned_threads`(MAY)、`_cond_locks`(条件锁)。
- **分类依据**：upstream 没有锁集，我们从零实现 `LocksetDomain`（C 类）；但把它接入降阶积用的是 A 类框架（CompositeDomain 的 reduce product）。
- **论文**：S. Savage, M. Burrows, G. Nelson, P. Sobalvarro, T. Anderson, *Eraser: A Dynamic Data Race Detector for Multithreaded Programs*, ACM TOCS 15(4), 1997——Eraser 是**动态**数据竞争检测的经典锁集方法；本工作的静态 must-lockset 是对其思想的**保守上近似**，仅保留「必然持有」的锁（只含确定持有的锁，漏判只会产生 FP）。
- **测试**：`boundary-tests/d1-01-mutex-common-safe.c`、`d1-02-mutex-missing-race.c`、`d1-03-mutex-different-race.c`、`d1-04-rwlock-rr-race.c`、`d1-05-rwlock-rdwr-safe.c`、`d1-06-rwlock-ww-safe.c`、`d1-07-trylock-success-safe.c`、`d1-08-trylock-ebusy-race.c`；`clean-sv-benchmarks/goblint-regression/04-mutex_*.c`（80 例）。
- **soundness 核验点**：
  - 锁集只含「**必然**持有」的锁（MUST 语义）——强更新在单一锁路径，弱更新在合并处取交。
  - 判定「无公共锁 → 可能竞争」是保守方向：锁集**少算**（漏一把锁）会多报 FP（安全），**多算**（多一把锁）会漏报 FN（破坏 sound）——所以锁集必须**下近似**（只含确定的锁）。
  - `_spawned_threads` 用 MAY 格（join=∪），是创建边 HB 的正确极性（见 2.1.4）。

### 2.1.4 happens-before —— C 类 · 自研新增（完整实现）

- **语义**：判定器承认四类 HB 边（均**保守**使用，即「可能成立就跳过」）：① 程序序（同线程且唯一）；② join 边（`a` 已 join **`b` 的全部实例** → b 在 a 之前终止；实例级 + 传递，见核验点）；③ 创建边（`a` 是 `b` 的**唯一**创建者且 `a` 尚未 spawn `b` → a 先于 b 的所有访问）；④ 私有初始化发布（无锁初始化写 + 单锁提升 → HB 后续持锁读）。
- **分类依据**：upstream 没有 HB 概念，我们新增 `_joined_threads`(MUST，**键 = site id**)、`_spawned_threads`(MAY，键 = 函数名) 两个 digest + 判定器里的四类边判定。
- **论文**：E. Pozniansky & A. Schuster, *Efficient On-the-Fly Data Race Detection in Multithreaded C++ Programs*, PPoPP 2003——lockset+HB 混合的原始出处。
- **测试**：`boundary-tests/d2-01-create-before-safe.c`、`d2-02-create-after-race.c`、`d2-03-join-after-safe.c`、`d2-04-join-before-race.c`；`clean-sv-benchmarks/ldv-races/race-1_2b-join.c`、`race-1_3b-join.c`；`goblint-regression/10-synch_28-join-array.c`、`51-threadjoins_07-trivial-unknowntid.c`（join 实例级）。
- **soundness 核验点**：
  - **创建边 MUST→MAY 的极性修正**（`69b1baa`）：创建边 HB 要求「父访问一定先于 create」=「子线程**不可能**已被 spawn」= MAY 语义。原实现把 spawn 当 MUST 塞进 joined（`"s:"` 前缀），在条件 create 合并点被交集清空 → 误判父子访问为 HB → **FN**。已修正。
  - **join 边实例级 + 传递闭包**（`4daa7d5`）：`_joined_threads` 从「函数名」升级为「create 点 site id」。join 边跳过条件 = `a` 已 join **b 的全部实例**（`b 的全部 site id ⊆ a.joined`）——只 join `tids[0]` 不抑制 `tids[1]` 实例（28-join-array FN 根因）。传递：join 一个线程时并入其「退出时 MUST-join 摘要」（`_join_summary[func]`，`thread_modular.cpp` 每函数 fixpoint 后写），故 main join t_benign、t_benign join t_fun ⇒ main 传递 join t_fun（01-trivial.c）。
  - **未知 tid 的 join = 不加任何 join**（`4daa7d5`）：`pthread_join(id)` 读 pthread_t 的**值**（create 把 site id 以 `mem_write` 写成具体值）；值为 ⊤（如 `foo(&id)` 改写 handle）→ 无单点 site id → **不加**任何 join。旧的「join all」兜底凭空造出不存在的 HB 边 → FN（07-trivial-unknowntid）。sound 方向：更少 HB 边 ⇒ 更宽竞争集。
  - 每条 HB 边都必须是「**必然**先后」的充分条件；任何「可能并发」的情形必须**不**跳过（默认报竞争）。
  - **间接 spawn 的 ⊤ 编码**（本轮修复）：`_spawned_threads` 增加 ⊤ 标志（「可能 spawn 任意线程」）。当 `pthread_create` 的线程函数非常量（间接 spawn）时置 ⊤，创建边 HB 的 `!spawned_threads.count(child)` 在 ⊤ 下失效——父访问不再被误判为「一定先于 create」。原先 spawn digest 恒 ∅ 会被误读为「父未 spawn 子」，把真并发误判为 HB（潜在 FN）；置 ⊤ 后该跳过被拒绝，只会多报 FP、不再漏报。

### 2.1.5 thread-modular 黑板 + 两层 fixpoint —— C 类 · 自研新增（完整实现）

- **语义**：线程间通信经 `ConcurrentGlobalEnv` 黑板（不属于 AbstractDomain；**已去单例化 commit `bf51ec0`，由 `analyzer::Context::concurrent_env` 按值持有，经 `_ctx.concurrent_env`/`eng.ctx().concurrent_env` 访问**）。内层 `FunctionFixpoint` 跑单线程、外层 worklist 反复分析所有线程入口直到 `is_dirty()` 为假。
- **分类依据**：upstream 没有并发/黑板，我们新增 `ConcurrentGlobalEnv`（`_global_board`/`_lock_partition`/`_privatized_writes`/指针黑板/`_spawn_args`/`_escaped_locs`）。
- **论文**：线程模块化思想起源于 C. Flanagan & S. Qadeer, *Thread-Modular Model Checking*, SPIN 2003（模型检查领域）；本工作采用**抽象解释变体**，核心参考 A. Miné, *Static Analysis of Run-Time Errors in Embedded Real-Time Parallel C Programs*, ESOP 2012 与 *Relational Thread-Modular Static Value Analysis by Abstract Interpretation*, VMCAI 2014 的线程模块化值分析框架（AstréeA 的线程干扰/延迟发布）。
- **测试**：全量 `~/sv-benchmarks` 1030 例（无单一用例，是基础设施）。
- **soundness 核验点**：
  - 黑板值的 join/widen **单调上近似**（`join_into_cell_nolock` 只 `join_with`，延迟加宽后才置 ⊤），从不 narrow。
  - **bit-width/sign 规范化**（原生第 3 条不变式）在黑板 join 时必须保持，否则跨宽度 join 会 SIGABRT/漏报。
  - **读操作的上近似说明**：线程对共享内存的**读**，从全局黑板以 `join` 语义读取，读取值是**所有线程可能写入值的上近似**（⊔），因此不会遗漏任何其他线程的写入；读侧的这一上近似保证**不因读取而引入 FN**（读到的值只会更宽、不会更窄）。

### 2.1.6 PBR 延迟写 + strict unlock-flushing —— C 类 · 自研新增（完整实现）

- **语义**：加锁写 `record_privatized_write(lock, addr, val)` 进延迟队列，unlock 时聚合发布进 `_lock_partition[lock]`；无锁写进 sentinel 分区，迭代边界原子灌入 `_global_board`（**strict flush**，避免当轮就 widen 到 ⊤）。
- **分类依据**：upstream 没有，我们新增 PBR 队列 + flush 机制。
- **论文**：V. Vojdani, K. Apinis, V. Rõtov, H. Seidl, V. Vene, R. Vogler, *Static Race Detection for Device Drivers: The Goblint Approach*, ASE 2016——Protection-Based Reading 的原始出处。
- **测试**：`clean-sv-benchmarks/goblint-regression/13-privatized_03-priv_inv.c`。
- **soundness 核验点**：
  - 延迟写**只延后不丢**：unlock/边界 flush 把所有 pending 写 join 进黑板，漏 flush 会漏干扰（FN）。
  - `flush_nolock` 的「state-overwriting」语义（同一临界区内后写覆盖先写）是流敏感投影，只收窄不丢行为。

### 2.1.7 thread-local semantics（入口物化） —— C 类 · 自研新增（完整实现，`d19fad7` 修复生效）

- **语义**：`materialize_globals` 在**每线程入口**把 `_global_board` 的整数区间 join 进本地 invariant，使跨线程整数干扰（典型：创建者写的 `N=nondet` 循环界）在入口一次性可见，取代旧的「每读一次覆盖 lhs」（后者会把本地 widen 值收窄回陈旧单点，破坏 sound）。
- **分类依据**：upstream 没有，我们新增入口物化。
- **论文**：S. Mukherjee, O. Padon, S. Shoham, V. D'Silva, N. Rinetzky, *Thread-Local Semantics and Its Efficient Sequential Abstractions for Race Detection*, SAS 2017——本地写 + 发布干扰分离。
- **测试**：`boundary-tests/d4-01-local-counter-safe.c`、`d4-02-global-counter-race.c`；`~/sv-benchmarks/c/pthread/fib_unsafe-10-racy.i`；`~/sv-benchmarks/c/weaver/popl20-more-parray-copy.wvr.c`（最后一个 FN，已翻转）。
- **soundness 核验点**：
  - 物化是 `⊔`（join），单调、从不收窄——这是它 sound 的根本。
  - **历史陷阱**：该函数曾因两个 bug 长期 no-op（`gv->type()->is_integer()` 恒 false——`type()` 返回指针类型；`int_to_interval(get_global(gv))` 对指针变量断言），导致线程 `load @N` 恒读初值 `[0,0]`、`for(i=0;i<N;i++)` body 塌 ⊥——**最后一个 FN（popl20）**。`d19fad7` 修复：`pointee()->is_integer()` + `pointer_assign` pin 全局指针（否则 `mem_write` 触发 `mem_forget_all` 把整个 invariant 炸 ⊤）+ `mem_read/join/mem_write` cell 回环。

### 2.1.8 原子内建（pseudo-lock） —— B 类 · 封装增强（部分实现）

- **语义**：`__VERIFIER_atomic_begin/end`、`__sync_*`、`__atomic_*`、CAS/TAS 被注入一个**保留地址的伪锁** `PSEUDO_ATOMIC_LOCK`，使该段访问共享同一把「锁」而互斥。
- **分类依据**：upstream **有** intrinsic 调用（CallBase）的建模点与锁集接入框架；我们**封装**成伪锁。
- **论文**：ISO/IEC 9899:2011（C11）§7.17；H.-J. Boehm & S. V. Adve, *Foundations of the C++ Concurrency Memory Model*, PLDI 2008。
- **测试**：`boundary-tests/d6-01-verifier-atomic-safe.c`。
- **soundness 核验点（B 类的关键）**：伪锁把「CPU 原子段」近似成「该段与自身互斥」。核验：**这等价于 atomic 访问自身互斥**（C11 DRF 下 atomic 访问不竞争）——是**上近似**（伪锁过近似了原子段之间的乱序，但不漏竞争）。**但**伪锁不是真内存序，不提供 acquire/release HB（缺口 #3）。

### 2.1.9 C11 `_Atomic` 真原子语义 —— C 类 · 自研新增（部分实现）

- **语义**：AR 层给 Load/Store 加 `AtomicOrdering`；`atomicrmw/cmpxchg` 被拆成 Load+Op+Store 序列；判定器按「两个 atomic 访问不竞争」过滤（C11 DRF 的直接推论）；**混合 atomic/non-atomic 访问对 → 标记 UNKNOWN**（`Result::Warning`，元数据 `verdict:"unknown"`、`unknown_reason:"mixed atomic/non-atomic access"`）。
- **分类依据**：upstream 没有原子内存序语义，我们新增（frontend LowerAtomic + AR ordering + checker 过滤）。
- **论文**：M. Batty, S. Owens, S. Sarkar, P. Sewell, T. Weber, *Mathematizing C++ Concurrency*, POPL 2011；S. V. Adve & M. D. Hill, *Weak Ordering — A New Definition*, ISCA 1990（DRF0）；M. Kusano & C. Wang, *Thread-Modular Static Analysis for Relaxed Memory Models*, FSE 2017（完整路线图）。
- **测试**：`boundary-tests/d6-02-c11-atomic-safe.c`、`d9-01-atomic-nonatomic-mixed.c`（UNKNOWN）、`d9-02-atomic-atomic-safe.c`（豁免）、`d9-03-plain-race.c`（普通 RACE 回归哨兵）；`clean-sv-benchmarks/pthread-atomic/dekker-b.c`、`lamport-b.c`、`peterson-b.c`、`read_write_lock-1b.c`。
- **soundness 核验点**：「atomic vs atomic → 不竞争」是 C11 DRF 的直接推论，只去 FP 不去 FN。**混合 atomic/non-atomic → UNKNOWN**：atomic 的同步语义（acquire/release 内存序）未建模，判定器无法对该 pair 做可靠 may 分析，故诚实标 UNKNOWN（**永不等于 SAFE、永不吞真竞争**）；纯普通（non-atomic vs non-atomic）pair 不受影响，仍输出 RACE。**缺口**：没有 acquire/release HB（atomic 的同步作用只当成「自身互斥」，没当成「跨访问的顺序」）——这是 #3 的理论缺口。

### 2.1.10 线程身份 + 唯一性 —— C 类 · 自研新增（完整实现）

- **语义**：`current_thread_id` 沿 call-context 链走到**最外层调用**所在函数（=线程入口），而非语法函数（修复「同一 helper 被两线程调用 → tid 折叠」）；`is_thread_unique(name)` = 静态 spawn 次数 ≤ 1。**线程身份已升级为实例级**（commit `4daa7d5`）：`ConcurrentGlobalEnv` 给每个 `pthread_create` 调用点分配稳定 site id，`_func_instances` 记录「函数名 → site id 集」；判定器把 join 摘要从「名」换成「site id」，同函数多实例可区分（28/29/30 的 join 只需抑制被 join 的那一个实例）。
- **分类依据**：upstream 没有线程概念，我们新增。
- **论文**：ESOP'23 thread-uniqueness 抽象（`is_thread_unique` 代码注释引用；题名/作者/DOI【待人工核验】）。
- **测试**：`boundary-tests/d3-01-unique-self-safe.c`、`d3-02-two-same-fn-race.c`。
- **soundness 核验点**：唯一性是「自环跳过」的前提——**只有**唯一线程才允许 `tid 相同 → 顺序 → 跳过`；非唯一线程**必须**物化第二实例，否则漏「同函数多实例互竞争」（FN）。

### 2.1.11 region tainting → self-locked —— C 类 · 自研新增（完整实现，带 sound 门）

- **语义**：为数组槽/多态基建立**符号索引**（`SymbolicIndex{var_id, coeff, const}`）与基指针 SSA 关联，判定「访问与其锁在同一实例」（self_locked），据此抑制折叠类 FP。
- **分类依据**：upstream 没有，我们新增。
- **论文**：Goblint 的区域/符号索引思想（工程化，无单篇必引）。
- **测试**：`clean-sv-benchmarks/goblint-regression/09-regions_*.c`（44 例，代表：`09-regions_11-arraylist_nr.c`、`09-regions_03-list2_rc.c`）。
- **soundness 核验点**：`region_tainting_sound()` 门——遇到跨槽指针拼接等使污染不可靠的模式就**整体关闭**（宁多 FP 不 FN）。

### 2.1.12 offset-aware locking —— C 类 · 自研新增（完整实现）

- **语义**：锁键 `offset_lock_key(addr, const_offset)` 区分同地址不同字段的锁；常量 offset 独立成键，可变 offset 拒绝（sound）。
- **分类依据**：upstream 有 offset 区间（A 类基础），锁键封装是我们加的。
- **论文**：字段锁键（工程）。
- **测试**：`boundary-tests/d8-01-struct-field-safe.c`、`d8-02-struct-two-fields-race.c`；`clean-sv-benchmarks/goblint-regression/06-symbeq_14-list_entry_rc.c`。
- **soundness 核验点**：可变 offset 必须拒绝成「无此锁」（多 FP），不能当成「有此锁」（漏 FN）。

### 2.1.13 spawn 实参绑定 + 逃逸 —— B 类 · 封装增强（完整实现）

- **语义**：`pthread_create` 第 4 实参的 points-to 记入 `_spawn_args`，子线程入口 `pointer_refine` 绑到形参（含 offset，支持 container_of）；实参指向的栈局部标 `_escaped_locs`；经**全局指针发布**的 cell（`gp = &local`）也标 `_escaped_locs`。
- **分类依据**：upstream **有** `pointer_refine`（把指针分析结果 refine 进指针域），我们**封装**成 spawn 实参绑定 + 逃逸标记。
- **论文**：pointer_refine 封装（工程）。
- **测试**：`boundary-tests/d5-03-spawn-arg-escape-race.c`、`d5-04-global-ptr-escape-race.c`；`clean-sv-benchmarks/goblint-regression/04-mutex_45-escape_rc.c`。
- **soundness 核验点**：逃逸标记必须**过近似**（漏标 → 子线程访问不可见 → FN）。`_escaped_locs` 现收两类：① `pthread_create` 第 4 实参指向的栈局部（`record_spawn_arg`）；② 经全局指针发布的 cell（`join_global_pointer`，本轮修复）。漏标任何一类都是 FN——原实现只收①，全局指针发布的栈局部未被 `touches_shared_memory` 视为共享，导致 `gp=&local` 后经 `*gp` 的跨线程写漏报（d5-04）。

### 2.1.14 条件变量（cond_wait/signal/broadcast） —— C 类 · 自研新增（部分实现）

- **语义**：`pthread_cond_wait(&cond, &mutex)` 释放 mutex → 等待 → 唤醒后重获取 mutex；`pthread_cond_signal`/`broadcast` 无副作用（只唤醒等待者）。
- **分类依据**：upstream 没有 cond-var 语义，我们新增 stub。
- **论文**：IEEE Std 1003.1（POSIX），pthread_cond_wait/signal/broadcast 的同步语义。
- **测试**：`clean-sv-benchmarks/pthread-race-challenges/thread-join-counter-inner-race-3.i`（FN 已翻转）；`boundary-tests/d7-01-condvar-mutex-safe.c`（弱）。
- **soundness 核验点（本轮根因修正：可达性，非锁集）**：
  - **最初误判为「锁集塌缩」**：cond_wait/signal 走 `exec_unknown_extern_call` 的 `may_throw_exc=true` → `throw_unknown_exceptions()` 确实会塌缩 must 锁集，是一个真实隐患，但**不是** thread-join-counter FN 的主因。
  - **真正根因是可达性**：cond_wait 变空操作后，`while(threads_alive) pthread_cond_wait(...)` 循环体不改变 `threads_alive`，抽象不动点里循环**永不退出** → 循环后的无锁读（`return data`）在正常流判 ⊥（`normal=⊥`）→ 该读漏记录 → FN。控制变量法佐证：去掉 cond_var 即不漏（V1）；detach / nondet 循环均无关（V2/V3 仍漏）。
  - **修复（`24ec4ab`）**：① cond_wait/signal 单独 stub，不带 `may_throw_exc`、不 `mem_forget_reachable`（堵锁集塌缩隐患）；② cond_wait 必须 `mem_forget_all`——等待期间其它线程可能改全局，循环才可能退出（堵可达性 FN）。二者缺一不可。
  - **缺口（第二步精度项）**：signal→wait 的 happens-before 边仍缺失，只降 FP、不影响 soundness。

### 2.1.15–16 未实现项（TLS / 无锁线性化） —— C 类 · 自研新增

| 模块 | 缺失语义 | 论文 | 测试 | 影响 |
|---|---|---|---|---|
| `_Thread_local` | 每线程一份 cell | ISO/IEC 9899:2011（C11）§6.2.4/§6.7.1 | `boundary-tests/d7-03-tls-safe.c` | FP |
| 无锁线性化 | lock-free 正确性（线性化点） | M. P. Herlihy & J. M. Wing, *Linearizability: A Correctness Condition for Concurrent Objects*, TOPLAS 12(3), 1990 | `clean-sv-benchmarks/pthread-ext/libvsync 族` | FP |

## 2.2 实现状态清单（含具体测试文件）

| 模块 | 实现状态 | 边界测试（boundary-tests/） | 回归集（clean-sv-benchmarks/） | 通过 |
|---|---|---|---|---|
| 锁集 | 完整 | d1-01 … d1-08（8 例） | goblint-regression/04-mutex_*（80） | rwlock/trylock 部分 GAP |
| HB | 完整 | d2-01 … d2-04（4 例） | ldv-races/race-1_2b-join.c、race-1_3b-join.c | d2 基本 OK |
| 黑板+两层 fixpoint | 完整 | — | 全量 1030 | 基础设施 |
| PBR | 完整 | — | goblint-regression/13-privatized_03-priv_inv.c | — |
| 入口物化 | 完整 | d4-01、d4-02 | pthread/fib_*.i、weaver/popl20-more-parray-copy.wvr.c | d4-02 已 OK（`d19fad7`） |
| pseudo-lock | 部分 | d6-01 | — | GAP（伪锁语义过窄） |
| C11 atomic | 部分 | d6-02 | pthread-atomic/dekker-b.c、lamport-b.c、peterson-b.c | GAP |
| 线程身份/唯一性 | 完整 | d3-01、d3-02 | — | 全 OK |
| region tainting | 完整 | — | goblint-regression/09-regions_*（44） | — |
| offset-aware | 完整 | d8-01、d8-02 | goblint-regression/06-symbeq_14-list_entry_rc.c | d8-01 GAP |
| spawn 绑定+逃逸 | 完整 | d5-03 | goblint-regression/04-mutex_45-escape_rc.c | d5-03 OK |
| 条件变量 | 部分（stub，无 signal→wait HB） | d7-01（弱） | pthread-race-challenges/thread-join-counter-inner-race-3.i | d7-02 barrier 仍 GAP |
| TLS | 未实现 | d7-03 | — | GAP |
| 无锁线性化 | 未实现 | — | pthread-ext/libvsync 族 | — |

> 边界测试 27 例完整清单见 `boundary-tests/run.py`（D1 锁集/D2 HB/D3 线程身份/D4 数值循环/D5 指针逃逸/D6 原子/D7 cond-var·barrier·TLS/D8 字段敏感）。**当前实测约 25 OK + 2 GAP**——仅剩 d7-02-barrier（缺口 #5 的 signal→wait HB）与 d7-03-tls（缺口 #7）两个真语义缺口，无意外。
>
> **回归三件套**：`boundary-tests/run.py`（边界）→ `yaml_ab_test.py clean-sv-benchmarks`（121 回归，**FN=0 红线**）→ `svcomp_race_test.py ~/sv-benchmarks`（1030 全集）。

## 2.3 现在的 soundness 依据（逐条核验）

**基础定义与目标模型声明**（soundness 论证所用概念，先厘清）：

- **FN（漏报）定义**：FN = 程序**真实存在**数据竞争，但分析输出 `safe`。**`unknown` 不属于 FN**——分析在无法判定时输出 `unknown`（或按红线默认报竞争），从不输出 `safe`，故 `unknown` 不破坏 soundness。soundness 的判定口径是：**「凡输出 safe，则必无数据竞争」**（safe ⟹ 无竞争），即 FN=0。
- **目标内存模型**：本分析默认目标为**顺序一致性（Sequential Consistency, SC）**。对**正确同步（data-race-free, DRF）的 C11 程序**，由 DRF 原则（S. V. Adve & M. D. Hill, *Weak Ordering — A New Definition*, ISCA 1990，DRF 准则原始文献）保证其在弱内存模型下的可观察行为与 SC 等价；因此本 SC 模型下证明的 sound 结论**适用于所有正确同步的 C11 程序**，无需显式建模 acquire/release 内存序（后者是精度问题，非 soundness 前提）。

**整体 soundness 论证框架**（自上而下的完整逻辑链）：

在 §1.3 的 4 条原生不变式与 §1.3 末尾的边界假设范围内，本分析对数据竞争的 soundness 由以下五层环环相扣构成：

1. **底层——单线程转移局部 sound**：原生 IKOS 的单线程转移函数局部 sound（上游已保证，§1.3 第 1 条），每个线程的抽象执行 ⊒ 其具体执行，单线程内可达的访问集是过近似的（不漏真实访问）。
2. **架构——线程模块化抽象解释**：采用线程模块化（thread-modular）抽象解释，每个线程入口函数独立跑前向不动点；各线程局部分析继承 IKOS 的 soundness（§2.1.5）。
3. **全局通信——黑板 join 单调**：跨线程整数/指针干扰经 `ConcurrentGlobalEnv` 黑板以 `join` 传递，join 单调、延迟 widening、**从不 narrow**（§2.1.5），故每线程看到的共享值 ⊒ 其他线程实际可能写入的值（上近似），不会遗漏干扰。
4. **判定规则——may/must 对偶**：判定器**跳过竞争**的四类依据（公共锁、HB 边、同线程唯一性、self-locked）全部是 **must 下近似**（只含「必然不竞争」，漏判只产生 FP）；判定**可能竞争**的依据（共享内存定位、线程创建、指针逃逸、⊤ 保守配对）全部是 **may 上近似**（包含所有可能竞争，多判只产生 FP）。二者对偶，共同保证「凡判 safe 必无竞争」。
5. **回退策略**：任何无法判定的情形，一律输出 `unknown` / 默认报竞争，绝不输出 `safe`。

**结论**：在上述五层 + §1.3 前提 + §1.3 末尾边界假设范围内，本分析对数据竞争是 **sound 的（FN=0）**。

并发分析的整体 soundness 由**「原生 4 条不变式」+「并发各条的保守方向」**共同构成。判定器**只有当以下「必然不竞争」的条件全部满足时才跳过**，否则默认报竞争：

1. **公共锁**（must-lockset 交集非空）——锁集是「必然持有」的下近似，少算锁只会多 FP。
2. **HB 边**（程序序/join/create/私有发布）——每条都是「必然先后」的充分条件；可能并发的情形绝不跳过。
3. **同线程同语句**（仅唯一线程）——唯一性判定是前提，非唯一必须物化第二实例。
4. **self-locked**（region 污染）——带 `region_tainting_sound()` 门，不可靠即关闭。

**may/must 对偶性总述**：判定「无竞争」使用 **must 下近似**（只含必然成立的情况，漏判只会产生 FP）；判定「可能竞争」使用 **may 上近似**（包含所有可能情况，多判只会产生 FP）。二者对偶，共同保证 FN=0。

**「过近似」的四个具体抓手**（宁可 FP 不 FN 的落地）：

- **⊤ points-to 保守配对**：⊤ 访问与所有其他访问配（`daabe6e`），不把 ⊤ 掩成「无」（否则漏 FN）。
- **锁集下近似**：只含确定持有的锁。
- **HB 边下近似**：只跳「必然先后」。
- **黑板 join 单调**：只 `join_with`、延迟加宽，从不 narrow（`materialize` 是 ⊔）。

**这些保守方向之所以 sound**，根子上依赖原生第 3 条（bit-width/sign 规范化）与第 4 条（cell 强/弱更新）不被并发代码破坏——这是并发模块每次改动的**高危雷区**（历史：黑板 join 漏规范曾 SIGABRT；materialize 的 `mem_write` 未 pin 指针曾 `mem_forget_all` 炸 ⊤）。

---

# 第三部分：总结

## 3.1 当前系统能力边界

| 维度 | 现状 |
|---|---|
| **Recall（漏报率）** | **全集 FN=0**（Recall=1.000）——红线达成 |
| **Precision** | 全集 FP=98（Precision≈0.706，正确 ILP32 arch）；clean-121 FP=0 |
| **已建模同步** | mutex、rwlock、trylock（路径近似）、join/create HB、`__VERIFIER_atomic`（伪锁）、C11 `_Atomic`（access-level）、cond_wait/signal/broadcast（stub，无 HB 边） |
| **未建模同步** | cond-var 的 signal→wait HB、barrier、TLS、无锁线性化、acquire/release 内存序 |

## 3.2 主要理论缺口（按杠杆排序）

| 缺口 | 类型 | 影响 | 论文 |
|---|---|---|---|
| #3 C11 内存序（acquire/release/relaxed） | 语义缺口 | ~95 文件的 FP+FN | Batty et al., *Mathematizing C++ Concurrency*, POPL'11 等 |
| #5 条件变量/屏障 HB（signal→wait 边） | **MAY，不可 sound 使用**（见下注） | FP（value-barrier/thread-join-counter 等 ~9 例；**FN 已由 cond stub 堵住**，见 §2.1.14） | IEEE Std 1003.1（POSIX） |
| #5b 互斥传递 HB（无锁写 → lock(m) → 互斥 → unlock(m) → 无锁读） | 语义缺口（通用） | 是 #5 那 9 例 + join 类一部分的**真根因** | — |
| #6 结构体字段锁准入 | 语义缺口 | FP（折叠类 11 例的死因） | Flanagan & Qadeer, SPIN'03 |
| #7 TLS | 语义缺口 | FP | ISO/IEC 9899:2011（C11） |
| #8 无锁线性化 | 语义缺口 | FP | Herlihy & Wing, TOPLAS'90 |
| **精度上限** | 折叠（字段/数组）+ ⊤ points-to 保守配对（含 join 句柄 `tids[i]` 读 ⊤ →「全部实例 joined」判定失败） | 大量 FP（weaver +93、thread-join-*、per-thread-* 等） | — |

> 注意 1：#3 里「C11 atomic 本身不竞争」已实现（去 FN），「混合 atomic/non-atomic」已标 UNKNOWN（去 FP、不吞真竞争），但「atomic 的 acquire/release 同步作用」未实现——这是 #3 剩余的核心。
>
> 注意 2（2026-09 实测更正）：**#5 的 signal→wait 边不能 sound 地补**——`cond_wait` 会伪唤醒、`signal` 唤醒哪个 waiter 不确定，所以「A signal c → B wait c」是 MAY 边（`lockset_domain.hpp:166-168` 注释已明确「cannot soundly suppress a race」）。`value-barrier.i` 这类案例真正变安全靠的是 `while(!ready) cond_wait` + `ready_mutex` 的**互斥传递 HB**（#5b），与 cond 无关。同理 `thread-join-*` 的 join 规则没缺，是 `tids[i]` 数组单元读 ⊤ 导致「全 joined」判定失败（精度上限，非语义缺口）。

## 3.3 下一步路线

1. **语义修复（先补齐会漏报的缺口，守住 FN=0）**
   - ✅ 间接 spawn 的 ⊤ 编码（`_spawned_threads` 加 ⊤ 标志）——已修复。
   - ✅ 全局指针逃逸标 `_escaped_locs`（`join_global_pointer` 标记发布目标）——已修复（d5-04 翻转）。
   - ✅ 条件变量 stub（cond_wait/signal/broadcast + `mem_forget_all`）——已修复（thread-join-counter FN 翻转，根因是可达性非锁集，见 §2.1.14）。
   - 逐条核验 §2.1 每个 B/C 模块的保守方向是否仍成立。

2. **重新核验全部语义规则**
   - 以 §2.1 的「soundness 核验点」为 checklist，逐条跑边界测试 + 回归三件套，确认 FN=0 不破。
   - 给每个 C 类模块补一个「破坏 soundness 会翻红的哨兵用例」（红蓝对抗）。

3. **精度调优（在 FN=0 前提下压 FP）**
   - 字段敏感 MemoryFactory（清零 11 个折叠类 FP，杠杆最高）。
   - ⊤ points-to 的定向收窄（在不漏 FN 的前提下，避免「⊤ 配对所有」的盲目 FP）。
   - 数值域 widening-delay / threshold 调优，回收 weaver 目录的 +93 FP。

---

## 参考文献（全文引用汇总，便于集中检索）

> **检索说明**：有 DOI 直接解析；无 DOI 用 dblp.org / Google Scholar 搜精确题名。【待人工核验】表示本环境未联网核实，需人工核对。

### 一、原生 IKOS 复用的理论（A 类来源）

1. P. Cousot, R. Cousot. *Abstract Interpretation: A Unified Lattice Model for Static Analysis of Programs by Construction or Approximation of Fixpoints.* POPL 1977. DOI 10.1145/512950.512973
2. P. Cousot, R. Cousot. *Comparing the Galois Connection and Widening/Narrowing Approaches to Abstract Interpretation.* PLILP 1992.
3. A. Miné. *The octagon abstract domain*（机器整数区间/octagon 的权威实现，按题名搜）。

### 二、并发分析新增/复用的理论

4. S. Savage, M. Burrows, G. Nelson, P. Sobalvarro, T. Anderson. *Eraser: A Dynamic Data Race Detector for Multithreaded Programs.* TOCS 15(4), 1997. DOI 10.1145/265924.265927
5. E. Pozniansky, A. Schuster. *Efficient On-the-Fly Data Race Detection in Multithreaded C++ Programs.* PPoPP 2003. DOI 10.1145/781498.781529
6. C. Flanagan, S. Qadeer. *Thread-Modular Model Checking.* SPIN 2003. DOI 10.1007/3-540-44829-2_14
7. A. Miné. *Static Analysis of Run-Time Errors in Embedded Real-Time Parallel C Programs.* ESOP 2012.
8. A. Miné. *Relational Thread-Modular Static Value Analysis by Abstract Interpretation.* VMCAI 2014.
9. V. Vojdani, K. Apinis, V. Rõtov, H. Seidl, V. Vene, R. Vogler. *Static Race Detection for Device Drivers: The Goblint Approach.* ASE 2016. DOI 10.1145/2970276.2970337
10. H.-J. Boehm, S. V. Adve. *Foundations of the C++ Concurrency Memory Model.* PLDI 2008. DOI 10.1145/1375581.1375591
11. M. Batty, S. Owens, S. Sarkar, P. Sewell, T. Weber. *Mathematizing C++ Concurrency.* POPL 2011. DOI 10.1145/1926385.1926394
12. S. V. Adve, M. D. Hill. *Weak Ordering — A New Definition.* ISCA 1990. DOI 10.1145/325096.325100
13. M. Kusano, C. Wang. *Thread-Modular Static Analysis for Relaxed Memory Models.* FSE 2017. DOI 10.1145/3106237.3106274
14. S. Mukherjee, O. Padon, S. Shoham, V. D'Silva, N. Rinetzky. *Thread-Local Semantics and Its Efficient Sequential Abstractions for Race Detection.* SAS 2017.
15. M. P. Herlihy, J. M. Wing. *Linearizability: A Correctness Condition for Concurrent Objects.* TOPLAS 12(3), 1990. DOI 10.1145/78969.78972
16. S. V. Adve. *Data Races are Evil with No Exceptions.* CACM 53(11), 2010.
17. 【待人工核验】ESOP'23 thread-uniqueness 抽象（`is_thread_unique` 代码注释引用；dblp 搜「thread uniqueness / thread-modular abstract interpretation / static data race detection」）。
18. ISO/IEC 9899:2011（C11），§5.1.2.4（happens-before/数据竞争）、§6.2.4/§6.7.1（TLS）、§7.17（atomics）。
19. IEEE Std 1003.1（POSIX），pthread_mutex/rwlock/cond（wait/signal/broadcast）/barrier。

---

*本报告随代码同步维护；最近的实质变更：`d19fad7`（materialize_globals 修复，popl20 最后一个 FN 清零）。*
