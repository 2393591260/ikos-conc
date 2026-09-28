# IKOS 架构调研 + 原生版 vs 并发修改版对比

> 基准：`/home/ruan/ikos-original`（原生）　目标：`/home/ruan/ikos`（并发修改）
> 结论先行：并发扩展**大体遵循了 IKOS 的插件式扩展范式**（新增 intrinsic、新增抽象域、新增 checker）。侵入内核的部分为：锁集域承载在 `DomainProduct2` 顶层第二分量（`first()`=数据域、`second()`=锁集域，锁集不再走 `memory/scalar` forwarder）；pthread/并发 transfer 语义集中在独立模块 `execution_engine/concurrent_semantics.hpp`（`numerical.hpp` 只留门控守卫 + SFINAE 双态访问器）；全局干扰黑板 `ConcurrentGlobalEnv` 由 `analyzer::Context` 持有（非单例）。此外 `memory/scalar` 仍有 `uninit_assign_maybe` 轻量 forwarder、`memory_location` 加 `stable_id`、前端翻译 C11 原子——完整清单见第三部分。

---

## 第一部分：原生 IKOS 目录 & 模块说明

IKOS 是模块化设计，一条清晰的**静态分析流水线**：

```
C/C++ 源码 →[clang]→ LLVM .bc →[ikos-pp 优化]→ .bc →[ikos-import 翻译]→ AR(中间表示) →[analyzer 不动点]→ 抽象域结果 →[checker 检查]→ 报告/数据库
```

### 1. `ar/` —— 【IR 与前端】的「中间表示（AR）」

- **重要子目录**：`include/ikos/ar/semantic/`（语句与值）、`src/format/`（打印）、`src/verify/`（合法性校验）、`src/pass/`（AR 自身的 pass）
- **文件类型**：`.hpp`（头）+ `.cpp`（实现）
- **流水线位置**：接收 LLVM 位码，产出「AR」——一个**语言无关的程序中间表示**（类似 LLVM IR 但更抽象），是后端分析唯一的输入
- **核心类**：
  - `ar::Statement`（`statement.hpp`）—— 一条语句（Load/Store/Call/Comparison/ReturnValue…），是 CFG 的基本节点
  - `ar::Value`（`value.hpp`）—— 一个值（全局变量/局部变量/内部变量/常量），语句的操作数
  - `ar::Intrinsic`（`intrinsic.hpp`）—— 「内建函数」（assert、assume、memcpy、`__VERIFIER_nondet`…），把某些库调用/内建操作建模成带语义的特殊调用

### 2. `core/` —— 【核心抽象解释内核】

- **重要子目录**：`include/ikos/core/domain/`（抽象域全家桶）、`include/ikos/core/fixpoint/`（不动点迭代器）、`include/ikos/core/semantic/`（变量/内存位置等语义对象）、`include/ikos/core/number/`（任意精度数）
- **文件类型**：几乎全是 `.hpp`（模板/header-only，抽象域都是 C++ 模板）
- **流水线位置**：整条流水线的**数学引擎**——抽象域（表示程序状态）+ 不动点迭代（在 CFG 上反复算到收敛）
- **核心类**：
  - `AbstractDomain`（`domain/abstract_domain.hpp`）—— 所有抽象域的**基类接口**，定义 join（并）/meet（交）/widen（放大）/narrow（收窄）/leq（偏序）等格运算
  - 具体抽象域：`machine_int/`（整数区间/同余/DBM）、`nullity/`（空指针）、`uninitialized/`（未初始化）、`pointer/`（指向关系）、`memory/`（内存模型）、`exception/`（异常）、`numeric/`（数值）、`scalar/`（标量组合）
  - `FwdFixpointIterator`（`fixpoint/fwd_fixpoint_iterator.hpp`）—— 前向不动点迭代器（工作队列算法，反复传播状态直到不再变化）
  - 组合器：`domain_product.hpp`（笛卡尔积组合多个域）、`separate_domain.hpp`（分离域）

> ⚠️ 重要澄清：`fixpoint/concurrent_fwd_fixpoint_iterator.hpp` 里的 **「concurrent」是「交错调度的不动点」**（Sung Kook Kim 的算法，让多个函数/节点交错分析以加速收敛），**不是多线程竞态**。这是命名巧合，别混淆。

