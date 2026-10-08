# IKOS BMC 文档与实现审查（2026-10-08）

审查对象：`docs/ikos-bmc-complete.md`，以及它依赖的前端、线程模块化分析、DataRaceChecker、BMC 和 witness 管线。

**结论：两层架构值得保留，但文档目前不能充当正确性论证。最紧迫的工作是修复已复现的 FALSE 误报和 TRUE 漏报，建立语义契约；扩大 BMC 能力应排在其后。**

本文把文档内的开发要求和“铁证”标签作为审查对象，没有将它们当作用户指令或数学证明。未修改原文和分析器源码。

## 1. 证据范围与可复现结果

- 项目 HEAD：`10beda2400504dddd9774f7b5f1948031e63a455`。审查过程中原文另有工作区更新；以下也核对了更新后的 SC 范围声明、相邻判定注释和 gate 统计待办。
- 实跑二进制：`install/bin/ikos-analyzer`，与 `build/analyzer/ikos-analyzer` 的 SHA-256 相同：`ef0dc5258eea3e96aca4d5798dfad9fe2663d8c0aaa449bf4d32946c42756eb7`。没有重新构建。
- 三个主要用例在 LP64、ILP32 下均复现；两个对照变体也做了检查。以下表格展示 ILP32 结果。
- Dartagnan：核对实际 jar 内 `git.properties`，为 4.3.1 / `7caef5c850dc2dbf81c5554dcf41e402007f7a19`。注意旁边 `target/classes/git.properties` 是旧版本，不能用它判断实际 jar。
- 下载并核对了该 commit 的 `PropertyEncoder.java`；阅读 FMCAD 2018、CAV 2019、OOPSLA 2022 论文的相关建模与优化章节。
- 没有重跑 SV-COMP 全量集，没有独立验证文档中的分数、TP 预测和 Dartagnan 全量错误数，也没有声称这是整个项目的完整 soundness 审计。

`BMC` 模式指 `--demote-race-to-unknown` 加 `IKOS_BMC_RECOVER=1`。`demote` 模式不启用 BMC。

| 用例 | 语义判断 | 模式 | 实际结果 | 含义 |
|---|---|---|---|---|
| exclusive-flag.c | 无竞争 | demote | potentially UNSAFE / UNKNOWN | 抽象候选没有被确认 |
| exclusive-flag.c | 无竞争 | BMC | definitely UNSAFE / FALSE | **BMC 误报** |
| published-before-init.c | 有竞争 | 默认、BMC | SAFE / TRUE | **上游漏报，BMC 没有候选可补救** |
| published-before-init-control.c | 有竞争 | 默认 | definitely UNSAFE / FALSE | 删除后续加锁写入后恢复报告 |
| published-before-init-control.c | 有竞争 | BMC | potentially UNSAFE / UNKNOWN | 当前 BMC 堆对象门阻止确认 |
| overlapping-widths.c | 有竞争 | 默认、BMC | SAFE / TRUE | **上游漏报，BMC 没有候选可补救** |
| overlapping-widths-control.c | 有竞争 | 默认、BMC | definitely UNSAFE / FALSE | 把字节偏移 1 改为 0 后恢复报告 |

用例、复跑脚本和机器可读结果保存在 [review-repros/bmc-2026-10-08/](review-repros/bmc-2026-10-08/)。从仓库根目录运行：

```bash
python3 docs/review-repros/bmc-2026-10-08/run.py
python3 docs/review-repros/bmc-2026-10-08/run.py --bits 64
```

结果中的 TRUE/FALSE 是 no-data-race 属性的判定。UNKNOWN 不等于错误 TRUE，也不等于找到了竞争。所有主要诊断运行正常退出。

## 2. 三个应先处理的问题

### 2.1 有限事件集合不等于真实执行的欠近似

`exclusive-flag.c` 的主线程先给 `selector` 写入一个任意整数，再创建两个线程。线程 A 在 `selector == 1` 时写 `x`，线程 B 在 `selector == 2` 时写 `x`。创建之后没有人修改 `selector`，所以两次读取得同一个值，两次写不可能同时执行；初始化写又发生在线程创建之前。

但是 BMC 报告了这两次写竞争。`encode.hpp:118` 起为 AR 变量建立自由整数；`encode.hpp:247` 起添加 Comparison。它没有把这两次 load 绑定到共同的初始写，也没有编码赋值、算术、实参和形参关系。于是两个读结果可以分别取 1 和 2，形成原程序没有的执行。

