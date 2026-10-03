# SV-COMP 2027 规则页完整翻译 + 逐条讲解 + 流程总结 + 核验清单对照

> 原文：https://sv-comp.sosy-lab.org/2027/rules.php（抓取于 2026-09-29）
> 说明：页面顶部是赛事导航信息。第 16 届软件验证竞赛（SV-COMP 2027），隶属 TACAS 2027，2027 年 4 月于丹麦哥本哈根举行。
> 本文件按页面顺序全文翻译，每节末尾附「**讲解**」，最后给出参赛流程总结和核验清单逐条对照。

---

## 1. Witnesses（验证见证文件）

存在一套基于 YAML 的、用于交换验证结果的**见证格式（witness / exchange format；witness 指工具产出的、可被机器独立核验的验证证据文件，下文统一译为"见证"）**。目前支持该格式的 **2.0、2.1、2.2** 三个版本。SV-LIB 使用自己的见证格式（见该论文第 7 节）。

对于每个**基础类别（base category）**，下表规定了该类别中验证任务所产出的**正确性见证（correctness witness）**和**违例见证（violation witness）**分别支持哪些格式版本：

- 如果表中标注 "**not supported**"（不支持），则不要求提供见证（甚至不期望提供）。
- 如果标注 "**(demo mode)**"（演示模式），同样不要求见证；但如果工具产出了见证，组委会会用可用的验证器（validator）运行并分析它（**不影响得分**）。
- 其他情况下，**每一个验证结果都必须附带一个受支持格式的见证文件**。

| 语言 | 基础类别前缀 | 后缀 | 正确性见证 | 违例见证 |
|---|---|---|---|---|
| C | C.unreach-call. | Arrays、Heap | not supported | 2.0 或更高 |
| C | C.unreach-call. | Floats | 2.0 或更高（demo） | 2.0 或更高 |
| C | C.unreach-call. | Concurrency | 2.1 或更高 | **2.2** |
| C | C.unreach-call. | Huawei\* | 2.1 或更高（demo） | 2.2（demo） |
| C | C.unreach-call. | all others（其他全部） | 2.0 或更高 | 2.0 或更高 |
| C | C.valid-memsafety. | Concurrency、Huawei\* | not supported | 2.2#（demo） |
| C | C.valid-memsafety. | all others | not supported | 2.0 或更高# |
| C | C.valid-memcleanup. | all | not supported | not supported |
| C | C.no-overflow. | Concurrency | 2.1 或更高 | **2.2** |
| C | C.no-overflow. | Huawei\* | 2.1 或更高（demo） | 2.2（demo） |
| C | C.no-overflow. | all others | 2.0 或更高 | 2.0 或更高 |
| C | C.no-data-race. | all | not supported | **2.2** |
| C | C.termination. | all | 2.1 或更高 | 2.1 或更高 |
| Java | Java.valid-assert. | Main | not supported | ??（尚未定） |
| Java | Java.no-runtime-exception. | Main | not supported | ?? |
| SV-LIB | SV-LIB.correct-tags. | all | SV-LIB | SV-LIB | Python | Python.??. | all | ?? | ?? |
| MoXI | MoXI.??. | all | ?? | ?? |

> **#** 现有所有格式版本只支持 valid-memsafety 的子属性 **valid-deref** 和 **valid-free** 的违例；若被违反的子属性是 **valid-memtrack**，则没有受支持的见证格式。

每个见证必须写入文件 **witness.yml**（YAML 格式）或 **witness.svlibyml**（SV-LIB 见证）。见证会被交给见证验证器（witness validators）检查有效性。见证验证任务的资源限制为：**2 个处理单元、7 GB 内存**；时间上，违例见证 **90 秒**（即验证时间的 10%），正确性见证 **300 秒**（33%）。