### 3. `frontend/llvm/` —— 【IR 与前端】

- **重要子目录**：`src/import/`（翻译器）、`src/pass/`（ikos-pp 的优化 pass）、`src/ikos_pp.cpp`、`src/ikos_import.cpp`
- **文件类型**：`.cpp` + `.hpp`（依赖 LLVM 库）
- **流水线位置**：最前端——把 C 源码编译成 `.bc`，再翻译成 AR
- **核心类**：
  - `FunctionImporter`（`src/import/function.cpp`）—— 把一个 LLVM 函数翻译成一个 AR 函数体（`translate_call`/`translate_load`/`translate_store`…）
  - `LibraryFunctionImporter`（`src/import/library_function.cpp`）—— 把 `malloc`/`pthread_*`/`__ikos_*` 等**库函数名**映射成 `ar::Intrinsic`
  - `ikos-pp` 的 pass（`src/pass/`）—— mem2reg、lower-switch、去常量表达式等**规整化 pass**（这是后来加 freeze/保留 load 的地方）

### 4. `analyzer/` —— 【核心抽象解释内核】的分析驱动层

- **重要子目录**：
  - `include/ikos/analyzer/analysis/execution_engine/` —— **转移函数引擎**（`numerical.hpp` 是核心）
  - `include/ikos/analyzer/analysis/value/` —— 函数级/过程间分析调度
  - `src/analysis/value/intraprocedural/`（`sequential` / `concurrent`）—— 函数内不动点
  - `src/analysis/value/interprocedural/` —— 过程间分析
  - `include/ikos/analyzer/checker/` —— 各种**属性检查器**
  - `python/ikos/` —— `ikos` 命令行入口（CLI 包装脚本）
- **文件类型**：`.hpp`（transfer 引擎是模板）+ `.cpp` + `.py`
- **流水线位置**：把 AR + 抽象域 + 不动点**串起来**，执行分析、跑检查器、出结果
- **核心类**：
  - `NumericalExecutionEngine`（`execution_engine/numerical.hpp`）—— **转移函数**：对每条 AR 语句，定义「怎么更新抽象状态」（`exec(ar::Load)`/`exec(ar::Store)`/`exec(ar::Call)`…）
  - `Checker`（`checker/checker.hpp`）—— 检查器基类，在不动点收敛后，对每个程序点检查某个属性
  - `Context`（`analysis/context.hpp`）—— 全局上下文（变量工厂、内存位置工厂、选项…）

### 5. 其它顶层目录 —— 【外围辅助工具】/【测试】

- `script/` —— 构建/发布脚本【外围】
- `cmake/` —— CMake 构建模块【外围】
- `doc/`、`core/doc/`、`ar/doc/` —— Doxygen 文档【外围】
- `test/`、`core/test/unit/`、`analyzer/test/regression/` —— 回归测试【测试】
- `core/include/ikos/core/example/` —— 抽象域示例【外围】
- `core/include/ikos/core/adt/`、`support/`、`literal.hpp`、`linear_expression/constraint.hpp` —— 基础数据结构（线性表达式/约束、字面量）【核心辅助】

---

## 第二部分：原生 IKOS 的编程范式 & 扩展惯例

### 1. 新增功能怎么组织代码

**典型做法是「插件式」——新增独立文件/独立类，挂到既有框架的钩子上，几乎不改内核：**

- **加一种分析** → 新建一个 `Checker` 子类（如 `null_dereference.hpp`），在 `checker/name.hpp`+`kind.hpp` 里注册名字和枚举
- **加一种抽象域** → 新建一个 `core/domain/xxx/`，继承 `AbstractDomain`，实现格运算接口
- **加一种「内建调用」语义** → 在 `ar::Intrinsic` 枚举里加一个 ID + 在 `library_function.cpp` 里按名字映射，然后在 transfer 引擎里加一个 `exec_xxx` 处理它

### 2. 三类扩展的具体模式