这直接否定原文 §2 的“BMC 是欠近似，所以 FP=0”作为当前实现的保证。有限展开限制了搜索范围，遗漏语义约束却可以增加虚假行为。两种近似方向同时存在，不能靠模块名称判断。

应建立两条独立契约：

1. SAFE 方向：所有具体执行都被抽象分析覆盖，而且每个过滤候选的规则都保守。
2. FALSE 方向：每个满足竞争公式的模型，都能构造成原程序在约定内存模型下的有限竞争执行。

推荐把 FALSE 公式的职责写为：初始化与线程启动上下文、控制流、局部数据流、共享内存一致性、同步语义、地址重叠、竞争属性。任何职责没有编码，也没有证明可以省略，就返回 Unsupported/UNKNOWN。

尤其需要注意：AI 对 A、B 的分支各自判“可能”，不能证明两条路径可以共同发生。把任意抽象不变式当作 BMC 可达初始状态也可能引入伪反例；不变式可以约束搜索，但确认仍需可达前缀或有效的执行重建。

### 2.2 “私有初始化”过滤缺少发布证明

`published-before-init.c` 中，主线程分配 `p` 并在创建两个线程之前把它放入全局变量。线程 A 先无锁写 `p->v`，随后持锁再写同一字段；线程 B 持该锁写同一字段。A 的第一笔写并未持锁，对象又已共享，因此它可以与 B 的写竞争。

源码 `data_race.cpp:670` 起，只根据“同一线程的无锁堆写之后，记录中还有对同一抽象对象的单锁访问”，设置 `private_init`。`data_race.cpp:813` 起再据此跳过与其他持同锁访问的配对。代码没有在这里证明对象在第一笔写时尚未逃逸，也没有证明实际发布发生在初始化之后。访问记录排列也不能直接代替 CFG 的必然先后关系。

对照变体只删除 A 后面的加锁写入，原有的竞争保留，报告却从 SAFE 恢复为 FALSE；这与上述过滤规则的触发条件相符。

建议要求对象私有性/逃逸证据、具体发布事件及发布前后顺序。不能证明时保留候选或返回 UNKNOWN。BMC 的保守 gate 无法补救已被上游删掉的候选。

### 2.3 起始偏移不相交不等于访问范围不相交

`overlapping-widths.c` 的 A 写整个 `unsigned int x`，B 写 `((unsigned char*)&x)[1]`。字符类型访问对象表示是允许的；两个访问的字节范围重叠。

`data_race.cpp:888` 却根据两个起始 offset 区间不相交跳过配对。`AccessRecord` 没有独立的访问宽度字段。这个用例被报 SAFE；把 `[1]` 改为 `[0]` 后恢复 FALSE。

应记录访问的字节长度或规范化的内存位置，并使用区间重叠：`offA < offB + sizeB && offB < offA + sizeA`。未知长度和溢出必须保守处理。BMC 目前仅检查起始地址相等，也需要与统一的冲突定义一致。

如果项目有意只支持同宽且按位置规范化的访问，需要明确支持范围；遇到范围外输入不能由这个过滤直接得出 SAFE。

## 3. 文档与源码之间的具体偏差

下表的“源码观察”不等于每一项都已构造反例；除第 2 节外，应按对应证明义务继续审计。