**见证格式文档：**
- Witnesses 2.1 用户指南
- YAML Schemas 2.1
- 文献：Verification Witnesses 1.0（TOSEM 2022）、2.0（SPIN 2024）、Concurrency Witnesses（VMCAI 2025）、Contract Witnesses（arXiv 2025）、Non-termination Witnesses（ASE 2025）。

> **讲解：**
> - 见证是 SV-COMP 的核心机制：工具不能只喊"有 bug / 没 bug"，必须给出可被独立验证器复核的证据（错误路径 = 违例见证；不变式/证明 = 正确性见证）。
> - **与我们直接相关的一行：`C.no-data-race.` → 正确性见证 not supported，违例见证必须是格式 2.2。** 也就是说我们报 FALSE 时必须产出 2.2 版 witness.yml，而报 TRUE 不需要正确性见证（这与清单 C5 一致，且明确了版本号要求是 2.2，不能只给 2.0）。
> - 验证器只给 90 秒、2 核、7 GB——见证必须能被快速自动确认，过于复杂/不可解析的见证等于没有。
> - 并发相关的 unreach-call、no-overflow 同样要求 2.2，因为并发调度信息需要新格式。

---

## 2. Call for Participation — Procedure（参赛邀请与流程）

竞赛将从**有效性（effectiveness）和效率（efficiency）**两方面比较最先进的软件验证工具。竞赛分两个阶段：

- **训练阶段（training phase）**：把基准程序提供给工具开发者；
- **评估阶段（evaluation phase）**：在基准验证任务上运行所有参赛验证器，测量其解出的实例数量和运行时间。

竞赛由 TACAS 竞赛主席（Competition Chair）执行并展示。**本规则页是竞赛规则的权威参考**，同时应阅读 FAQ（含更多说明）。

> **讲解：** 两阶段制意味着你拿到的训练集就是评测集的超集，评测任务从训练任务中抽取——不能靠"见过题"之外的黑科技，重点是工具在已知语义任务上的真实能力。

---

## 3. Publication and Presentation of the Competition Candidates（参赛成果的出版与展示）

评审团（jury）选出合格的参赛候选，使其可以（在 TACAS 的 LNCS 论文集中）**发表一篇概述本参赛工具的投稿论文（contribution paper）**。竞赛组织者的一篇概述论文将描述竞赛流程并展示结果。论文投稿截止日期见日期页，投稿要求见提交页（submission page）。

此外，**每个合格的参赛候选都将获得一个 TACAS 报告时段**，向 TACAS 听众展示工具。

> **讲解：** 合格（qualified）不只是跑个分，还附带一篇 LNCS 短文和一次 TACAS 口头报告；短文有单独截止日（清单 F：2026-12-17）。

---

## 4. Definition of Verification Task（验证任务的定义）

一个**验证任务（verification task）**由一个 C 程序和一份规约（specification）组成。一次**验证运行（verification run）**是参赛候选在单个验证任务上的**非交互**执行，用以检验命题：*"该程序满足该规约"* 是否成立。

验证运行的**结果（result）是一个三元组 (ANSWER, WITNESS, TIME)**。ANSWER 为以下结果之一：

| 结果 | 含义 |
|---|---|
| **TRUE + Witness** | 规约被满足（即不存在违反规约的路径），并产出正确性见证。 |
| **FALSE + Witness** | 规约被违反（即存在一条违反规约的路径），并产出违例见证。 |
| **UNKNOWN** | 工具无法判定，或因崩溃、超时、内存不足而终止（即未能算出 TRUE 或 FALSE）。 |

**TIME** 是验证器终止前消耗的 CPU 时间，包含验证器启动的所有进程的 CPU 时间。若 TIME 达到或超过时间上限，验证器被终止，ANSWER 记为 "timeout"（按 UNKNOWN 处理）。

C 程序被划分为若干**类别（categories）**，由 category-set 文件定义；类别及所含程序见基准页（benchmark page）。

> **讲解：** ① 工具必须是非交互、全自动的批处理程序；② 计时包含子进程（IKOS 起的 llvm/apron 子进程都算）；③ 没有见证的 TRUE/FALSE 不被承认——三元组里 WITNESS 是必备组件（除非该类别标注 not supported）。