| 扩展点 | 原生模式 | 关键文件 |
|---|---|---|
| **IR 语句** | 不加新 `Statement` 子类，而是加 `ar::Intrinsic`（内建调用），语句仍是 `ar::Call` | `intrinsic.hpp` + `library_function.cpp` |
| **抽象域** | 继承 `AbstractDomain`，通过 `domain_product` 组合进状态；memory 域通过 `polymorphic_domain` 的多态分发 | `domain/xxx/` + `domain_product.hpp` |
| **transfer 语义** | 在 `NumericalExecutionEngine` 里加 `exec(ar::XXX*)` 重载 / `exec_intrinsic_call` 的 case 分支 | `execution_engine/numerical.hpp` |

### 3. 命名空间 / 头文件 / 继承惯例

- **命名空间**：`ikos::core`（内核）、`ikos::ar`（IR）、`ikos::frontend::import`（前端）、`ikos::analyzer`（驱动）
- **头文件**：抽象域是 **header-only 模板**（`core/` 全 `.hpp`）；前端/驱动是 `.cpp`+`.hpp`
- **继承**：抽象域 `class XxxDomain : public AbstractDomain<...>`；检查器 `class XxxChecker : public Checker`；迭代器 `class XxxFixpointIterator : public FwdFixpointIterator<...>`
- **CRTP / 模板组合**：memory 域用 `polymorphic_domain.hpp` 做「类型擦除 + 多态派生」，加新域要在这里和 `composite` 里加 forwarder（这是**原生**加域的套路；并发锁集域已改走 `DomainProduct2` 第二分量，不再走这条路）

---

## 第三部分：并发修改版改动清单（严格基于 `diff -r` 与原生对比）

> 对比基准：`/home/ruan/ikos-original`（原生）。下面只列**实际有 diff 的文件**；未列出的 = 与原生逐字节相同。

### A. AR 层（`ar/`）

| 文件 | 改动 |
|---|---|
| `ar/semantic/intrinsic.hpp/.cpp` | 新增 4 个 pthread 内建：`PthreadCreate/PthreadJoin/PthreadMutexLock/PthreadMutexUnlock`（签名 `void*`、返回 si32） |
| `ar/semantic/statement.hpp/.cpp` | `Load`/`Store` 加 `AtomicOrdering` 字段（`NotAtomic/Unordered/Monotonic/Acquire/Release/AcqRel/SeqCst`）+ `is_atomic()`/`ordering()` |
| `ar/semantic/value.hpp/.cpp` | `GlobalVariable` 加 `is_thread_local` 标志（`__thread`/`_Thread_local` 线程私有全局，访问永不跨线程竞争） |

### B. core 抽象域（`core/domain/`）

| 文件 | 改动 |
|---|---|
| `lockset/lockset_domain.hpp`（新增） | `LocksetDomain`：must 锁集、读锁集、条件锁、spawned/joined 线程 digest |
| `concurrent_global_env.hpp`（新增） | `ConcurrentGlobalEnv`：全局干扰黑板（扁平黑板 + 锁分区 + 指针黑板 + 线程发现表），由 `analyzer::Context` 持有 |
| `domain_product.hpp` | `DomainProduct2` 加 `FirstDomain/SecondDomain` 类型别名 + `widening_threshold` |
| `memory/{abstract_domain,dummy,partitioning,polymorphic_domain,value}.hpp` | 加 `uninit_assign_maybe`（共享内存读的未初始化「可能已初始化」解耦，forwarder 贯穿 memory/scalar） |
| `scalar/{abstract_domain,composite,dummy,machine_int}.hpp` | 同上；`composite.hpp` 的 ptrtoint 在 width/sign 不匹配时改 `Cast`（原 `SignCast`） |
| `uninitialized/{abstract_domain,dummy,separate_domain}.hpp` | 加 `assign_maybe` |

### C. analyzer 执行引擎 + 调度