| 原文位置/说法 | 当前源码观察 | 建议修订 |
|---|---|---|
| §0/§2：两层流程是普遍工作方式 | `data_race.cpp:1173` 仅在 demote 选项和环境变量都启用时运行 BMC；默认分析与比赛包装器行为不同 | 列出默认、demote-only、BMC、比赛入口四种配置及判定含义 |
| §4/§6/§7：地址提取失败保守不确认 | `encode.hpp:273` 只在 base 非零且相等时加约束；其他情况没有提前返回，而是继续查询 SAT | 明确检查失败路径；使用显式 Known/Unknown 地址结果，避免省略必需约束 |
| §4/§7：锁身份提取失败保持 0、拒绝确认 | `unroll.hpp:167` 会回退到 Value 的裸指针身份，同一实际锁可能被拆成不同身份 | 未知身份必须显式表示；区分 must-same、must-different、may-alias |
| §7：same_global 门阻挡偏移 TOP | `data_race.cpp:1202` 的 var_index 可放行 TOP 偏移，另有结构偏移回退 | 准确列出放行条件及依赖的后端同址证明，不把全部 TOP 写成统一阻挡 |
| §5：helper 已内联 | `unroll.hpp:811` 的 callee_path 是副本；目标在 caller 后续代码时，callee 事件没有作为返回前缀合并回 caller；也没有参数/返回值绑定 | 区分“递归搜索 helper 内目标”与“完整调用语义”；支持 call/return 展开或受证明约束的摘要 |
| §5：跳过 helper/未知调用只丢 TP | 目标在 helper 内时可能找不到目标；目标在调用之后时，跳过调用的值流/同步也可能产生伪执行 | 不能把跳过调用统一解释为 UNKNOWN；没有语义摘要的相关调用应阻止确认 |
| §5：归纳变量仅认 v=v±const，完整展开 N 次 | `is_induction_var` 没有检查二元运算种类、常量步长、初值；`constant_loop_bound` 没有完整检查两比较使用同一变量及互补谓词 | 改成有条件的模式识别说明，补足证明条件或采用通用有界展开 |
| §5：循环条件只是 bookkeeping | `unroll.hpp:771` 在 skip_cmp 时省略 Comparison，迭代间也没有动态 SSA 重命名 | 保留路径语义，逐迭代版本化变量；删除条件需要独立证明 |
| §5：abort/exit 子串匹配，覆盖 ar. 前缀 | `unroll.hpp:755` 实际只精确匹配 libc.abort/libc.exit | 与 importer 的 Intrinsic ID 对齐，不维护两套名称猜测 |
| §5/§7：资源上限防爆炸 | `unroll.hpp:1028` 完成 DFS 后才检查路径数/长度；Z3 没有设置 timeout/rlimit | 将预算放入展开过程，增加每对及全任务预算；标注当前只是事后拒绝大公式 |
| §3/§7：双方持同一 held 锁才跳过 | `bmc.hpp:166` 起结合函数直接使用的锁集；双方各有任意 held 锁，且一方 held 锁被另一函数某处使用，就可能跳过 | 如实说明它是更粗的能力限制；helper 中锁也要纳入统一摘要 |
| §6：mutex 互斥编码 | `encode.hpp:88` 每把锁只保留最后一次 Lock 和最后一次 Unlock，没有逐次临界区配对 | 使用完整锁状态机和每次 acquisition/release；不把最后一对代表所有锁历史 |
| §6：join 末事件排序 | 当前每个 Join 都排序到另一条路径末尾，不检查句柄；事件提取又只检查 pthread_join 名称，没有覆盖 ar.pthread.join 名称 | 区分已识别 join 的过约束与识别缺失；按实例句柄匹配，统一 intrinsic 分类 |
| §8：原子过滤、循环、witness“等价” | 排除竞态对、禁用整程序 BMC、正确展开和提供 witness 是不同层面的性质 | 改成支持能力、语义条件、未支持范围、验证证据四列 |
| §10.1：不用 points-to | same_global 和上游候选过滤都依赖抽象 points-to | 改为“用于 may-alias/能力筛选，但不是具体执行中同址的证明” |
| §10.3：APRON 天然支持 double | `numerical.hpp:1476`/`:1611` 仍标 TODO；浮点运算赋 nondet、比较不做数值推理，BMC 浮点比较拒绝编码 | 区分“能接受浮点程序”与“具有 IEEE-754 推理”，删除当前优势证明 |

## 4. 通用性：没有文件名分支，也可能是语义补丁

在本次检查的 BMC、checker 和关键并发语义代码中，没有发现根据具体 benchmark 文件名分派结果的逻辑。注释中列出回归文件名是有用的溯源，不能据此认定硬编码。

但“规则适用于很多文件”不意味着它是正确的通用规则。第 2 节的 private_init 就是适用范围很广、语义前提却不足的例子。把注释改成“能力边界”也不会改变规则的证明义务。

建议将规则区分为三类：

- **语义规则**：由语言、POSIX 或明确 benchmark 约定定义，例如成功 join、互斥锁和 SV-COMP 的原子函数命名约定。需要与前端降低后的语义一致。
- **能力限制**：无法编码时拒绝确认，例如不支持浮点比较。应给出机器可读原因和明确的配置范围。
- **启发式优化**：anchor 顺序、切片种子、尝试顺序。可以决定先查哪个候选，但不能单独证明 SAFE 或确认 FALSE。