---

## 5. Properties（待验证属性）

### 5.1 规约文件与 LTL

对程序 "path/filename" 要验证的规约，要么放在同名文件 **"path/filename.prp"** 中，要么放在 **"Category.prp"** 中。

- 定义 `init(main())` 通过调用函数 `main` 给出程序的初始状态。若 main 带参数（C 标准允许），假定这些参数初值满足 C 标准规定的条件（C99 第 5.1.2.2.1 节"程序启动"）。
- 定义 `LTL(f)` 表示公式 f 在程序的每个初始状态成立。LTL 算子 `G f` 表示 f 全局成立。命题 `label(L)` 在到达 C 标签 L 时为真；命题 `call(func())` 在函数 func() 被调用时为真。

### 5.2 可接受的判定格式（Accepted Verdict Formats）

- 若验证运行检测到某属性被违反，结果中必须给出被违反的（可能是部分的）属性：**FALSE(p)**，其中 p ∈ {unreach-call, termination, no-overflow, valid-free, valid-deref, valid-memtrack, valid-memcleanup, no-data-race}，表示（部分）属性 p 被违反。
- 若验证运行检测到所有被检查属性均未被违反，结果中**不应**带属性：**TRUE**，表示输入满足所有属性。

### 5.3 各属性定义

**错误函数不可达（Unreachability of Error Function）：**
```
CHECK( init(main()), LTL(G ! call(reach_error())) )
```
函数 `reach_error` 在任何执行中都不被调用。

**内存安全（仅 MemorySafety 类别）：**
```
CHECK( init(main()), LTL(G valid-free) )
CHECK( init(main()), LTL(G valid-deref) )
CHECK( init(main()), LTL(G valid-memtrack) )
```
- `G valid-free`：所有内存释放都合法（反例：非法 free）。
- `G valid-deref`：所有指针解引用合法（反例：非法解引用）。
- `G valid-memtrack`：所有已分配内存块都被跟踪。被跟踪块集合定义为满足以下两条规则的最小集合：① 当某程序变量中存有指向该块（不必指向块开头）或指向该块之后第一个地址的指针时，该块被跟踪（变量可为指针类型或含指针的复合类型，可在全局/栈上、不必在当前作用域）；② 若某被跟踪块中的指针指向另一块，则被指向的块也被跟踪。泄漏的内存块不被跟踪，因此有内存泄漏的程序不满足该属性。

*约定：* MemorySafety 类别中，程序要么满足全部（部分）属性，要么恰好违反一个可从入口到达的（部分）属性 p。找到首次违例后路径上的进一步违例被忽略（首次违例后行为未定义）。

**内存清理（Memory Cleanup，仅 MemorySafety）：**
```
CHECK( init(main()), LTL(G valid-memcleanup) )
```
程序终止前所有已分配内存都被释放。在 valid-memtrack 基础上进一步要求：不存在"程序终止时仍指向已分配内存"的有限执行。（与 Valgrind 对比：即使 Valgrind 报告 "still reachable"，本属性仍可能被违反。）

**无溢出（No Overflow）：**
```
CHECK( init(main()), LTL(G ! overflow) )
```
绝不允许出现"某运算的结果类型为有符号整数类型、但结果值超出该类型可表示范围"的情况。违例对应 C11 定义的未定义行为。（因此，**向有符号整数类型的转换不违反本属性**。）

**无数据竞争（No Data Race，仅 ConcurrencySafety）：**
```
CHECK( init(main()), LTL(G ! data-race) )
```
若对同一内存位置存在两个或更多并发访问、且至少一个是写访问，则所有访问都必须是原子的。

**终止性（Termination，仅 Termination 类别）：**
```
CHECK( init(main()), LTL(F end) )
```
每条路径最终到达程序结尾。命题 "end" 在每次有限执行结束时为真（exit、abort、从 main 初始调用返回等）。反例是一次无限执行。