| 文件 | 改动 |
|---|---|
| `value/abstract_domain.hpp/.cpp` | 顶层 `AbstractDomain = DomainProduct2<ExceptionDomain<MemoryDomain>, LocksetDomain>`（`first()`=数据、`second()`=锁集） |
| `execution_engine/numerical.hpp` | SFINAE 双态访问器（`data()/lockset()/data_of()`）+ `enable_thread_modular` 门控守卫 + 4 个 getter（`ctx/lit_factory/var_factory/data_layout/call_context`） |
| `execution_engine/concurrent_semantics.hpp`（新增） | pthread/并发 transfer 语义（15 个自由函数模板：`exec_pthread_*`、`on_load/on_store/on_comparison_*`、原子伪锁、PBR 私有化、`exec_pthread_create`） |
| `execution_engine/symbolic_index.hpp`（新增） | 符号数组下标提取（`symbolic_index_of`/`pointershift_base`） |
| `execution_engine/inliner.hpp` / `concurrent_inliner.hpp` / `context_insensitive.hpp` | FS+FI 混合间接调用、`post.second().set_to_bottom()`（DomainProduct2 双态初始化） |
| `value/interprocedural/sequential/analysis.cpp` | `Analysis::run()` 入口分叉：`enable_thread_modular` → `ThreadModularAnalysis`；顺序路径重构为 Phase 3 / Phase 4 |
| `value/interprocedural/init_invariant.cpp` | `.normal()` → `.first().normal()`（DomainProduct2 机械） |
| `value/thread_modular.hpp/.cpp`（新增） | `ThreadModularAnalysis`：全局 worklist 不动点（外层迭代到 `is_dirty()` 为假），每线程用 `interprocedural::sequential::FunctionFixpoint` 跑内层不动点 |
| `value/concurrency_scanner.hpp`（新增） | `requires_concurrency` 零配置探测 |

### D. 辅助 / 基础设施（改内核）

| 文件 | 改动 |
|---|---|
| `analysis/memory_location.hpp/.cpp` | 加 `stable_id`（全局单调原子计数器，替换裸指针地址作为排序/哈希键，消除 ASLR 非确定性） |
| `analysis/pointer/constraint.hpp` | 4 个 pthread 内建在 FPA 中按不透明指针消费者处理 |
| `analysis/option.hpp/.cpp` | 加 `enable_thread_modular`、`emit_concurrency_invariants` 选项 |
| `analysis/context.hpp` | `opts` 由 const 改非 const + 加 `concurrent_env` 成员（去单例化） |

### E. checker

| 文件 | 改动 |
|---|---|
| `checker/data_race.hpp/.cpp`（新增） | `DataRaceChecker`：析构期 O(N²) 两两配对，HB digest + 锁集交集判定竞态 |
| `checker/kind.hpp` / `name.hpp` / `checker.cpp` | 注册 `CheckKind::DataRace` / `CheckerName::DataRace` |
| 17 个 `checker/*.cpp`（assert_prover/buffer_overflow/division_by_zero/…） | (a) `.normal()` → `.first().normal()`（DomainProduct2 机械）；(b) 4 个 pthread 内建加 no-op/空结果分支（避免 `ikos_unreachable`） |

### F. 前端 LLVM 导入（改内核）

| 文件 | 改动 |
|---|---|
| `import/function.cpp/.hpp` | `AtomicRMWInst`/`AtomicCmpXchgInst` 翻译（拆成 atomic load/store，带 `AtomicOrdering`） |
| `import/constant.cpp/.hpp` | `container_of`/`offsetof` 常量折叠（`sub(0, ptrtoint(GEP null))` 的 zext/sext/trunc 常量表达式） |
| `import/type.cpp` | 指针类型匹配放宽（任意 LLVM 指针 ↔ AR `void*`/opaque/i8 泛型指针，pthread 声明签名兼容） |
| `import/bundle.cpp` | `is_thread_local` 标志透传 + declaration 函数也走 `LibraryFunctionImporter`（有 DWARF 也映射 pthread 内建） |
| `import/library_function.cpp` | `pthread_*` 名字 → `ar::Intrinsic::PthreadXxx` 映射 |
| `ikos_pp.cpp` | 移除 `LowerAtomicPass`（改由 importer 处理原子）；DCE → `create_preserve_loads_dce_pass` |
| `pass.hpp` / `pass/initialize.cpp` | 注册 `PreserveLoadsDCEPass` |
| `pass/freeze_uninit.cpp` / `pass/preserve_loads_dce.cpp`（新增） | 两个 ikos-pp pass（freeze undef、保留 load 的 DCE） |

### G. CLI / python

