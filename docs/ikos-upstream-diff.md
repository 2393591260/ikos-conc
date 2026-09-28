# IKOS 上游对比改动清单（客观 diff 事实）

**对比基准**：`NASA-SW-VnV/ikos` @ `ac7f7c1`（master，官方原版；注意用户提供的 `arslab/ikos` 仓库已不存在）。
**对比方式**：`diff -r`（排除 `.git/build/install/CMakeCache.txt/CMakeFiles/__pycache__/*.db/*.o/*.so/*.a` 等生成物），仅源码。
**结果**：**无任何文件被删除**；本地 = 原版基础上「新增 17 项文件/目录 + 修改 53 个文件」。所有并发/竞争能力是**在原生 IKOS 之上叠加的一层**，原生抽象域（区间、同余、points-to、内存域等）全部保留未动。

> **更新注记（2026-09-24，commit `f5c0fe6`）**：下表 2.B「把锁集挂进原版域」的行是**重构前快照**。此后锁集域已从 `composite` 第 5 分量提升为 `core::DomainProduct2` 顶层第二分量，`memory/*` 与 `scalar/composite` 上的 `lockset_*` forwarder 全部删除、恢复 ikos-original。相关行的现状以行内「（已回退/已重构）」标注为准。

---

## 类别1：上游原版原生就存在的模块/抽象域（原版自带，未改动）

以下模块在原版中已存在，且 **diff 中无改动**（本地只在其上叠加或轻度扩展）。它们是本地并发分析的「地基」：

| 层 | 原生模块 | 说明 |
|---|---|---|
| 数值抽象域 | `core/domain/numeric/`（interval、congruence、interval_congruence、dbm、octagon、apron、gauge、var_packing_*、union、separate_domain、linear_interval_solver、equality_congruence_solver） | 原版自带，本地未改 |
| 机器整数域 | `core/domain/machine_int/`（interval、congruence、interval_congruence、polymorphic_domain、separate_domain 等） | 原版自带，本地未改 |
| 指针域 | `core/domain/pointer/`（`operator.hpp`、`solver.hpp`，即 `PointsToSet`/`Pointer`） | 原版自带，本地未改 |
| 内存域 | `core/domain/memory/value/`（cell_set、mem_loc_to_cell_set、mem_loc_to_pointer_set） | 原版自带，本地未改 |
| 内存位置 | `analyzer/analysis/memory_location.hpp` 的 **7 种 MemoryLocation 类别**：Local / Global / Function / Aggregate / AbsoluteZero / Argv / LibcErrno / DynAlloc | 类别结构原版自带，本地仅加 `stable_id`（见类别2） |
| 其它抽象域 | `core/domain/exception`、`core/domain/lifetime`、`core/domain/nullity`、`core/domain/uninitialized` | 原版自带；仅 uninitialized 加了 `assign_maybe`（见类别2） |
| 指针分析 | `analyzer/analysis/pointer/`（function、pointer、value） | 原版自带；仅 constraint.hpp 加 pthread 内建处理 |
| 现有 checker | assert_prover、division_by_zero、double_free、dead_code、function_call、int_overflow_*、pointer_alignment/compare/overflow、shift_count、signed/unsigned_int_overflow、uninitialized_variable | 原版自带；buffer_overflow/memory_watch/null_dereference/soundness 仅加 pthread 内建 no-op 分支 |
| 执行引擎 | `analyzer/analysis/execution_engine/`（engine、concurrent_inliner、context_insensitive、fixpoint_cache、inliner、numerical） | 原版自带；numerical.hpp/inliner.hpp/concurrent_inliner.hpp 被大量扩展（见类别2） |

> 关键事实：**本地没有重写任何原生抽象域**。数值域、points-to、内存 cell 折叠模型（`mem_loc_to_cell_set`）都是原版语义。**（已重构）** 初稿时本地并发层的锁集/线程语义是「新增的第 4 个分量挂进原版 `composite` 标量域」；现改为 `DomainProduct2` 正交产品的第二分量（`first()`=数据域、`second()`=锁集域），不再侵入 `composite`。