终止性是**活性（liveness）属性**，因此反例必须具有最大执行长度，即要么是终止的、要么是无限的；所有终止路径以命题 "end" 结束。对于 "FALSE + Error Path" 结果，错误路径必须是一条有限路径，其终止于一个在无限路径中被无限重复的位置。若存在周期性无限执行，可附加第二条路径：该路径从被无限重复的位置出发、再回到该位置，并与无限执行相对应。

> **讲解：**
> - 报 FALSE 必须带属性名，例如数据竞争报 `FALSE(no-data-race)`；报 TRUE 不带任何属性。这是 tool-info 模块 `determine_result()` 必须输出的格式（清单 C4 要核对）。
> - 我们目标属性 `no-data-race` 的语义：两个并发线程访问同一地址、至少一个写、且没有全部用原子操作/同步保护 = 违例。lockset + happens-before 算法的判定口径必须与此一致。
> - 注意 benchmark 里常见的错误函数是 `__VERIFIER_error()`/`reach_error()`，而我们类别不直接用它。

---

## 6. Benchmark Verification Tasks（基准验证任务）

训练验证任务将在指定日期于官网提供。**训练任务的一个子集将用于实际竞赛实验**；若不全取，由评审团商定子集的确定程序。组委会避免使用未经训练的任务，因为 C 语言语义是欠定义的（underspecified），需要参赛者、评审团和社区对任务预期含义达成共识（这与 SAT/SMT 等完全规定问题的竞赛不同）。

程序假定以 **GNU C** 编写（部分遵循 ANSI C）。每个程序是单文件，要么是经过预处理的 **.i** 文件，要么是可能未预处理的 **.c** 文件。验证器可凭扩展名区分。未预处理程序满足：

1. `#include` 只引入 C 标准库头文件或 **pthread.h**；
2. 不使用 `#define`；
3. 所有用到的宏都由 C 标准或 pthread.h 定义。

验证器可按程序架构用 `cpp -m32` 或 `cpp -m64` 预处理 .c 文件，无需额外 `-D` 宏或 `-I` 路径。注意**见证仍应引用未预处理的 .c 文件**（可借助 `#line` 指令实现）。每个程序包含验证所需全部代码，即所有非标准函数都有定义。

潜在参赛者可在指定日期前提交基准验证任务。任务须满足：(1) 程序以 GNU C 或 ANSI C 编写；(2) 带有上述属性之一给出的规约。其他规约也可能接受，但需提案讨论。

**新提议的类别只有在至少 3 个不同工具或团队参加时才会被纳入**（不能是同一工具换配置重复参加）。

每个类别会标明程序针对 **ILP32（32 位）** 还是 **LP64（64 位）** 架构。

### 基准中使用的约定函数

- **`__VERIFIER_error()`**：可假定实现为 `void __VERIFIER_error() { abort(); }`，即调用后永不返回、程序终止。
- **`__VERIFIER_assume(expression)`**：表达式求值为 0 时永久死循环，否则返回（无副作用）。实现：`void __VERIFIER_assume(int expression){ if(!expression){LOOP: goto LOOP;}; return; }`
- **`__VERIFIER_nondet_X()`**：返回指定类型的任意值，X ∈ {bool, char, int, int128, float, double, loff_t, long, longlong, pchar, pthread_t, sector_t, short, size_t, u32, uchar, uint, uint128, ulong, ulonglong, unsigned, ushort}，无副作用。模板：`X __VERIFIER_nondet_X(){ X val; return val; }`
- **`__VERIFIER_nondet_memory(void*, size_t)`**：用任意值初始化给定内存块。第一参数须指向大小匹配的有效内存块；通过此方式设置的指针值解引用会导致未定义行为，即指针值必须另行显式设置后才能解引用。
- **`__VERIFIER_atomic_*()`**：已弃用，但旧任务中仍有。建议改用 stdatomic.h 和 pthread.h 的标准特性（原子类型、原子 load/store、互斥锁等）。多线程环境中，`__VERIFIER_atomic_begin()` 与 `__VERIFIER_atomic_end()` 之间（或名字以 `__VERIFIER_atomic_` 开头的函数内）的语句被视为不可中断的原子执行；两个调用必须在同一控制流块内，不允许嵌套或交错。
- **`malloc()`、`free()`**：假定 malloc 和 alloca **总返回有效指针**（分配永不失败），free 总是释放内存并使该指针对后续解引用失效。