`function_has_atomic_section` 是能力限制，而不是“helper 含 begin/end 就代表无锁协议”的语义定理。线性化与数据竞争是不同性质；数据结构可线性化不能直接推出其 C 内存访问无竞争。

每条规则应有“语义前提、近似方向、失败后判定、最小正例、最小反例”。此外加入语义保持变换检查：重命名文件/变量、抽取或内联 helper、等价改写条件、增加重复调用、更换循环初值和步长。文件名不进入决策，并且变换后判定稳定，才是较有力的通用性证据。

## 5. Dartagnan 对照应补上什么

### 5.1 相邻不是一般的“无 HB”定理

固定版本 `PropertyEncoder.java:338` 的注释限定该编码适用于具有相应 hb 关系的 SC/SVCOMP 模型。`:389` 的条件明确包括 `hbEncoder.encode(m,w)`，不是一般意义的“两个访问之间没有 HB”。

CAT 中名为 hb 的关系含 rf/fr/co；它不能直接与 POSIX/C11 的 happens-before 混同。原文新增的“扩展 hb = po + com + mutex + assume”也需要修正：assume 是执行/值约束，不是自然的事件二元排序关系；Dartagnan 的同步还依赖程序降低与完整执行编码。

不可比元素可以在线性扩展中相邻，只给出一个方向。可比较且没有中间元素的两个事件也可以相邻，不能反推不可比。原文应描述“在指定程序事件、内存模型和同步降低下的专用归约”，并列出所需前提，而不是引用一个偏序结论作为充要条件。

对 IKOS 应证明：抽取/删除事件、路径截断、调用处理和锁建模之后，SAT 的相邻事件仍对应原程序的竞争。复制 clock 差 1 不会自动继承 Dartagnan 的正确性。

### 5.2 两路径局部 SAT 必须能扩展到全程序执行

两条子线程路径缺少主线程初始化、pthread_create 实参、其他线程写入以及中间同步参与者。本次 selector 反例甚至不需要复杂同步：第三个线程 main 的初始化已经足以使局部 SAT 不可扩展。

因此“固定两条路径”可以保留为查询起点，但不宜作为无需证明的永久语义边界。可以按数据依赖、地址依赖、控制依赖和同步依赖增加相关事件/线程；预算耗尽时返回 UNKNOWN。必须证明被省略的部分与见证无关，或能够重建兼容的全程序前缀。

### 5.3 rf/co 不能只加在目标地址

原文 §9 只对目标地址建立写顺序与读值选择，会遗漏决定目标可达性的 selector、标志、句柄、指针和数组下标。本次 FP 的依赖地址是 selector，目标地址是 x。

最低要求包括初始化写、每个已执行读的合法来源、读写值关系、co 的同址写顺序、fr 与所选 SC 模型的一致性，以及遗漏线程可能贡献的写入。SC 下只让读选一个同值写还不够；还需要防止选择被已发生写覆盖的旧写。

建议使用“候选驱动的依赖闭包”，不是“只编码目标地址”。同一状态下的共同可行性也应通过这个闭包表达。

### 5.4 BMC 不具备无界程序的一般完备性

文档将“能证 SAFE 和 RACE”解释为完备性，并以此解释 Dartagnan 速度，概念不准确。Dartagnan 同样受展开界限和资源限制；无界循环没完全处理且没有找到反例时仍可能 UNKNOWN。合法的有限反例也不要求把所有循环完全展开。

“公式指数爆炸”需要区分路径/调度数、公式大小和求解最坏复杂度；事件对枚举本身主要产生二次规模，不等于必然显式构造指数大小公式。Dartagnan 默认 CAAT 也不能概括为单一 monolithic SMT。

删除“最后约 35 个数学上追不到”这样的断言。合理的表述是“当前能力与预算下未覆盖，后续通过消融实验评估”。性能和有限测试覆盖数无法推出某个具体 benchmark 集的不可达上界。

## 6. 应参考的论文，以及它们对 IKOS 的实际价值

这里应把每篇论文绑定到具体设计决策，而不是在末尾堆引用。