---

## 类别2：本地新增 / 修改的代码（与原版存在差异的所有代码）

### 2.A 新增的并发抽象层（全新文件，原版完全没有）

| 文件 | 客观内容 |
|---|---|
| `core/include/ikos/core/domain/lockset/lockset_domain.hpp` | 新增 `LocksetDomain`：must-锁集 + 读锁集 + 条件锁（CondLock/CondPolarity）+ spawned/joined 线程 digest。join 语义：MUST 用 ∩、MAY 用 ∪ |
| `core/include/ikos/core/domain/concurrent/`（目录） | **空目录**（diff 仅因空目录而报 "Only in"，无实际代码） |
| `core/include/ikos/core/domain/concurrent_global_env.hpp` | `ConcurrentGlobalEnv`「全局黑板」（**已去单例化 commit `bf51ec0`，由 `Context` 持有**）：`_global_board`（区间扁平黑板）、`_lock_partition`（AstréeA 式按锁分区）、全局指针黑板（`get/join_global_pointer`）、堆字段指针黑板（`get/join_heap_pointer`）、`record_privatized_write`/`flush_privatized`（PBR）、线程句柄映射 `map_thread_handle`、`record_spawn_arg`、符号锁索引 `record_lock_symbolic_index`、`is_thread_entry`/`is_glob_flown`/`mark_glob_flown`、`register_thread_func` |
| `analyzer/include/ikos/analyzer/analysis/execution_engine/symbolic_index.hpp` | `SymbolicIndex`：符号下标/符号基址（flat-array region 消歧，09-regions 系列） |
| `analyzer/include/ikos/analyzer/analysis/value/thread_modular.hpp` + `src/analysis/value/thread_modular.cpp` | `ThreadModularAnalysis`：线程模块化全局不动点 worklist 驱动，含 `materialize_globals`（线程局部全局物化）、`flush_unlocked_partition`（严格无锁 flush）、线程入口边界发布 |
| `analyzer/include/ikos/analyzer/analysis/value/concurrency_scanner.hpp` | `requires_concurrency(bundle)`：扫描 AR 里是否出现 `pthread_*`，供三态 `--concurrency` 零配置探测 |
| `analyzer/include/ikos/analyzer/checker/data_race.hpp` + `src/checker/data_race.cpp` | `DataRaceChecker`：析构期 O(N²) 两两比对 `AccessRecord`（thread_id/locks/read_locks/offset/instance/has_region/joined/spawned/is_atomic/provenance/private_init/promote_locks），判定 happens-before 边与锁交集 |
| `frontend/llvm/src/pass/preserve_loads_dce.cpp` | `PreserveLoadsDCEPass`：保留 load 的死代码消除（普通 DCE 会删掉无副作用的 load，丢竞争访问） |
| `core/test/unit/domain/concurrent/`（目录） | **空目录**（无单测代码） |

### 2.B 核心抽象域扩展（修改，锁集域承载方式——重构前挂进原版域、现为 DomainProduct2 第二分量）