- `python/ikos/{analyzer,args,enums,report}.py` — `race` 分析注册、`--concurrency` 三态、freeze-undef 透传、`Summary.unknown` 字段

### H. 测试 / 文档（非内核）

- `boundary-tests/`、`clean-sv-benchmarks/`、`docs/`、回归脚本（`yaml_ab_test.py` 等）

---

## 第四部分：整体评估 & 风险清单

### 1. 是否对齐原生模式？

**大体对齐（约 7 成）**：intrinsic、抽象域、checker、CLI、测试都走原生「加文件/加类/注册」的插件路子。**偏离点**：锁集域承载在 `DomainProduct2` 正交产品的第二分量（数据域在 first、锁集域在 second）；pthread 语义集中在独立模块 `concurrent_semantics.hpp`（而非 `numerical.hpp` 内联）。

### 2. 低风险改动（插件式、纯新增文件）

- `lockset_domain.hpp`、`concurrent_global_env.hpp`（新增域/黑板）
- `concurrent_semantics.hpp`、`symbolic_index.hpp`（新增 transfer 模块）
- `thread_modular.hpp/.cpp`、`concurrency_scanner.hpp`（新增驱动）
- `data_race.hpp/.cpp`（新增 checker）
- `frontend/.../pass/freeze_uninit.cpp`、`preserve_loads_dce.cpp`（新增 pass）
- `checker/kind.hpp`/`name.hpp`/`checker.cpp`（注册）、python CLI、测试

### 3. 高风险改动（改内核、易引 bug / 破坏收敛）

1. **`execution_engine/numerical.hpp`** —— SFINAE 双态访问器（`data()/lockset()/data_of()`）+ 门控守卫 + 4 getter。虽然 pthread 业务逻辑已剥离，但引擎核心仍有 SFINAE 改动，且「带锁集的值分析」与「不带锁集的指针分析」两种实例化都要正确编译。
2. **`core/domain/{memory,scalar,uninitialized}/*` 的 `uninit_assign_maybe`/`assign_maybe` forwarder** —— 为「共享内存读可能被其他线程初始化」解耦，forwarder 仍贯穿 `scalar/composite → machine_int → dummy`，漏一层会静默 no-op（比原来的 5 层锁集 forwarder 短，但仍在）。
3. **`value/interprocedural/sequential/analysis.cpp`** —— `Analysis::run()` 入口分叉 + 顺序路径 Phase 3 / Phase 4 重构（原生是「每入口 fixpoint+checks」一个循环，fork 拆成两阶段并在 Phase 4 重新跑 fixpoint），顺序路径行为被重排。
4. **`value/thread_modular.cpp`（新增）** —— 全局 worklist 不动点，收敛性依赖 `is_dirty()`/`_global_iteration` 的加宽纪律，外层迭代逻辑是新增的、无原生参照。
5. **`frontend/.../function.cpp`** —— `AtomicRMWInst`/`AtomicCmpXchgInst` 翻译（动了翻译主路径的指令分发），cmpxchg 是过近似（总是 store 新值）。

### 4. 一句话总结：并发「零件」怎么拼进流水线

```
源码 →[clang]→ .bc →[ikos-pp: freeze_uninit/preserve_loads_dce]→ .bc
     →[frontend: library_function.cpp 把 pthread_* 映射成 ar::Intrinsic::PthreadXxx；function.cpp 翻译 C11 原子]→ AR
     →[analyzer: ThreadModularAnalysis 调度多个线程函数 + ConcurrentGlobalEnv 黑板]
     →[transfer: concurrent_semantics.hpp 的 exec_pthread_* / on_load/on_store 更新 LocksetDomain]
     →[fixpoint 收敛后: DataRaceChecker 离线两两配对访问 + 锁集交集判定竞态]
     → 报告
```

**一句话**：并发分析 = 「前端把 pthread 变 intrinsic + 翻译原子指令（A/F）→ 新增锁集域（`DomainProduct2` 第二分量）+ 黑板记线程/锁状态（B）→ transfer 的 pthread 语义集中在 `concurrent_semantics.hpp`（C）→ 线程模块化全局不动点调度（thread_modular）→ 竞态 checker 收尾」，拼法是对的。