| 文献 | 直接价值 | 应落到本文的哪一部分 |
|---|---|---|
| Biere, Cimatti, Clarke, Zhu, *Symbolic Model Checking without BDDs*, TACAS 1999，[DOI](https://doi.org/10.1007/3-540-49059-0_14) | 基础 BMC 与转移关系展开；有限界限不替代语义正确性 | 明确 SAT 反例正确性与有界 UNSAT 的范围；本次未阅读全文 |
| Ponce de León et al., *BMC with Memory Models as Modules*, FMCAD 2018，[论文](https://hernanponcedeleon.github.io/pdfs/fmcad2018.pdf) | §III 明确控制流、数据流、内存模型、性质四部分；展开后 SSA；所有已执行的数据运算与 guard 需成立 | 重写当前 soundness 小节与事件/值状态接口；本次读了相关章节 |
| Gavrilenko et al., *BMC for Weak Memory Models: Relation Analysis for Compact SMT Encodings*, CAV 2019，[论文](https://hernanponcedeleon.github.io/pdfs/cav2019.pdf) | §3 的 may pairs 与 active pairs，将静态分析用于缩小关系编码，保留所需语义 | IKOS 的 points-to/MHP/不变式用于 sound 的编码裁剪；比盲目限制两线程更贴合本项目 |
| Haas, Meyer, Ponce de León, *CAAT: Consistency as a Theory*, OOPSLA 2022，[论文](https://hernanponcedeleon.github.io/pdfs/oopsla2022.pdf) | 控制/数据流与一致性分离，使用惰性理论求解；推翻“全程序就只能单体大公式”的二分 | 后期研究按需一致性检查；不能只仿照循环而省掉数据流，也不建议立即移植整个 CAAT |
| Alglave, Kroening, Tautschnig, *Partial Orders for Efficient Bounded Model Checking of Concurrent Software*, CAV 2013，[DOI](https://doi.org/10.1007/978-3-642-39799-8_9) | 并发软件的部分序编码，不需要枚举每种调度 | 时间/同步模型与执行见证；题录也由 CAAT 参考文献交叉核对，本次未阅读全文 |
| Ponce de León, Haas, Meyer, *Dartagnan: SMT-based Violation Witness Validation*, TACAS 2022，[论文](https://hernanponcedeleon.github.io/pdfs/svcomp2022.pdf) | 独立验证反例，验证生成见证所表达的程序执行 | FALSE 的完整证据链；本次核对题录，未阅读全文 |
| Miné, *Static Analysis of Run-Time Errors in Embedded Real-Time Parallel C Programs*, ESOP 2012；*Relational Thread-Modular Static Value Analysis by Abstract Interpretation*, VMCAI 2014 | 干扰过近似、线程模块化和关系保持 | 单独论证 SAFE 后端与 AI/BMC 接口；不能只拿 BMC 文献证明全项目 sound |
| Vojdani et al., *Static Race Detection for Device Drivers: The Goblint Approach*, ASE 2016；POSIX 同步规范 | 私有化、锁保护及实际同步前提 | 审计发布/逃逸、must-lockset、条件锁和 join 的语义；本次以项目现有题录为阅读建议 |

即使近期只支持 SC，FMCAD/CAV 的“程序语义与内存模型分离”和“静态分析缩小相关关系集合”也非常适合 IKOS。弱内存支持可以后置，相关论文不必等到实现 RC11 才有价值。

## 7. 更贴合项目的改进路线

### P0：修复判定错误与文档承诺

- 将第 2 节用例纳入回归：无竞争 selector 用例不得确认 FALSE；两个竞争用例不得返回 TRUE。
- 修复或禁用前提不足的 private_init/偏移过滤；补充 UNKNOWN 能力边界，而不是以减少基准误报为由删除真实候选。
- FALSE 确认检查所有依赖语义是否已建模。地址未知、相关调用被跳过、条件依赖值来源未知等情况暂不能当作具体反例。
- 原文“保证 FP=0/FN=0”改为带配置、任务集合和版本的实测结论；已复现错误必须单独记录。

### P1：建立共享语义与可检验见证

- 统一前端 Intrinsic/API 分类、调用 effect 摘要和 BMC capability 查询，减少 raw 名称与 ar.* 名称的不一致。
- 支持检查不能只是枚举几类“不支持的同步”。还需明确处理 rwlock、once、try/timedlock、未知 extern 与间接调用的相关效果；未知且不能证明无关时拒绝确认。
- 事件使用 `(线程实例、调用上下文、循环迭代、静态语句、动态值版本)` 标识；函数入口名不等于线程实例，AR 变量指针不等于动态值。
- 编码赋值、phi/边赋值、参数与返回值；对整数使用位宽和 signed/unsigned 语义，优先考虑 bit-vector。数学整数需要明确且可核验的无溢出前提。
- 引入统一访问范围、对象与别名身份，区分抽象对象和具体分配实例。不要因抽象 pts 是 singleton 就自动推断唯一堆对象。
- 用成功/失败返回条件约束锁和 create/join；逐次建模临界区，明确 cond_wait 的虚假唤醒和 benchmark 语义。不要先松全程序 gate，再补这些内容。
- `confirm_race` 返回 `Confirmed(witness)`、`BoundedUnsat`、`Unsupported(reason)`、`SolverUnknown`、`BudgetExceeded`，而不是把全部情况压成 bool。

当前 `check_race` 丢弃 Z3 model，只返回 bool。报告层、YAML witness 又按函数名分配 thread_id，无法充分区分同一入口的多个实例。应让反例携带被确认的 pair、动态线程与调用实例、分支/输入、读源、地址和同步顺序，并能重建或独立验证执行。

`ikos2graphml.py:52` 也没有像 `svcomp_witness.py:first_race` 那样检查 `status == 2`，而且 GraphML 路径默认走不带 demote 的分析。它可以输出静态候选的 witness；因此“已有 GraphML”不能直接等同于“BMC 确认结果已被外部验证”。无需给 partial witness 强塞所有细节，但内部必须保有可验证的实际见证。

### P2：候选驱动、依赖闭包和预算

- 保留 IKOS 产生候选、BMC 查询反例的架构；按候选相关依赖构造有界程序切片，允许必要的 main 前缀和额外线程进入切片。
- 用 AI 的可信不可达结论、must-HB 和 may-alias 结果裁剪无关部分；不能用 may 事实直接确认可达或竞争，也不能用抽象初始状态直接代替可达状态。
- 采用通用 k 次展开并保留 guard/动态 SSA。界限处截断回边，而不是猜 N 次后强行走 exit；合法有限反例不要求无关循环完全展开。
- DFS 内及时检查节点、路径、事件和深度预算；solver 设置预算；再加每候选组、每文件总预算。256×256 条路径组合仍可能很昂贵。
- 缓存定义索引、调用摘要和路径前缀，尝试共享公式/增量查询；先测量开销再决定迁移到惰性内存模型求解。

### P3：扩展精度并做可复现实验

在上述契约成立之后，再遍历更多候选、扩展 K 个相关线程、精化堆实例。遍历所有 pair 时必须把报告和 witness 指向真正 SAT 的 pair，不能继续用 first pair 当锚点。

每次优化记录 gate 原因、动态事件数、相关线程数、展开与求解耗时、内存峰值、验证器结果。除了 TP/FP/TN/FN，还应分别统计 unsafe/safe 任务上的 UNKNOWN、崩溃和超时；`svcomp_race_test.py` 当前 Recall 分母排除了 UNKNOWN，FN=0 时不能据此说明覆盖了全部竞争。

建议使用独立保留集、通用语义小例和语义保持变换，避免只围绕 lost-TP 文件拟合规则。A/B 是必要的回归证据，不能替代程序语义证明。

## 8. 原文建议怎样重组

建议把“完整设计文档”调整为“当前实现、语义契约与扩展计划”，依次写：

1. 输入/语义/配置范围：SC、pthread、整数位宽、内存访问规范化、extern 假设和多线程实例。
2. 两个方向的判定契约，以及 UNKNOWN 的含义。
3. 当前实现的事实清单，含源码版本和明确未实现项。
4. 已复现缺陷与能力限制，分别列出 SAFE 过滤和 FALSE 确认。
5. 依赖闭包、执行编码、见证与预算设计及其证明义务。
6. 文献依据：每篇对应一个具体模块或结论。
7. 可复现评价：脚本、环境、任务列表、数据模型、版本、分项 UNKNOWN 和 witness 验证结果。
8. 历史设计与 TP 预测放附录，不能当作现状或数学能力上界。

证据标签建议使用“源码事实”“实测现象”“有前提的已证明结论”“设计假设”“未实现”。读过源码只能证明代码做了什么，不能自动证明代码的语义正确性。