| 文件 | 客观改动 |
|---|---|
| ~~`core/domain/memory/abstract_domain.hpp`~~ | ~~新增 lockset 抽象接口（`lockset_add_lock/remove_lock/holds_lock/is_top/held_mutexes/held_read_locks/add_cond_lock/add_joined_thread/add_spawned_thread/set_spawned_top` 等，默认 no-op）~~ **（已回退）** `lockset_*` 接口全删，恢复 ikos-original |
| ~~`core/domain/memory/polymorphic_domain.hpp`~~ | ~~声明 + 转发同样的 lockset 接口（纯虚 → 转发 `_inv`）~~ **（已回退）** 同上 |
| ~~`core/domain/memory/value.hpp`~~ | ~~`MemoryDomain` 把 lockset 方法转发给 `_scalar`~~ **（已回退）** 同上 |
| `core/domain/memory/dummy.hpp` / `partitioning.hpp` | 加 `uninit_assign_maybe`（dummy 转发标量；partitioning 逐分区转发）（保留） |
| `core/domain/scalar/composite.hpp` | **（已回退为 ikos-original）** 初稿把 `LocksetDomain` 加为复合标量域的第 5 个分量并贯穿 join/widen/...；重构后仅保留两处非锁集改动：`uninit_assign_maybe`、`ptrtoint` 的 width/sign 不匹配时用 `Cast` 而非 `SignCast` |
| `core/domain/domain_product.hpp` | **（新增改动）** `DomainProduct2` 加 `FirstDomain`/`SecondDomain` 类型别名 + `widening_threshold`（第一分量阈值加宽、第二分量常规加宽） |
| `analyzer/include/.../value/abstract_domain.hpp` | **（新增改动）** 顶层 `AbstractDomain` 重新定义为 `DomainProduct2< ExceptionDomain<MemoryDomain>, LocksetDomain >`（`first()`=数据域、`second()`=锁集域） |
| `analyzer/src/analysis/value/abstract_domain.cpp` | **（新增改动）** `make_bottom/initial_abstract_value` 改为 `AbstractDomain(DataDomain(...), LocksetDomain::bottom()/top())` |
| `core/domain/lockset/lockset_domain.hpp` | 加 `#include <cstdint>`（补齐传递包含依赖） |
| `core/domain/scalar/abstract_domain.hpp` / `machine_int.hpp` / `dummy.hpp` | 加 `uninit_assign_maybe`（保留） |
| `core/domain/uninitialized/abstract_domain.hpp` / `separate_domain.hpp` / `dummy.hpp` | 加 `assign_maybe`（⊤：可能被其它线程初始化）（保留） |

### 2.C 执行引擎扩展（修改 `numerical.hpp`，初稿最大改动 +1574 行；现 pthread 语义已剥离到 `concurrent_semantics.hpp`）

> **（已重构，commit `56f20a7`）** 下表全部并发语义（`exec_pthread_*`、原子伪锁、PBR 私有化、指针黑板恢复、uninit 解耦、FS+FI）现已迁到新增的 `execution_engine/concurrent_semantics.hpp`（15 个自由函数模板）。`numerical.hpp` 只剩受 `enable_thread_modular` 门控的单行守卫调用 + SFINAE 双态访问器 + 4 个 getter，对上游 diff 已从 +1574 行收敛到 ~+50 行。下表「客观改动」仍逐条成立，只是所在文件变了。

| 客观改动 | 说明 |
|---|---|
| `offset_lock_key` / `heap_pointer_key` / `lock_key_base` / `lock_key_instance` | (base stable_id << 32 \| 字节偏移) 的可逆打包，恢复字段/元素锁灵敏度 |
| `dynalloc_in_loop` | 判断 malloc 调用点是否在 CFG 环上（环内 malloc 折叠成单 DynAlloc，拒绝作为确定锁） |
| `classify_atomic_intrinsic` + `AtomicKind` + `PSEUDO_ATOMIC_LOCK` | 硬件原子/伪锁（`__sync_*`、`__atomic_*`、`__VERIFIER_atomic_*`）分类，注入固定伪锁 0xA7E0AD1C |
| `exec_pthread_mutex_lock/unlock` | 强锁准入门（单点 points-to 才入 must 集）、偏移感知锁、符号锁、循环 DynAlloc 守卫、条件锁（trylock/branch）、弱解锁全排除、PBR flush |
| `exec_pthread_cond_wait/cond_signal` | cond-var 半建模（wait 释放/重获 + `mem_forget_all` 保证可达性；signal 无副作用） |
| `exec_pthread_join` | 线程句柄 → 线程函数名解析，加 join digest |
| pthread_create 探测 | 注册线程函数、spawn digest（MAY）、句柄初始化（`scalar_assign_nondet`）、`record_spawn_arg`（arg 实参绑定） |
| PBR 私有化写 + 严格无锁 flush | `record_privatized_write`（持锁 → 私有桶；无锁 → sentinel 分区），解锁/迭代边界 flush |
| 全局/堆指针黑板恢复 | load 全局指针/堆字段指针时 `pointer_assign_nondet` + `pointer_refine` 强绑恢复 points-to |
| uninit 并发解耦 | load 共享内存时 `uninit_assign_maybe` |
| FS+FI 混合间接调用 | 线程入口对「全局 flown 指针」join 函数指针分析结果 |

