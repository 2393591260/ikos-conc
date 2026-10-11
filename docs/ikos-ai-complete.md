# IKOS-ConC 抽象解释：原理、语义合同与优化

本文是 AI 的唯一维护入口，整合原正确性审查、优化计划、双向反馈研究及专项修复。
当前基线见 [README](README.md#current)，整体调用链见 [架构](concurrency-architecture.md)。
这里的“已验收”指明确范围内的源码论证、对照实验及配对回归，不能读作全语言证明。

阅读顺序：先看 §1–2；按问题查 §3–6；研究与后续工作看 §7–8；
§9 是按机制组织的证据索引。构建/提交过程在 [开发速查](dev-gotchas.md#workflow)。

<a id="principles"></a>
## 1. 为什么抽象解释可以证明安全

每个程序点的抽象状态覆盖该点所有相关具体状态。局部转移覆盖本线程行为，
环境干扰覆盖其他线程可能改变的共享内存，再由内外两层不动点求稳定结果。
checker 收集访问，用保守充分条件排除不可能冲突的对。
值分析、访问覆盖和候选过滤三者都成立，才可由“无冲突候选”得出 SAFE。

这一分工源自原生 IKOS 的抽象解释框架；并发干扰与收敛的理论参照是
Miné LMCS 2012，锁保护、私有化、创建历史和 MHP 的实现参照主要是固定版本 Goblint。
不能把整个实现统称为移植 AstréeA 或 Goblint，逐机制依据见 §10。

MAY 值、对象和创建集合允许多报；错误收窄会漏执行。
MUST 持锁、完成或私有事实必须在每条相关执行上成立，未知时撤销该证明。
竞争本身虽属于 ISO C 未定义行为，仍是本工具要检测的性质；
不能因为“竞争是 UB”而删除竞争执行。其他 UB、API 失败和布局不支持另列合同。

### 先理解一个共享读

设线程 A 曾在本地写 `x=0`，线程 B 随后可能写 `x=1`。
A 再读 x 时，不能只保留自己的 0；允许干扰必须包括 1。
若多个线程可能写不同值，共享摘要取全部可能效果的并集，
不能按分析先后顺序只保留最后一个值。
局部强更新与跨线程/跨路径效果 join 是不同操作。

参考 Miné §3.2.1/Fig.7、§3.3/Fig.13：
允许共享读覆盖本地值和干扰；线程效果在外层不动点中累积。
保护精化可以减少允许干扰，但需要完整保护条件，不能先假设无竞争再删除干扰。

## 2. 代码所有权与状态

| 信息 | 拥有者/搜索入口 | 修改时要保持什么 |
| --- | --- | --- |
| 局部数值、指针、内存 | `execution_engine/numerical.hpp`、内存/数值域 | 位宽、别名、强弱更新和异常流覆盖 |
| pthread 与调用 | `concurrent_semantics.hpp`、`concurrent_inliner.hpp`、`handle_origin.hpp` | 实际参数、全部目标、库输入/输出/回调效果 |
| 锁/线程格 | `LocksetDomain` | MUST join 取交，MAY join 取并，TOP 不当成空精确信息 |
| 共享效果 | `ConcurrentGlobalEnv` | 所有可能写并集、pointer/heap 来源和重叠失效 |
| 局部与环境收敛 | `thread_modular.cpp` | 摘要语义变化重算，最终检查不能消费 stale 结果 |
| 访问与 SAFE 证书 | `data_race.hpp/.cpp`，`AccessRecord` | 每条过滤都有来源、前提和失败回退 |
| 供 BMC 的 facts | `analysis/bmc/facts.hpp` | 使用合同分开，不把 may 初值当实际 store |

数据/异常域与锁集域在顶层乘积中分离；引擎通过双态访问器兼容不含锁集的实例。
语义修复应放在拥有事实的层，checker 只消费已有事实或明确受限的充分条件，
不增加 benchmark 文件名、标签或形状特例。

<a id="interference"></a>
## 3. 共享内存、干扰与对象来源

### 3.1 当前读写覆盖

整数共享读覆盖本地内存与可能外部值，写队列发布全部可能效果。
global/heap pointer 读取保留初始化、本地与干扰来源；未知来源不能覆盖精确旧值后被忘记。
未知 offset、mixed width、字节 copy/set 和指针对象表示需要按字节重叠考虑。
完整标量发布有受限支持，其他布局保留覆盖警告或显式不支持。

堆指针的精确字段与通配字段若可能重叠，读端必须合并两者；
不能只查精确 key 而遗漏未知字段写。
pointer 复制、NULL 复制和重叠字节写会使旧精确 pointer 摘要失效。
非零整数转指针、部分重建和不支持的表示不能获得未经证明的 points-to 精度。

原六类审查中的 F4/F6 分别对应“读端遗漏后续干扰”和“写端遗漏其他写”；
F2 的发布父对象但漏掉可达子对象、后续 pointer 多来源和字节重建反例分别验证其他通道。
原始反例与控制见 [六类输入](artifacts/ai-audit-2026-10-10/cases/)及 §9。

### 3.2 私有、逃逸与生命周期

无完整 may 可达闭包时不排除 fresh heap；stack/TLS 指针发布后保守处理可达目标。
不能从“不在已知边表里”证明不逃逸，也不能从首写/之后持锁证明首写之前未发布。
旧 private-init 排除已禁用。

TLS 声明对应每动态线程不同对象；自己的 TLS 访问可以独立，
但经共享指针访问别的线程 TLS 必须保留对象来源。
allocation site、函数名或类型均不等于具体分配实例；
同 worker 的两次栈/堆锁分配可能是两把锁。
general free/lifetime 冲突尚未认证。

### 3.3 地址与宽度

`AccessRecord.width` 独立于起始 offset，load/store 来自 DataLayout。
只有证明所有可能访问字节范围不相交才可排除；
不同起点可能重叠，未知宽度不能被当成零长度。
相对字段偏移与 pointee 类型不能补足共同对象原点：
union、嵌入子对象和未知 getter 可让“相对 4”与“相对 0”访问同一绝对字节。

<a id="locks"></a>
## 4. 锁、线程身份与非并发证明

### 4.1 真实锁身份

共同 MUST mutex 需要同一具体对象、完整 byte offset、动态身份、有效成功持锁和互斥模式。
可表示性不足时不建立 MUST 锁；未知 unlock 必须撤销受影响证明。
不凭“同数据 region”“同字段布局”或哈希相同建立共同锁。
rwlock 的 shared/shared 不互斥；对写访问不能借两把 reader lock 排除。

确切共同 mutex 可直接排除候选，不必额外要求数据 region 单例；
但锁自身的具体身份仍须证明。原 self-consistency 不完整锁相等捷径已撤销。

### 4.2 相对锁与 helper 上下文

已有受限充分条件证明每条访问前缀持有 `data_address + delta` 的 mutex。
对共同非 TLS global，令 p 为 `2^pointer_bits` 与字节系数的 gcd；
两侧起点同余、width 均不大于共同周期时，字节重叠推出起点相同，
再由相同 delta 推出同 mutex。不同余数、过宽访问、heap summary 或未知原点不能使用。

跨 helper 通过准确 CallContext 绑定不可变指针形参与实参；
逐 callee 前缀无 unlock/call/rebind 后才向 caller 延续证明。
缓存按语句与上下文，而非只按静态 store。
递归、间接入口、未覆盖 CFG 环、预算不足、局部动态下标或中途解锁保留候选。
机器位宽、sext/zext/trunc 必须区分，不能把数学整数等式用于可回绕算术。

### 4.3 条件加锁

建立条件锁需要相同动态激活内守卫的零值等价，
截断、phi、共享 load、跨宽度运算和重复 helper 不能按语法名字剥层。
同位宽、相同来源且单次激活的加减常量可按模等式证明零偏移抵消；
no-wrap 运算只对能继续执行的路径使用，不能宣称覆盖发生 UB 的源级执行。
实际执行 lock 的分支还保留普通 MUST；两个不同分支不能凭各自 MAY 锁拼出 MUST。

<a id="threads"></a>
### 4.4 创建、join 和条件存活

线程身份至少包含真实创建来源与动态唯一性；同入口函数不代表同一个线程。
存在重复父实例时，某父访问尚未创建孩子不代表另一个父实例没有已存活孩子。
非 unique 实例要保留 self-race，MAY spawned TOP 不能解释为“确定未创建”。

静态至多一次执行证书基于隐式 main 根、唯一非逃逸调用/创建边与无可重复 CFG。
创建边可纳入嵌套生命周期证明，但静态唯一调用链不等于“当前只活着一个线程”。
未知调用目标、重复/循环/递归、逃逸入口或注册创建点覆盖不足时拒绝证书。

pthread_t 是内部身份令牌而非 POSIX 可观察整数。
区间端点不能证明 join 目标；多 may 输出 create 写必须弱更新。
有来源的唯一非逃逸标量槽、完成支配与成功零返回关联可恢复受限确定目标；
多 writer、数组/heap 槽、错误身份、数值观察及动态不透明来源保守拒绝。
detach 不建立完成/HB，sleep 不证明别的线程已结束。

独立 MAY live-child 摘要覆盖条件 create/join，join 合流取并，未知为 TOP。
空 live 集只在直接 creator 唯一、相关注册点覆盖等前提下证明“不可能并发”；
它不是方向 HB，也不能删除未完成的孙线程。条件 factory 的结果关联不能无限跨调用猜测。

### 4.5 结构化 MHP 的迁移约束

Goblint 的 MHP 参照分成四类非并发条件：同一唯一实例、确定未开始、确定已 join、
创建/完成的传递关系。它们需要兼容的线程历史与祖先事实，不能把静态祖先集合里的 MAY 边
作为 must-ancestor。IKOS 一条函数级访问可能代表多个实例，排除须覆盖其**全部可能实例**；
只挑一个已 join 的 site 不成立。若以后分区线程上下文，可保留更精确的状态与实参，
但须控制上下文增长、合流和完整覆盖，不能因性能约束永久禁止上下文敏感分析。

现有嵌套生命周期/live-child 是受限证书，不是完整 History/digest 移植。
原 MHP 草案里“给记录加单个 instance_sid 就能排除”及按静态 may 祖先递推的实现清单
不作为当前实现合同；来源/唯一性/全称覆盖核实后才可采用。
固定参照在 Goblint 的 `mHP.ml`、`mHPAnalysis.ml` 和线程身份域。

<a id="effects"></a>
## 5. 库、内部调用与访问覆盖

未知 extern、未展开 internal、未知线程入口和递归遗漏访问必须有覆盖回退。
函数名字前缀、首个可能目标或假定“只是输出”均不能证明没有用户内存效果。
全部可能 callee 的效果取并集；精确声明摘要要求签名、参数和 ABI 合同成立。

| API/效果 | 当前需要保留的含义 | 边界 |
| --- | --- | --- |
| pthread_create | 写句柄输出、读取相关属性、透传线程参数并分析入口 | 透传 arg 不自动等于父线程写；输出不能整调用豁免 |
| pthread_cond_wait / signal | wait 期间遗忘共享内存精度，保留返回后合法持锁/续行覆盖；signal/awaited 只作 MAY 观测 | 不从 signal 数量、epoch 或 MAY 队列建立确定跨线程 HB；完整失败和 mutex 属性另有边界 |
| pthread_once | 按具体 control 身份至多一次、覆盖全部回调 | control offset/动态对象/未知回调不能按函数名合并 |
| attr getter、sem_getvalue | 用户输出槽写 | 同步对象内部状态豁免不包括输出槽 |
| sem_timedwait | 合法可能失败/阻塞路径中的 timeout 输入读 | 参考工具未建模该读的 PASS 不能证明无竞争 |
| printf 家族 | %s/format 读，%n/目标 buffer 写；还可能有 stream 同步 | 仅核实过的不可变格式/整数输出摘要受支持 |
| read/recv 等 buffer | 输出与共享效果覆盖，zero-byte read 保留合法续行 | 不能删掉零返回后的访问；一般精确值流未覆盖 |
| pthread_exit(NULL) | 终止当前线程续行，保留退出前访问/摘要 | 非 NULL、析构回调及全部库状态未完整建模 |
| sleep/usleep/nanosleep | 经过核实声明的暂停合同 | 不释放用户锁、不产生完成事实 |
| 外部原子库 | 显式效果覆盖或不支持 | 不凭 GCC-looking 函数名注入伪锁 |
| verifier atomic | 按竞赛 API 合同保护完整函数体/命名边界 | 正常函数、错误原型和缺失 body 不能套用 |

上述仅陈述受限实现，完整 POSIX 失败、属性、回调及 free/lifetime 仍需独立验收。
编译/导入明确拒绝是 error，不得声称都已经变成 UNKNOWN。

<a id="fixpoint"></a>
<a id="safe-rules"></a>
## 6. 耦合不动点与 SAFE 验收合同

外层摘要按语义增长收敛。offset 扩张、points-to 转 TOP、MUST 撤销、
创建目标增加及 join summary 变化都需要 dirty/replay，而不只是集合 size 增长。
每轮 flush 覆盖写队列；最终检查若改变摘要，不能继续消费旧 SAFE 结论。
已有这类工程防线，但通用独立 post-fixpoint 认证器尚未实现。

### 当前过滤/覆盖规则的证明义务

| 规则/通道 | 必须证明 | 前提不可证时 |
| --- | --- | --- |
| normal bottom | 初始化、局部转移、干扰和收敛覆盖支持执行 | 保留未知效果，不借意外 bottom 证明安全 |
| 私有访问 | 全部跨线程 alias 被排除 | fresh/逃逸不明保留共享候选 |
| read/read | 已完整记录相关写及生命周期效果 | 未知效果继续记录 |
| 同一线程 | 同一动态实例或实例不能重叠 | 非 unique 保留 self-race |
| join HB | 有来源的确定目标、成功完成和全部相关实例 | 不用区间端点/任意数值令牌 |
| create HB | 祖先/creator、父生命周期和完整 MAY spawn | 未知父实例/入口保留候选 |
| may points-to 不交 | 所有可能对象及表示来源都被覆盖 | TOP 对所有可能对象配对 |
| 双 atomic | 实际 AR atomic 与所选 race 定义相符 | mixed pair 保留 |
| byte range 不交 | 对象原点、offset、width 覆盖全部可能字节 | TOP/未知 width 保留 |
| 共同锁 | 相同具体锁、有效持有及互斥模式 | 符号 hash/未知 unlock 不构造 MUST |
| 条件锁 | 零值等价、固定动态激活及实际分支 | trunc/phi/重复调用不获证书 |
| 相对锁 | 全部路径 data+delta、重叠引理、准确上下文 | 环/call/rebind/预算耗尽拒绝 |
| private-init/region 捷径 | 完整发布、别名和保护证明 | 旧排除禁用，不以注释标签代替证明 |
| 库/internal 覆盖 | 所有读写、回调、同步效果 | 警告/UNKNOWN 或显式导入错误 |
| coupled fixpoint | 全部摘要失效与最终检查稳定 | 检测变更后重算/保守回退 |
| opt-in 覆盖查询 | 稳定 over-approx、共同对象和准确 width | 矛盾/超时/不支持保持原候选 |

[工程清单](artifacts/ai-precision-2026-10-11/final-acceptance/inventory.json)保存消费点、
原始 runner 退出状态和支持义务。
历史 single-execution、lock-allocation-identity 及 sem-value runner 中更强的精度断言，
曾在前驱/当前二进制同时失败；保留 [旧断言配对](artifacts/ai-precision-2026-10-11/final-acceptance/legacy-contracts.json)，
不能称所有旧 runner 全绿。没有登记为已复现未处理的本轮 FN，不等于不存在其他潜在 FN。

<a id="feedback"></a>
## 7. AI/BMC 协同与论文研究边界

### 三类查询不能混用

| 查询 | 覆盖范围 | SAT | UNSAT |
| --- | --- | --- | --- |
| 有界竞争 BMC | 所选有限前缀，真实值/对象/同步 | 满足执行合同才确认 FALSE | 仅该有界搜索未找到，不证明 SAFE |
| AI 候选排除 | 全部相关访问状态的 over-approx | 可能是真冲突，也可能是伪报 | 覆盖前提成立时可排除该对 |
| 摘要归纳检查 | init、每个本地 step、全部环境 interference | 暴露闭包漏洞，可能抽象伪路径 | 加上结构覆盖才认证归纳闭包 |

自由变量对 FALSE 可能制造假执行；对排除查询扩大状态通常是保守的。
不能复用一个“SMT 返回真假”的无类型接口，也不能把 BMC 有界 UNSAT 改名当安全证书。

### 已实现的方向

AI 提供 may 候选、布局、地址范围及受限 no-wrap/生命周期证书；
BMC 独立建立实际 store 来源、线程与对象身份。
跨 helper 候选减少和 helper 发布的事实组消融见 [架构 §5](concurrency-architecture.md#5-ikos-与-bmc-的接口边界)。
具体 BMC 消费合同在 [BMC §3](ikos-bmc-complete.md#architecture)；
独立 AR 检查仍重走实际执行，不直接相信 AI 证书。

### 已实验但默认关闭的原型

原型 A：稳定 AI 的 interval/congruence 支持字节地址候选排除，
原型 B：查询模型仅选择更强数值域，对同一不可变 IR 重新收敛。
模型不作为环境假设；失败/预算不足保留初轮。B 属于域重算/portfolio，
未形成真正的 BMC 执行→并发抽象分区反馈，也不称 CEGAR。

固定 ILP32 集的 8 组历史消融逐项配对：AI 配置零判定变化，
两个 BMC 配置判定相同。强域查询 1240 SAT、0 SMT 排除；
模型选域只有 7 个任务进入第二轮，仍无新增 TN。
合成稀疏地址例有覆盖查询收益，但冻结集没有新增精度或提速依据。

保存 [完整消融](artifacts/ai-precision-2026-10-11/feedback-corpus/summary.json)、
[逐任务配对](artifacts/ai-precision-2026-10-11/feedback-corpus/paired.json)、
[IR 一致性](artifacts/ai-precision-2026-10-11/feedback-corpus/ir-hashes.json)、
[重算](artifacts/ai-precision-2026-10-11/feedback-corpus/retries.json)、
[测量限制](artifacts/ai-precision-2026-10-11/feedback-corpus/measurements.json)。
测量为单轮，wrapper 含反复 hash 开销；child CPU 不含父进程，
单 child max RSS 不等于进程树峰值，不能据此声称端到端加速。

### 尚未实现的研究路线

真实 BMC 执行可用于选取 partition/pack 或 writer 分区，但精化后 AI 必须重新收敛；
或另建 init/step/完整环境闭包认证。依赖摘要变更后证书重查，
失败/不支持保留原 over-approx。先定位真实抽象损失，再选择一个有一般证明的受限片段。

“用 BMC 帮助 AI”至少在 SAS 2011 path focusing 已出现；
顺序 reduced CFG/cutpoint 定理不能省掉并发环境义务直接沿用。
证书首先指可核对合同、公式和依赖，不自动意味着 solver proof object 或官方 witness。
论文仍需近邻工作检索、明确保持性论证、AI-only/BMC/事实开关/反馈消融、
公平参考工具比较和独立任务集。现有局部参考误判不证明整体支配。

<a id="roadmap"></a>
## 8. 后续 TN 机制与阶段出口

每个入口先读具体程序，预测状态，再检查固定 Goblint 的实际摘要/路径和 IKOS 探针。
参考 TRUE 只选入口，不代替本项目证明；失败、Unsound 与不支持分别记录。
最新任务总数在 [README](README.md#current)，不重复承诺全部恢复。

| 优先方向 | 具体缺口 | 实施前提与出口 |
| --- | --- | --- |
| 保护相关 release 端值 | privatized/嵌套锁中间写污染其他线程 | 初始化、进行中 may writes、实际 release endpoint 分开；完整 MUST 保护集；迟到未保护 writer dirty/replay |
| 稳定 pointer 来源 | 初始 NULL 与发布偏移的无序 join 破坏精确锁地址 | 唯一非逃逸 full pointer writer；所有相关 create 在发布后；writer 单调来源摘要及重叠失效 |
| 字段/字节干扰 | unrelated cells 合流，未知字段混合读 | 保留 whole-object/TOP bucket，精确与未知写/读覆盖所有重叠 |
| 动态身份/MHP | 重复 create、opaque handles、条件 join 相关性 | 上下文与来源保持，不把一个静态 site 当唯一动态实例 |
| 关系/收敛优化 | guard/地址关系或过粗 widening | 先证实丢失位置，选择小 pack；环境关系与机器算术闭包成立 |
| 依赖/稀疏枚举 | 全线程重算或大量互斥候选 | 未知依赖回退全相关集合，TOP bucket 完整配对，逐任务判定不变后测成本 |

两个 release 入口：
`13-privatized_03-priv_inv` 的临界区写 0 再写 1，
`28-race_reach_60-invariant_racefree` 的外层 L 排除 worker，但内层 M 释放中间值。
当前 `record_privatized_write` 记录中间写，不能当真实 release 摘要；
未被用来证明保护读的 `read_global_with_lockset` 不能直接启用作快捷恢复。

候选设计需覆盖 initializer ∪ 不受所持锁排除的 ongoing writes ∪ 真实 release 值；
已由外层锁私有的状态不能因获取内层锁被无条件刷新。
unknown writes、cond_wait、未知 alias/布局/同步效果必须撤销相应精化；
所有通道经 monotone join/widening 及 dirty/replay 收敛。
这是待实现设计，尚不能称完整 PBR。

稳定 pointer 入口为 `race-3_1-container_of-global`；先确认来源/offset 第一次丢失，
不能仅凭疑似 NULL 污染就采用最近 store 强更新。
AI helper 生命周期证书与 BMC 发布支持不自动提供 AI 的 writer-specific pointer 摘要。

阶段结束应是：机制前提可核实、正/反/变体通过、同配置 AI/BMC 全量配对、立即单机制提交，
剩余案例有具体缺口。若路线无收益可停止扩张该原型，不等于整体优化完成或语义不可实现。

<a id="evidence"></a>
## 9. 按机制查证据

以下索引保存历史实验的版本与转移；表中旧编号仅用于回溯原报告，不是阅读顺序。
原始长文可用 README 中的 Git 命令取回。每次新机制更新其对应章节及证据行，
不再追加一份完整阶段报告。

| 原编号 | 机制或实验 | 原始依据 |
| --- | --- | --- |
| <a id="ai-step-01"></a>01 | P0-F1：撤销 self-consistency 共同锁捷径 | [记录](artifacts/ai-p0-2026-10-10/f1/summary.json)、[目录](artifacts/ai-p0-2026-10-10/f1/) |
| <a id="ai-step-02"></a>02 | P0-F3：未知线程体的覆盖回退 | [记录](artifacts/ai-p0-2026-10-10/f3/summary.json)、[目录](artifacts/ai-p0-2026-10-10/f3/) |
| <a id="ai-step-03"></a>03 | P0-F5：join 只从确定句柄获得 MUST 完成信息 | [记录](artifacts/ai-p0-2026-10-10/f5/summary.json)、[目录](artifacts/ai-p0-2026-10-10/f5/) |
| <a id="ai-step-04"></a>04 | P0-F6：共享写队列按可能效果取并集 | [记录](artifacts/ai-p0-2026-10-10/f6/summary.json)、[目录](artifacts/ai-p0-2026-10-10/f6/) |
| <a id="ai-step-05"></a>05 | P0-F4：整数共享读覆盖本地值与可能干扰 | [记录](artifacts/ai-p0-2026-10-10/f4/summary.json)、[目录](artifacts/ai-p0-2026-10-10/f4/) |
| <a id="ai-step-06"></a>06 | P0-F2：无逃逸闭包证据时不排除堆对象 | [记录](artifacts/ai-p0-2026-10-10/f2/summary.json)、[目录](artifacts/ai-p0-2026-10-10/f2/) |
| <a id="ai-step-07"></a>07 | P0：多源指针读保留本地与全部干扰值 | [记录](artifacts/ai-p0-2026-10-10/pointers/summary.json)、[目录](artifacts/ai-p0-2026-10-10/pointers/) |
| <a id="ai-step-08"></a>08 | P0：耦合摘要参与收敛，最终检查变更回退 | [记录](artifacts/ai-p0-2026-10-10/coupling/summary.json)、[目录](artifacts/ai-p0-2026-10-10/coupling/) |
| <a id="ai-step-09"></a>09 | P0：pthread_once 控制对象与回调覆盖 | [记录](artifacts/ai-p0-2026-10-10/once/summary.json)、[目录](artifacts/ai-p0-2026-10-10/once/) |
| <a id="ai-step-10"></a>10 | P0：字节内存操作发布与覆盖边界 | [记录](artifacts/ai-p0-2026-10-10/bytes/summary.json)、[目录](artifacts/ai-p0-2026-10-10/bytes/) |
| <a id="ai-step-11"></a>11 | P0：堆指针通配字段与精确字段重叠 | [记录](artifacts/ai-p0-2026-10-10/heap/summary.json)、[目录](artifacts/ai-p0-2026-10-10/heap/) |
| <a id="ai-step-12"></a>12 | P0：锁偏移打包只对可表示身份建立MUST | [记录](artifacts/ai-p0-2026-10-10/locks/summary.json)、[目录](artifacts/ai-p0-2026-10-10/locks/) |
| <a id="ai-step-13"></a>13 | P0：未知调用体的访问覆盖与经过核实的声明合同 | [记录](artifacts/ai-p0-2026-10-10/calls/summary.json)、[目录](artifacts/ai-p0-2026-10-10/calls/) |
| <a id="ai-step-14"></a>14 | P0：外部原子库效果覆盖与函数名前缀 | [记录](artifacts/ai-p0-2026-10-10/atomics/summary.json)、[目录](artifacts/ai-p0-2026-10-10/atomics/) |
| <a id="ai-step-15"></a>15 | P0：库buffer效果与零字节read续行 | [记录](artifacts/ai-p0-2026-10-10/library/summary.json)、[目录](artifacts/ai-p0-2026-10-10/library/) |
| <a id="ai-step-16"></a>16 | P0：合成线程句柄表示与 join 来源 | [记录](artifacts/ai-p0-2026-10-10/handles/summary.json)、[目录](artifacts/ai-p0-2026-10-10/handles/) |
| <a id="ai-step-17"></a>17 | 精度：detach 的不透明身份消费 | [记录](artifacts/ai-p0-2026-10-10/detach/summary.json)、[目录](artifacts/ai-p0-2026-10-10/detach/) |
| <a id="ai-step-18"></a>18 | 精度：经过核实的 POSIX sleep 声明 | [记录](artifacts/ai-p0-2026-10-10/sleep/summary.json)、[目录](artifacts/ai-p0-2026-10-10/sleep/) |
| <a id="ai-step-19"></a>19 | 精度：NULL状态的当前线程退出 | [记录](artifacts/ai-p0-2026-10-10/exit/summary.json)、[目录](artifacts/ai-p0-2026-10-10/exit/) |
| <a id="ai-step-20"></a>20 | 正确性：指针对象表示与重叠写入 | [记录](artifacts/ai-p0-2026-10-10/ptrbytes/summary.json)、[目录](artifacts/ai-p0-2026-10-10/ptrbytes/) |
| <a id="ai-step-21"></a>21 | 简化配置 Goblint 对照池 | [记录](artifacts/ai-p0-2026-10-10/ptrbytes/goblint-current-safe-unknown.json)、[目录](artifacts/ai-p0-2026-10-10/ptrbytes/) |
| <a id="ai-step-22"></a>22 | 精度：统一 verifier atomic 函数体合同 | [记录](artifacts/ai-precision-2026-10-11/atomic-prefix/summary.json)、[目录](artifacts/ai-precision-2026-10-11/atomic-prefix/) |
| <a id="ai-step-23"></a>23 | 官方 Goblint portfolio 校准与后续入口 | [记录](artifacts/ai-precision-2026-10-11/goblint-portfolio/summary.json)、[目录](artifacts/ai-precision-2026-10-11/goblint-portfolio/) |
| <a id="ai-step-24"></a>24 | 正确性：create 的 may-output 必须弱更新 | [记录](artifacts/ai-precision-2026-10-11/create-handle-alias/summary.json)、[目录](artifacts/ai-precision-2026-10-11/create-handle-alias/) |
| <a id="ai-step-25"></a>25 | 精度：用访问地址证明互斥锁对应 | [记录](artifacts/ai-precision-2026-10-11/relative-mutex/summary.json)、[目录](artifacts/ai-precision-2026-10-11/relative-mutex/) |
| <a id="ai-step-26"></a>26 | 精度：仿射数组地址与 mutex 对应 | [记录](artifacts/ai-precision-2026-10-11/affine-mutex/summary.json)、[目录](artifacts/ai-precision-2026-10-11/affine-mutex/) |
| <a id="ai-step-27"></a>27 | 正确性：条件锁守卫的零值等价与动态激活 | [记录](artifacts/ai-precision-2026-10-11/conditional-guard/summary.json)、[目录](artifacts/ai-precision-2026-10-11/conditional-guard/) |
| <a id="ai-step-28"></a>28 | 正确性：整数发布与字节重建的指针表示 | [记录](artifacts/ai-precision-2026-10-11/integer-escape/summary.json)、[目录](artifacts/ai-precision-2026-10-11/integer-escape/) |
| <a id="ai-step-29"></a>29 | 前端独立阶段：无损导入 LLVM memory length | [记录](artifacts/ai-precision-2026-10-11/copy-boundary/summary.json)、[目录](artifacts/ai-precision-2026-10-11/copy-boundary/) |
| <a id="ai-step-30"></a>30 | 精度：条件加锁分支同时保留实际 MUST | [记录](artifacts/ai-precision-2026-10-11/conditional-must/summary.json)、[目录](artifacts/ai-precision-2026-10-11/conditional-must/) |
| <a id="ai-step-31"></a>31 | 精度：精确调用上下文下的 helper 锁/数据证书 | [记录](artifacts/ai-precision-2026-10-11/helper-context/summary.json)、[目录](artifacts/ai-precision-2026-10-11/helper-context/) |
| <a id="ai-step-32"></a>32 | 研究原型 A：稳定 AI 地址事实上的覆盖查询 | [记录](artifacts/ai-precision-2026-10-11/pair-feedback/summary.json)、[目录](artifacts/ai-precision-2026-10-11/pair-feedback/) |
| <a id="ai-step-33"></a>33 | UNKNOWN逐文件台账与工程P0出口 | [记录](artifacts/ai-precision-2026-10-11/unknown-ledger/summary.json)、[记录](artifacts/ai-precision-2026-10-11/final-acceptance/inventory.json)、[目录](artifacts/ai-precision-2026-10-11/unknown-ledger/) |
| <a id="ai-step-34"></a>34 | 研究原型 B：模型选域、同IR的新分析与完成节点 | [记录](artifacts/ai-precision-2026-10-11/feedback-corpus/summary.json)、[目录](artifacts/ai-precision-2026-10-11/feedback-corpus/) |
| <a id="ai-step-35"></a>35 | 第二版精度：固定激活下的机器位宽加减抵消 | [记录](artifacts/ai-precision-2026-10-11/guard-cancellation/summary.json)、[目录](artifacts/ai-precision-2026-10-11/guard-cancellation/) |
| <a id="ai-step-36"></a>36 | 第二版精度：嵌套创建的单生命周期证明 | [记录](artifacts/ai-precision-2026-10-11/nested-lifetime/summary.json)、[目录](artifacts/ai-precision-2026-10-11/nested-lifetime/) |
| <a id="ai-step-37"></a>37 | 第二版精度：已完成创建的标量句柄来源 | [记录](artifacts/ai-precision-2026-10-11/handle-origin/summary.json)、[目录](artifacts/ai-precision-2026-10-11/handle-origin/) |
| <a id="ai-step-38"></a>38 | 第二版精度：条件创建/join的活跃实例摘要 | [记录](artifacts/ai-precision-2026-10-11/live-children/summary.json)、[目录](artifacts/ai-precision-2026-10-11/live-children/) |
| <a id="ai-step-39"></a>39 | 第二版精度：具体同锁互斥与数据region解耦 | [记录](artifacts/ai-precision-2026-10-11/concrete-locks/summary.json)、[目录](artifacts/ai-precision-2026-10-11/concrete-locks/) |
| <a id="ai-step-40"></a>40 | 第二版精度：隐式main的共享栈对象与SC来源 | [记录](artifacts/ai-precision-2026-10-11/main-stack/summary.json)、[目录](artifacts/ai-precision-2026-10-11/main-stack/) |
| <a id="ai-step-41"></a>41 | 第二版精度：AI生命周期证书用于helper发布 | [记录](artifacts/ai-precision-2026-10-11/publication/summary.json)、[目录](artifacts/ai-precision-2026-10-11/publication/) |
| <a id="ai-step-42"></a>42 | 递归AR类型诊断的独立修复与本轮出口 | [记录](artifacts/ai-precision-2026-10-11/diagnostic-types/summary.json)、[目录](artifacts/ai-precision-2026-10-11/diagnostic-types/) |

### 六类原审查与早期专项

| 机制 | 复现与证据 |
| --- | --- |
| 原 F1–F6 与无竞争/改名变体 | [输入](artifacts/ai-audit-2026-10-10/cases/)、[逐结果](artifacts/ai-audit-2026-10-10/ikos-results.json)、[参考与警告](artifacts/ai-audit-2026-10-10/reference-results.json) |
| stdio 用户内存和只读识别 | [摘要](artifacts/stdio-effects-2026-10-09/summary.json)、[复现](review-repros/stdio-effects/) |
| sem_getvalue 输出槽 | [摘要](artifacts/sem-value-2026-10-09/summary.json)、[复现](review-repros/sem-value/) |
| 同步 API 参数效果 | [摘要](artifacts/sync-effects-2026-10-09/summary.json)、[复现](review-repros/sync-effects/) |
| 共同对象原点与结构偏移 | [摘要](artifacts/structural-offset-2026-10-09/summary.json)、[复现](review-repros/structural-offset/) |
| 调用/创建单生命周期 | [摘要](artifacts/single-execution-2026-10-09/summary.json)、[复现](review-repros/single-execution/) |
| 重复父线程创建过滤 | [摘要](artifacts/creation-instance-2026-10-10/summary.json)、[复现](review-repros/creation-instance/) |
| 部分标量写与宽度 | [复现](review-repros/interference-width/) |
| stack/heap 锁实例 | [摘要](artifacts/lock-allocation-identity-2026-10-10/summary.json)、[复现](review-repros/lock-allocation-identity/) |
| TLS 来源与具体身份 | [摘要](artifacts/tls-identity-2026-10-10/summary.json)、[复现](review-repros/tls-identity/) |

### 对照、撤回结论与参考工具差异

- [helper 解锁与 Goblint 符号锁消融](artifacts/ai-precision-2026-10-11/relative-mutex/goblint-ablation.json)：固定配置正常 TRUE 与源码竞争/IKOS 执行有分歧，关闭 symb_locks 变 UNKNOWN。未采集全部内部 EqualSet 轨迹，不称完整根因证明。
- [Dartagnan ILP32 布局](artifacts/ai-precision-2026-10-11/affine-mutex/case7-reference-layout.json)：原生 mutex offset=4，参考处理部分地址为 8，执行图因此使用两把锁；LP64 bound3/5 PASS。固定版本局部缺陷不等于整体工具更差。
- 原部分 Goblint ILP32 实验带 Machine definition 不支持警告，不用于认证；后续机器定义校准和 portfolio 参照见上表 23。官方表的资源/任务/配置与本地不相同，不能直接比较 TN 总数。
- 旧 mutex-HB 报告把“不能消费最后一个 release 的单值”扩大成“thread-modular 不能实现 privatization”，该全称结论不保留。后者已有 Miné/Goblint 理论依据；IKOS 的具体实现义务仍见 §8。
- 旧分桶数量、文件名推测及参考 PASS 不作为语义真值；历史 CSV 保留原实验条件，不升级为当前对比。
- [早期 Goblint 测试 CSV](archive/ikos-on-goblint-tests.csv)与 [早期双工具 CSV](archive/ikos-vs-goblint-race-comparison.csv)是历史输入/配置记录。旧报告的 `.c`/`.i` 替换、RACE 标记解析和 may-mode 口径限制不沿用来宣称当前优势。
- 旧 CPAchecker 12 输入试点使用 LP64，不能和当前 ILP32 全量比总数；boundary-tests 的终端转录不再维护正文，保留实际测试代码，原运行细节从 Git 查询。

<a id="references"></a>
## 10. 理论与固定实现的出处

| 依据 | 实际用途与阅读边界 |
| --- | --- |
| Miné，[Static Analysis of Run-Time Errors in Embedded Real-Time Parallel C Programs](https://lmcs.episciences.org/799/pdf)，LMCS 2012，DOI 10.2168/LMCS-8(1:26)2012 | 已读相关 §3–4.4：干扰、外层不动点、同步读写及访问覆盖；ESOP 2011 扩展版。原型 Thésée，不能据此推断闭源 AstréeA 内部实现 |
| [The Astrée Analyzer](https://www.di.ens.fr/~cousot/publications.www/CousotEtAl-ESOP05.pdf)，ESOP 2005 | 已读 §4–6：域组合、精度与小变量 packs；不能当并发语义证明 |
| [Improving Thread-Modular Abstract Interpretation](https://doi.org/10.1007/978-3-030-88806-0_18)，SAS 2021 | 已读相关 §2–5：PBR/lock-/write-centered privatization 与 side-effecting constraints；不能拼接不同算法的更精确分支后继承定理 |
| [Clustered Relational Thread-Modular Abstract Interpretation with Local Traces](https://doi.org/10.1007/978-3-031-30044-8_2)，ESOP 2023 | 已读相关 §2–8：local traces、clusters、线程身份/MHP；双变量 cluster 结论需要其 2-decomposable 前提 |
| [The digest framework](https://doi.org/10.1007/s10009-024-00773-y)，STTT 2024 | 有限阅读 §3 admissibility：历史兼容性不能排除真实执行 |
| [Data Race Detection by Digest-Driven Abstract Interpretation](https://doi.org/10.1007/978-3-032-15700-3_15)，VMCAI 2026 | 实际阅读[作者扩展版](https://arxiv.org/abs/2511.11055)相关章节；完整付费正式版和 8.9GB artifact 实验未复现 |
| Monniaux/Gonnord，[Using Bounded Model Checking to Focus Fixpoint Iterations](https://www-verimag.imag.fr/~monniaux/biblio/Monniaux_Gonnord_SAS2011.pdf)，SAS 2011 | 已读 §3.1–3.4/§4：完整切点路径精化/归纳；并发环境闭包需另证 |

阅读章节和 PDF/hash 见 [publications.json](artifacts/ai-audit-2026-10-10/publications.json)、
[digest 来源](artifacts/ai-goblint-reference-2026-10-10/sources.json)。这次文档整理未重做论文实验。

固定 Goblint：/home/ruan/goblint-analyzer，commit bb55b70d91290992629ba3d073185c10cdf356e6。
按任务读 basePriv.ml、mutexAnalysis.ml、threadId.ml/threadIdDomain.ml、mHPAnalysis.ml、commonPriv.ml、mCPAccess.ml。
digest 是线程计算历史摘要，不是校验哈希或单纯函数摘要；IKOS 已有类似 operational 组件，尚未实现完整 digest/PBR 框架。
不能由“还未完整采用论文框架”推断必然弱多少，也不能由近似 TN 计数推断能力等价。
POSIX/API 布局依据固定声明、平台 header/native 断言和实际参数效果；完整 C11 弱内存不在当前认证范围。