> **讲解：**
> - 只允许标准库和 pthread.h——数据竞争任务全部基于 pthread，我们的工具需完整建模 pthread_create / pthread_mutex_* 等。
> - 32/64 位架构按类别指定，这正是清单 G3 的 **data_model** 问题：witness.yml 头部的 `data_model: ILP32/LP64` 必须与任务架构匹配，否则 validator 直接拒。
> - malloc 永不失败、nondet 系列语义这些"建模约定"要写进分析的假设，否则会产生与标准答案不一致的判定结果。

---

## 7. Competition Environment and Requirements（竞赛环境与要求）

### 7.1 竞赛环境

每次验证运行在一台 **GNU/Linux（x86_64，最新 Ubuntu LTS）** 机器上启动，有三个资源限制：

- **内存：15 GB（14.6 GiB）RAM**；
- **运行时间：15 分钟 CPU 时间**；
- **CPU：4 个处理单元（processing units）**。

若验证器挂起（不再消耗 CPU 时间），则在 **15 分钟墙钟时间（wall time）后被杀掉**，运行时间记为 15 分钟。

> **讲解：** 这就是硬上限：15 GB 内存、15 分钟 CPU、最多 4 核。打包时 LLVM14/APRON 必须在该内存内跑得动；并行分析最多用 4 核才有意义。注意区分 CPU 时间与墙钟时间——挂起（死锁等不占 CPU）按墙钟 15 分钟杀。

### 7.2 Benchmark Definition（基准定义）

竞赛环境在每次运行时给候选传入参数。**存在一套全局参数**可用于把验证器调到适合基准程序的状态。**禁止验证器利用程序名、文件哈希或当前类别来调参**。这套命令行参数必须在竞赛投稿论文和基准定义（benchmark definition）中写明。其中一个参数指定规约文件；有些类别需要按 64 位架构验证，若验证器有处理此问题的参数，也需写明。

**参加 SV-COMP 必须在 SV-COMP 仓库中有一个 benchmark definition**。技术上需通过 **merge request（MR）** 把它集成到 SV-COMP 仓库的 **benchmark-defs** 目录下。基准定义规定验证器要在哪些类别上运行、传入哪些参数、资源限制是多少。

> **讲解：** 对应清单 D（category-structure.yml 的 MR）。"不许按程序名/类别调参"意味着：参加并发类别时不能写"如果是 no-data-race 就走某分支"的硬编码——但你们 C3 里"固定 no-data-race、不透传 propertyfile"的做法之所以可行，是因为该候选**只报名并发类别**，参数集本身是全局统一的；这一边界要在 benchmark definition 里讲清楚。

### 7.3 Tool-Info Module（工具信息模块）

**参加 SV-COMP 必须在 BenchExec 仓库中有一个 tool-info 模块**。技术上需通过 **pull request（PR）** 集成到 BenchExec 仓库的 **benchexec/tools** 目录下。

模块任务（之一）是把验证器输出翻译成 FALSE、TRUE 等结果。组织者运行验证器时遵循安装要求、依赖该模块正确执行验证器并正确解释其结果。**tool-info 模块必须经过测试**（参见工具集成说明）。

> **讲解：** 对应清单 C（ikos-conc.py）。`cmdline()` 决定怎么起工具、`determine_result()` 决定输出怎么判分，是自动化评测的唯一适配器，必须按 BenchExec 官方集成指南测试通过。