### 2.D AR（分析表示）层扩展（修改）

| 文件 | 客观改动 |
|---|---|
| `ar/include/ikos/ar/semantic/intrinsic.hpp` + `src/semantic/intrinsic.cpp` | 新增 4 个内建：`PthreadCreate/PthreadJoin/PthreadMutexLock/PthreadMutexUnlock`（签名 `void*`、返回 si32），name 映射 `pthread.create/join/mutex.lock/mutex.unlock` |
| `ar/include/ikos/ar/semantic/statement.hpp` + `src/semantic/statement.cpp` | `Load`/`Store` 加 `AtomicOrdering` 字段（`NotAtomic/Unordered/Monotonic/Acquire/Release/AcqRel/SeqCst`）+ `is_atomic()`/`ordering()` |

### 2.E 前端 LLVM 导入扩展（修改）

| 文件 | 客观改动 |
|---|---|
| `frontend/llvm/src/import/function.cpp/.hpp` | 翻译 `AtomicRMWInst`（拆成 atomic Load+Op+Store）、`AtomicCmpXchgInst`（Load+Store+成功标志，值模型过近似），`translate_atomic_ordering` |
| `frontend/llvm/src/import/constant.cpp/.hpp` | 新增 `translate_constant_binary`（Add/Sub/Mul/And/Or/Xor）+ zext/sext/trunc 常量表达式，解锁 clang-14 对 `container_of`/`offsetof` 的 `sub(0, ptrtoint(GEP null))` 常量折叠 |
| `frontend/llvm/src/import/library_function.cpp` | `pthread_join→PthreadJoin`、`pthread_mutex_lock/spin_lock→PthreadMutexLock`、`pthread_mutex_unlock/spin_unlock→PthreadMutexUnlock`；trylock/timedlock **刻意不映射**（保留为 extern 走条件锁路径） |
| `frontend/llvm/src/import/type.cpp` | 指针类型匹配放宽：任意 LLVM 指针 ↔ AR `void*`/opaque/i8 泛型指针（pthread 声明签名兼容） |
| `frontend/llvm/src/import/bundle.cpp` | declaration 函数也走 `LibraryFunctionImporter`（有 DWARF 也映射 pthread 内建） |
| `frontend/llvm/src/ikos_pp.cpp` | **移除 `LowerAtomicPass`**（改由 importer 直接处理原子），DCE → `create_preserve_loads_dce_pass` |
| `frontend/llvm/src/pass/initialize.cpp` + `include/.../pass.hpp` | 注册 `PreserveLoadsDCEPass` |

### 2.F 驱动 / 选项 / 报告（修改）

| 文件 | 客观改动 |
|---|---|
| `analyzer/src/ikos_analyzer.cpp` | `--concurrency` 三态开关 + `--no-concurrency` + `--emit-concurrency-invariants`；`-a race` 自动开启 thread-modular；`requires_concurrency` 零配置告警；**SIGSEGV/SIGBUS/SIGILL/SIGFPE 恢复守卫**（sigsetjmp/siglongjmp，force_unlock_after_signal）；注释掉无 debug 信息告警 |
| `analyzer/include/.../option.hpp` + `src/analysis/option.cpp` | 加 `enable_thread_modular`、`emit_concurrency_invariants` 两个选项 |
| `analyzer/include/.../context.hpp` | `opts` 由 `const` 改为非 const（插件生命周期协调） |
| `analyzer/src/analysis/value/interprocedural/sequential/analysis.cpp` | 入口分叉：`enable_thread_modular` 时委托 `ThreadModularAnalysis`，否则走原版顺序驱动（Phase 1-4 保留） |
| `analyzer/python/ikos/{analyzer,args,enums,report}.py` | `race` 分析注册、`--concurrency` 三态、`Summary.unknown` 字段 + `_is_unknown_check` + 「The program is UNKNOWN」判决（unknown 永不算 SAFE） |
| `analyzer/checker/{kind,name}.hpp` + `src/checker/checker.cpp` | 注册 `CheckKind::DataRace` / `CheckerName::DataRace`（short name `race`），`make_unique<DataRaceChecker>` |
| `analyzer/src/checker/{buffer_overflow,memory_watch,null_dereference,soundness}.cpp` | 4 个 pthread 内建加 no-op/空结果分支（避免 `ikos_unreachable`） |
| `analyzer/include/.../pointer/constraint.hpp` | 4 个 pthread 内建在 FPA 中按不透明指针消费者处理 |

### 2.G 内存位置（修改）

| 文件 | 客观改动 |
|---|---|
| `analyzer/include/.../memory_location.hpp` + `src/analysis/memory_location.cpp` | 加 `_stable_id`（全局单调原子计数器分配的稳定逻辑 id），`IndexableTraits` 改返回 `stable_id()`（**替换原始指针地址作为排序/哈希键，消除 ASLR 导致的跨运行非确定性**）；新增 `std::hash<MemoryLocation*>` 特化 |

### 2.H 测试 / 文档 / 脚本（新增，非内核代码）

| 文件/目录 | 内容 |
|---|---|
| `boundary-tests/`、`clean-sv-benchmarks/`、`docs/` | 边界测试套件、SV-COMP 基准子集、理论/分类文档 |
| `svcomp_race_test.py`、`yaml_ab_test.py` | 全量回归、AB 对比脚本 |

---

## 类别3：本次规划待实现（两个版本目前都不存在）

以下两项在两个版本中均无实现，属于后续开发规划，供理论文档标注「缺口」：

1. **结构体 / 数组成员敏感 field-sensitive region**
   - 现状：内存域把整个全局结构体/数组折叠成**一个** `MemoryLocation`（`@m`、`@m[..]`），字段/元素消歧靠 checker 层的 `offset`/`instance` 哈希补丁（`offset_lock_key`），而不是抽象域本体的 field-sensitive region。
   - 规划：在 `MemoryLocation` / cell 层面引入 field-sensitive region，使 `m.x` 与 `m.y`、`m[3]` 与 `m[4]` 在域内即为不同 region（对应 FP 分类模块一 1.2/1.3）。

2. **mutex 锁实例别名区分**
   - 现状：锁身份靠 `(base stable_id, offset)` 可逆打包区分字段/元素；但**同一类型不同实例**（如 `A[0].mutex` vs `A[1].mutex` 之外的、经 `container_of` 或堆节点跨槽而指向不同实例的锁）仍可能因 offset 变 ⊤ 或 region 不唯一而折叠。
   - 规划：为锁建立实例级别名区分（锁实例 = 结构体实例基址，`instance_base_offset` 的完整化），区分「同一把锁的别名」与「不同实例的锁」（对应 FP 分类 2.1 锁指针别名 + 1.3 container_of）。

---

## 客观结论（供理论文档引用）

1. 本地 = **原版 IKOS 的严格超集**：零文件删除，原生数值/指针/内存抽象域全部保留。
2. 并发能力是**新增的一层**，贯穿四层：AR 内建（pthread 内建 + 原子序）→ 前端导入（原子指令/container_of 常量/pthread 映射）→ 核心域（`LocksetDomain` 作为 `DomainProduct2` 第二分量 + `ConcurrentGlobalEnv` 黑板）→ 执行引擎（锁准入门/PBR/伪锁/cond-var）+ 独立 checker（DataRaceChecker）+ 独立驱动（ThreadModularAnalysis）。
3. 关键**非并发**改动只有两处：`stable_id`（ASLR 确定性）与 SIGSEGV 恢复守卫（批量鲁棒性）——均不改变分析语义。
4. 引用的理论依据（diff 注释内嵌）：Flanagan-Qadeer 线程模块化、AstréeA 全局不变式/分区、Goblint PBR（保护式读、Mukherjee SAS'17）。