### 7.4 Tool（工具本身）

提交的系统必须满足提交页（submission page）上的要求。

注意：**一个验证工具可以作为不同的参赛候选多次参加**，前提是在描述中证明各候选在实现的概念或技术基础上有显著差异。这适用于不同版本和不同配置，以免开发者为每个新概念都新建工具。

> **讲解：** 对应清单 E3。我们基于 IKOS 加自研 thread-modular + lockset/HB，需要在描述里论证与上游 IKOS 及其他工具的概念差异，避免被判为"重复参赛"。

---

## 8. Requirements on Verifier Behavior（对验证器行为的要求）

验证器**不应使用验证任务中的标识符或注释**来对单个任务或任务组做指纹识别。**可以**利用标准库外部函数调用（如 `malloc`、`pthread_create`、算术函数等）的出现来检测任务使用了什么特性，并据此改变验证器行为。

竞赛组织者**有权对 C 代码做混淆（obfuscation）**：重命名局部标识符（外部函数和常见标识符白名单除外），增/改/删注释。

> **讲解：** 任何"靠文件名/变量名/特定注释猜答案"的做法都会被混淆击破，且属违规。工具只能依据程序结构和标准库调用特征做分析。这也意味着我们的见证必须靠 `#line`/结构信息定位，而不是靠花哨的标识符。

---

## 9. Evaluation by Scores and Runtime（按得分与运行时间评估）

得分按下表评定：

| 分值 | 报告结果 | 说明 |
|---|---|---|
| **0** | UNKNOWN | 未能算出结果、资源耗尽、程序崩溃。 |
| **+1** | FALSE 正确 | 找到程序中的错误，且违例见证被确认。 |
| **−16** | FALSE 错误 | 对满足规约的程序报错（误报/假警报，分析不完整）。 |
| **+2** | TRUE 正确 | 分析出程序无错误，且正确性见证被确认。 |
| **−32** | TRUE 错误 | 程序有错误但候选未发现（漏报，分析不健全/不可靠，unsound）。 |

TRUE 类结果的绝对分值高于 FALSE 类，因为一般认为发现错误比证明正确性"更容易"。错误结果的绝对分值高于正确结果，这样**一个正确答案抵不过一个错误答案**（一个随机给答案的假想工具应得到负总分）。

参赛候选按**总分**排名；总分相同则按 **success-runtime（成功运行时间）** 次级排名——即候选报告正确结果的所有基准上的 CPU 时间总和。参赛者有机会把验证结果与自己的预期结果核对，并与竞赛主席讨论不一致之处。

> **讲解：**
> - **清单 G1 得到官方证实：误报（FALSE incorrect）= −16，漏报（TRUE incorrect）= −32。** 一个误报要 16 个正确的 FALSE 才能补回；一个漏报要 16 个正确的 TRUE 才能补回。这对数据竞争工具是关键：lockset 分析若误报率高，会被重罚——**宁可保守报 UNKNOWN（0 分），也不要乱报 FALSE（−16）**。
> - 排名先看总分、再看正确答案的累计 CPU 时间，所以快也是竞争力。

---

## 10. Opting-out from Categories（退出类别）

每个团队可对每个类别（包括由其他类别任务组成的**元类别 meta categories**）提交**退出声明（opt-out）**。结果表中该类别记为破折号，不报告任何执行结果。若团队参加（即不退出），则该类别下**所有**验证任务都会运行。元类别得分按既定程序加权。（这意味着工具可以参加元类别、同时退出某个子类别：其真实结果计入元类别，但不在子类别中报告。）

> **讲解：** 对应清单 D2。如果只想跑 `C.no-data-race.Concurrency`，而不想被并发类下的 unreach-call、no-overflow 等子类别拖分，就必须显式 opt-out；默认是全部参加。这是控制"罚分暴露面"的重要手段，务必在 MR 中明确。

---

## 11. Computation of Scores for Meta Categories（元类别得分计算）

元类别由若干子类别组成，其得分由子类别中的**归一化得分（normalized scores）**计算。

### 11.1 程序

对包含 k 个（子）类别的元类别，按所含验证任务数归一化：验证器在类别 i 的归一化得分 **sn_i = s_i / n_i**（s_i 为该类别得分，n_i 为任务数）；然后求 sn_1..sn_k 之和 st = Σ sn_i，**再乘以各类别平均任务数**。

### 11.2 动机

降低大类别中单个任务相对于小类别中单个任务的影响，从而平衡各类别。不能按分数归一化，因为 TRUE 正分更高、错误结果负分更高（按分数归一化会抹掉这些有意设计的差异）。

### 11.3 示例

类别 1 有 10 个任务、答案为 TRUE；类别 2 有 10 个任务、答案为 FALSE。

- 验证器 A：类别 1 对 5 个 → 10 分；类别 2 对 5 个 → 5 分。总分 = ((10/10)+(5/10)) × 20/2 = 1.5 × 10 = **15**。
- 验证器 B：类别 1 对 10 个 → 20 分；类别 2 对 0 个 → 0 分。总分 = (2+0) × 10 = **20**。
- 验证器 C：类别 1 对 0 个 → 0；类别 2 对 10 个 → 10 分。总分 = (0+1) × 10 = **10**。
- 验证器 D：类别 1 对 8 个 → 16；类别 2 对 8 个 → 8。总分 = (1.6+0.8) × 10 = **24**。

排名：D（24）> B（20）> A（15）> C（10）。D 在所有类别都强，赢得 Overall；B 虽在一个类别比 D 强，但另一类别太弱，只得第二。

> **讲解：** 元类别（如 Overall）里，每个子类别权重相等（按任务数归一化），小类别和大类别贡献相同量级。偏科工具在 Overall 吃亏；但我们若只参加并发类别，主要影响的是并发元类别内部的汇总。

---

# 12. 我们要走的完整流程（总结）

按规则页 + 核验清单，参赛流程串起来如下：

1. **工具开发与打包（清单 A）**
   - 产出自包含归档 `ikos-conc.zip`：单一顶层目录、LICENSE、README、`smoketest.sh`（exit 0）、打包 APRON + LLVM14 全部依赖、可重定位、断网可跑、输出 ≤2MB、支持 `--version`、无 `.git`/`__MACOSX`/源码残留。
2. **BenchExec tool-info 模块（清单 C）**
   - 向 BenchExec 仓库提 PR：`benchexec/tools/ikos-conc.py`，实现 executable/version/cmdline/determine_result；FALSE 时产出 **witness.yml（格式必须 2.2，no-data-race 类别的硬性要求）**。
3. **Benchmark definition（清单 D）**
   - 向 SV-COMP 仓库 benchmark-defs 提 MR：把 `ikos-conc` 加入 `C.Concurrency` / `C.no-data-race.Concurrency` 的 verifiers；通过 opt-out 明确是否只参加数据竞争子类别；声明统一命令行参数（不能按任务/类别调参）。
4. **FM-Tools 条目（清单 B、E）**
   - 新建 `data/ikos-conc.yml`：唯一 id、version 字段填 **Zenodo DOI**、描述、贡献型 jury member（真实开发者身份）、标签（不用 ai/meta_tool）、toolinfo 模块指向、Ubuntu 包/容器镜像需求。
5. **Zenodo 归档（A/B 联动）**
   - 上传 zip 到 Zenodo 取得 DOI，回填 FM-Tools 的 version 字段；确认仓库公开（G5 待核）。
6. **合格性审查（清单 E）**
   - flags 正确、LICENSE 合格、与其他工具概念差异、有贡献 jury member——四条决定是否算"合格参赛"。
7. **卡关键日期（清单 F）**
   - 2026-10-08 注册 + 任务提交（B/C/D 三个 MR）；10-20 工具提交且 CI 全绿（DOI 定稿）；11-03 任务冻结；11-10 合格通知；11-17 最终重提交；12-01 离线评测完成；12-04 结果公开；12-17 参赛短文。

---

# 13. 核验清单对照（依据 rules.php 复核）

## 13.1 规则页可以直接证实/修正的条目

| 清单项 | 对照结论 |
|---|---|
| A6 stdout/stderr ≤ 2MB | 规则页未写此数字（属提交页/CI 要求），保留并以提交页为准 |
| C5 FALSE 产出 witness.yml | **规则页证实且加强**：`C.no-data-race` 违例见证**必须为 2.2 格式**（不是 2.0）；正确性见证 not supported，TRUE 无需见证 |
| C3 cmdline 固定 no-data-race | 规则页允许"全局统一参数集"，但禁止按程序名/类别/哈希调参；因只报名并发类别而固定属性文件，需在 benchmark definition 论文中写明，**属于边界操作，建议 MR 描述里明确论证** |
| D1 加入 C.Concurrency | 规则页证实：benchmark definition 必须经 MR 进 benchmark-defs |
| D2 opt-out | 规则页证实：不 opt-out 就跑该类别**全部**任务，罚分全暴露；只做数据竞争就必须显式退出并发下其他子类 |
| E3 概念差异 | 规则页证实：需在描述中论证概念/技术基础显著差异，否则视为同一工具重复参赛 |
| **G1 罚分 −16** | **规则页第 9 节明确：FALSE 错误 = −16，TRUE 错误 = −32，UNKNOWN = 0**。建议：拿不准的竞争优先报 UNKNOWN，避免 −16 |
| 环境限制 | 规则页明确：15 GB（14.6 GiB）内存、15 min CPU、4 处理单元；挂起按 15 min 墙钟杀 |
| 见证验证资源 | 2 处理单元、7 GB、违例 90 s、正确性 300 s |
| G3 data_model | 规则页明确类别按 ILP32/LP64 区分，witness 须匹配；G3 仍是真实风险，**必须逐任务确认架构，不能默认 LP64** |
| G5 工具须公开可访问 | **rules.php 本页未出现该原文**（"publicly available on the internet" 应在 submission 页）。需到提交材料页核实；在此之前仍按"仓库必须 public"准备 |

## 13.2 当前状态判定（依据清单自身标注 + 规则页）

- **已具备、且与规则一致**：A1–A10（✅）、C1–C5（✅，但 C5 需确认见证是 2.2）、E1–E3、B1/B3/B5/B6。
- **卡点未完成（影响 10-08 / 10-20 节点）**：
  - **B2 / E4（B4）：Zenodo DOI 与贡献型 jury member 身份**——合格性硬条件，必须在 10-20 前补齐。
  - **B7：Ubuntu 包/容器镜像清单**——需在 ubuntu:24.04 容器实际跑 smoketest（=G2）。
  - **D1/D2：benchmark-defs MR 尚未提交**——10-08 截止，是当前最紧迫动作。
- **技术风险（规则页证实存在但离线未验证）**：
  - **G3 data_model**：no-data-race 并发任务若为 ILP32，默认 LP64 的见证会被 validator 拒；
  - **G4 真实 witness validator 未跑**：规则要求见证被独立 validator 确认才计分（+1 前提），只过 linter 不够，必须在 clang/pycparser 环境用官方 validator 实测；
  - **G2 容器依赖**：APRON/LLVM14 在干净 ubuntu:24.04 容器能否跑通未验证。
- **结论**：规则符合性的**框架已满足**（归档结构、tool-info、类别机制、打分口径均已对齐），但**尚不能判定为"可提交"**——阻断项是 D 类 MR（10-08）、DOI + jury member（10-20）、以及 G2/G3/G4 三项环境与见证验证。建议下一步先过提交材料页（submission）核对 G5 与归档/CI 的具体要求，再逐一封锁 G2–G4。
