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

目的 may-points-to 为 TOP 的普通 Store 也必须保留覆盖警告，即使尚未创建线程、
写入持锁或没有候选对。该分支忘记本地内存并通知外层重算；共享旧摘要尚不能视为
已完整失效，故当前以持久能力边界阻止 SAFE，不能称为已完成未知目的值模型。

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
受限初始化发布证书逐对核对分配/调用身份、首次发布前不逃逸、槽的完整来源
及读端实际取值路径；不依赖旧 private-init 标记，见 §8 的实现与边界。

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

### 3.4 保护值与真实释放端点（M2）

普通 may-write 摘要包括临界区的中间值。例如 writer 持 L 执行 `x=0; x=1; unlock(L)`，
另一个持 L 的 reader 实际不能读到中间的 0；把全部写导入其守卫会制造不可达分支。
不能反过来只取“分析中最后一次写”：不同路径、线程和外层迭代没有这种执行顺序。

`protected_values.hpp` 与 `ConcurrentGlobalEnv` 新增三个单调通道：

| 通道 | 内容与保护标签 | 消费规则 |
| --- | --- | --- |
| I[x] | 正常全局初始化结束时的整数区间 | 第一次取得 mutex 的入口候选值 |
| O[x,H] | 每条可能 store 的值，H 为当时完整、可证明的 exclusive MUST mutex 集 | 仅当 H 与 reader 所持锁不相交时导入干扰 |
| R[x,H_after] | 实际 unlock 前的本地内存，合并与 H_before 不相交的 O；标签为释放后仍持锁集合 | 第一次取得锁时导入与新锁集不相交的 release 值 |

第一次精确取得普通 mutex 时，用 `I ∪ O_disjoint(H_new) ∪ R_disjoint(H_new)` 恢复整槽内存。
覆盖理由分两种：共享值来自当前可并发的写，则由 O 覆盖；来自已结束的受保护片段，
则由真实释放端点 R 覆盖。与新锁相交的进行中写不可能与已持锁 reader 同时发生。
初始化和所有不受该锁排除的写仍保留，不能先假设程序无竞争。

后续/嵌套 lock 保留本地值，只按取得新锁之前的 H 导入允许干扰；
旧 H 为空时还弱合并 handoff。持锁 load 继续合并允许的 O，空 O 保留本地内存。
R 不再混入 I 或旧 R：释放端点已经覆盖这些来源到当前点的转移。
内层 unlock 后仍持外层 L 的端点标记 L，因此不会把外层临界区的中间值发布给另一个持 L 的线程。
这些都是抽象状态摘要，不能作为新的源程序 store 或 BMC rf 事件。

`LocksetDomain` 的 MAY 获取历史在真正初态为 false，首次可能获取后为 true；
join 取 OR，meet 取 AND，参与 leq/equality/widen/narrow。丢失 MUST 锁不清除此历史，
避免循环、条件分支或后续片段被误当成首次获取。未知同步及 cond_wait 撤销精化。
符号锁地址与 verifier atomic 标记不作为本机制的真实 mutex handoff。

整 bundle 静态证书只注册 defined、非 TLS 的整宽整数 global：所有取址使用必须为
非 volatile/atomic 的整槽 load/store 或单一定义的透明零偏移别名。
传参逃逸、部分字节写、未知别名和重定义形参均拒绝。构造函数、自定义初始化或非 main 根
不启用该首次获取子集。未覆盖调用/创建、TOP 写及不支持 mutex 类型/成功条件撤销精化，
回到完整 may-write board；真实效果缺口另由 coverage gate 阻止 SAFE。
新保护标签即使数值不变也置 dirty，最终检查的迟到变化沿用既有重算/UNKNOWN 出口。

已覆盖库输出还需区分 NULL、局部目的与可能全局目的。`local_output_base` 只沿 pointer
copy/bitcast/PointerShift 返回实际 LocalVariable，显式拒绝 formal、load、call 和整数转换。
在合法前端数据流及有效对象内存语义下，它证明直接写不别名 eligible global；
它不证明整个局部对象私有、下标精确或未知调用的传递效果已覆盖。
TOP 输出仍忘记该局部分配的 cells，避免精确旧 alias 保留陈旧条件/句柄。
shared random-byte intrinsic、未知 create 输出和异常 destructor 的效果缺口保持 UNKNOWN。

参考固定 Goblint `basePriv.ml` 的 ProtectionBasedPriv（933 起）的 protected/unprotected
读写与 unlock 发布，特别是内层释放不应提前发布的条件；write-centered（1801 起）
另有完整 W/P/V/L 历史。本实现使用 IKOS memory interval 与受限首次获取证书，
没有照搬这些历史域，也不能称已实现完整 Goblint PBR 或 digest-driven 分析。
定向回归见 `docs/review-repros/ai-release-values/`；全量证据见 §8。

### 3.5 未知长度字符串复制

`exec_strcpy` 为精确 AR intrinsic `LibcStrcpy` / `LibcStrcpyCheck` 建立效果：
源参数是 Read，目的参数是 Write，访问宽度未知，包含结尾 NUL。
长度未知时不尝试保留字符串值；`mem_forget_reachable` 忘记每个可能目的分配的
内存及 pointer-set bounds。旧 `mem_abstract_reachable` 只清 cells，会让之后的
pointer load 继续被陈旧分配界限收窄。返回值仍是原目的指针。

并发效果由 `publish_unknown_byte_effect` 发布：目的必须有非空、具体的 Local/Global/
DynAlloc may-set；每个目标记录任意偏移、未知完整性的字节写，使重叠 pointer 来源失效。
整数 global 按实际位宽/符号发布 TOP，aggregate global 使用失效标记清除旧初始化内容；
受影响的保护值精化撤销。TOP/绝对地址/其他不支持目标保留库覆盖警告。
源和目的仍检查合成 pthread 句柄表示，不能因支持字符串而消费损坏身份。

**“源不含 pointer”不能证明不会产生 pointer。** 合法对齐对象中，`strcpy((char *)&p, "")`
的 NUL 可改变 pointer 的最低地址字节，并产生另一个有效地址。默认效果因此发布任意
逃逸目标；只有 `byte_copy_cannot_expose_pointers` 的整 bundle 证书成立才省略这项逃逸。
证书并不省略上述内容 havoc 或字节写摘要。

该证书排除所有可能的 pointer/aggregate 消费及表示传播：扫描函数体和 initializer，
消费者地址须追溯到与目的分配不相交的直接 Local/Global 地址；formal、load、整数转换及
不唯一的定义不算直接地址。普通非 pointer Store 必须写常量到直接分配，并把这些分配
一并视为可能表示，以覆盖 `switch(copy[i])` 再写字符常量的控制依赖传播。
唯一豁免是可信前端标记、由精确 intrinsic 生成的局部合成初始化。
整数转 pointer、pointer extract、未知/间接调用、内联汇编、未闭合 transport 均拒绝证书。
允许的外部 primitive 仅为已核实的 intrinsic/API；命名 pthread/`__assert_fail` 另核对
非 vararg、参数数目、声明/实参类型及 ABI。两个扫描合用 8192 条语句预算，耗尽即拒绝。
这些条件依赖合法前端数据流、可信 metadata、有效对象/provenance 与明确库合同；
失败时发布逃逸 TOP，既有干扰与句柄覆盖检查继续阻止不充分的 SAFE。

固定 Goblint `libraryFunctions.ml:37–39` 的 dest Write / src Read、`base.ml` 的
`Strcpy`/`string_manipulation` 是参考实现依据；IKOS 此轮使用整分配保守 havoc，
没有实现 Goblint 的精确字符串数组复制。仅检查过参考源码和已有定向 TRUE 结果，
没有新增 Goblint transfer 运行探针。BMC 仍不支持复制效果所需的值/执行编码；
AI 的 SAFE 收益不能当成 BMC 已能确认字符串相关反例。
定向回归见 `docs/review-repros/ai-string-effects/`，实验范围及限制见 §8。

<a id="locks"></a>
## 4. 锁、线程身份与非并发证明

### 4.1 真实锁身份

共同 MUST mutex 需要同一具体对象、完整 byte offset、动态身份、有效成功持锁和互斥模式。
可表示性不足时不建立 MUST 锁；未知 unlock 必须撤销受影响证明。
不凭“同数据 region”“同字段布局”或哈希相同建立共同锁。
rwlock 的 shared/shared 不互斥；对写访问不能借两把 reader lock 排除。

确切共同 mutex 可直接排除候选，不必额外要求数据 region 单例；
但锁自身的具体身份仍须证明。原 self-consistency 不完整锁相等捷径已撤销。

2026-10-12 的 release 摘要域测试查出既有 `LocksetDomain::leq` 的方向错误：
mutex depth、read/joined、conditional ownership 和 fresh heap 是 MUST，
join 取交，因此 `A ⊑ B` 要求 A 包含 B 的全部事实，递归深度不少于 B。
旧 helper 却按正向包含比较，甚至不统一 TOP 与空 MUST map；
`held.leq(no-held)` 为假、反向为真，与 join 上界冲突。
局部 forward iterator 用 `after.leq(before)` 判断增长迭代是否结束，
故这不是仅影响诊断显示的问题。修复只改 MUST 比较，MAY 分量仍按正向包含。
旧头文件上的新测试失败；修复后的 100 组域上下界/相等性检查及双架构锁回归通过。
此项不表示整个混合锁/线程域及所有 POSIX 返回码均已形式化认证。
仍需独立处理的域义务包括：`is_top()` 未检查 fresh heap 分量，
`top()/set_to_top()` 的若干 MAY 历史仍使用入口空集，以及 conditional meet 对同 guard
事实覆盖而非合并。它们尚未建立新的源级 FalseSAFE 复现，不能因此当成已证明无影响。
本轮仅修复已定位的 MUST 序与添加获取历史，不将这项局部检查称为完整格认证。
配对与回归证据见 [MUST ordering](artifacts/ai-precision-2026-10-12/must-order/summary.json)。
1029 项 AI/BMC 全量逐任务判定均不变：AI TN=679，AI+BMC TP=109/TN=679，
FP/FN=0、error=1；此修复没有以标签分数增长代替域正确性。

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
多 writer、heap 槽、错误身份、数值观察及动态不透明来源保守拒绝。
10/13 typed 整槽来源已支持确切数组字节 offset/width，覆盖写仍受内存转移与表示覆盖门约束；
四个独立显式槽全部 join 的双架构最小例已 SAFE，循环重复创建已另有常量控制全实例证书，见下一段。

10/13 `ConstantThreadCompletion` 只核查唯一 main 的完整私有句柄协议：每次 create 为新代，
覆盖槽保留旧代，成功 join 只完成实际整槽来源；未知控制/表示/外部效果/预算不足撤销整份
证明。完整 return 路径和全部注册点覆盖前提成立后，相关语句各次 occurrence 的非并发集合
取交，附到 checker 的 `inactive_names`。不拓宽静态 MUST joined/live-child 格，不消费BMC模型。
有效成功 pthread 库合同、default joinable、无detach/cancel/child join/ctor/逃逸仍是限制；
不是完整 POSIX失败或任意动态池证明。真实固定数组例 UNKNOWN→SAFE，TN687→688、TP115保持。
详见 [BMC手册§12.4](ikos-bmc-complete.md#m3-design)及其小型配对证据。
detach 不建立完成/HB，sleep 不证明别的线程已结束。

`initial_main_handle_seed` 仅识别可信 frontend 的 `ikos.freeze_uninit` 初始化，
目的为 main 入口中一次分配的完整整数 local，数组计数 1，points-to 精确 singleton、
offset 0。LLVM Store 与 fresh nondet Call 都带标记，值/Alloca 身份一致、LLVM 和 AR
均相邻，宽度匹配；nondet 为声明的 canonical 无参整数 intrinsic。
唯一 main/空调用上下文、无前驱入口、无 ctor/自定义 globals/额外根、全局 initializer
无调用，以及 Store 前只有上述无副作用 nondet 调用、spawned MAY 非 TOP 且为空，
共同排除更早的线程身份产生。任何前提或预算不可证则沿用原检查。

该初始抽象写不消费旧句柄值，因而可只豁免它自身的表示告警；正常 memory 转移和
checker 的访问记录仍保留，也不更改 escaped、共享判定或之后的 TOP 告警。
这项证书不证明地址私有、不赋予普通整数句柄有效性、不拓宽 join 身份语义。
更广泛的 TOP 排除需要 pointer-load 存储的来源/布局证明：union 整数写、逐字节复制、
堆 pointer 字段经整数指针覆盖后再读取，都可生成未知指针，实际 AR 无显式 inttoptr。
私有 `read` 输入也可生成这种表示；仅检查显式地址逃逸或 pointer-return API 不足。

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
| mutexattr init/settype/destroy、非 NULL mutex_init | 受限初始化证书下的目的写、属性输入读及不确定返回抽象 | 只支持完整全局对象、main 入口前缀、glibc NORMAL/ERRORCHECK；见下文 |
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

### 5.1 受限的 pthread 初始化证书

`pthread_initialization.hpp` 由数值引擎和 checker 共用，证明真实初始化效果与后续
锁合同；没有为此新增共享状态域或更改 frontend。`certified_pthread_initialization`
要求唯一的 main 根、无 ctor/自定义未初始化 globals、不可重入 main，以及无前驱的
main 入口块。初始化调用必须位于该块最前面的调用前缀；只有 canonical 无参 nondet
可以穿插，任何 lock/create/helper/其他调用之后再出现初始化都会拒绝整项证书。
同块内初始化之后的 lock 是合法消费者，不按“同 basic block”一概拒绝。

支持的对象是原始 LLVM 声明的完整非 TLS global，透明零偏移地址可以传递。
核对 Linux GNU x86 的 ABI、外部声明/实参/C calling convention，实际 opaque 类型、
分配大小与对齐；仅将用户 buffer cast 成 pthread 指针不能通过。
attribute 大小为 4，mutex 在 ILP32/LP64 下为 24/40。
`attr_init` 至多一次，之后可保持默认 NORMAL，或用常量 `settype(0/2)` 选择
NORMAL/ERRORCHECK；`mutex_init` 每对象至多一次，输入必须是仍有效的受证属性。
初始化完成后可销毁 attribute；mutex 的销毁、重建及其他属性操作不在此子集。

证书扫描所有函数和 initializer，并与 checker 共用 `synchronization_ignored_arguments`
的精确参数表。除受证初始化对象外，对每个被省略内部效果的 mutex/rwlock/cond/spin/
barrier/once/sem 存储实参，枚举全部透明 Assignment/Bitcast/PointerShift 定义，收集
所有 global/local 分配根；未知、形参、load 或调用结果来源拒绝。create 的回调及用户参数、
once 的回调不属于这类存储。已有闭包不能仅因某个合流值带有已知对象的 may 标签而跳过
另一路来源。局部、结构体字段和数组元素一律按整个分配保守关闭。

前向 may-alias 闭包包含多定义 copy/bitcast；受证完整对象仍只允许零偏移 PointerShift，
其他 runtime 分配的派生字段地址可继续追踪。attribute 只能由受证调用消费，受证 mutex
只能由核实过的 lock/unlock/trylock 消费；其他存储只能出现在共享表的 runtime 参数位。
角色混合、普通/raw 读写、表示复制和地址发布拒绝。全局初始化仅允许 RHS 递归不含
这些对象地址的常量存储；local 的普通初始化写仍保守拒绝。

普通读写缺少直接 Local/Global 原点、全局根仅为外部声明、pointer-valued load、整数转
pointer，及未知原点的 pointer 调用实参也拒绝。外部库存储有无 AR body 的隐式写：
例如 literal puts 仍可写 FILE 表示，不能仅关闭参数表中的同步存储就声称覆盖所有库状态。
扫描/闭包预算 8192，耗尽走旧覆盖回退；依赖合法 C 对象/provenance，不涵盖越界跨分配
或库符号 interposition。对象 ABI/生命周期认证仍只支持完整全局初始化；额外 runtime
roots 的关闭没有赋予 locals/fields 更强的初始化成功语义。

源码依据为核实过的 glibc 2.39/2.42：attr init 设默认标志；settype 保留 private、
nonrobust、PRIO_NONE；上述 mutex init 因而不走协议/robust 失败分支，正常返回 0。
NORMAL/ERRORCHECK 的阻塞获取具有相应所有权合同；ERRORCHECK 的自重入失败仍由
既有重复获取 gate 处理，trylock 的零返回条件保持原样。NORMAL 的可选 no-elision 标志
不改变这项初始化/所有权前提。不能用 POSIX 泛化解释这些无失败前提。
代码仍把返回值抽象为 nondet，不由返回抽象反过来证明成功。

引擎按实际对象宽度执行 `mem_forget_reachable(dest, size)`，发布精确范围的 pointer-write
失效和 aggregate 初始化失效标记；闭包排除了任意 pointer 内容消费，不发布逃逸 TOP。
mutex_init 的属性输入保留为 Read、4 字节；目的写为 24/40 字节，其他属性目的写为 4。
checker 消费同一证书记录访问，不删除用户内存足迹。
对象、生命周期、ABI 或闭包不可证时保留外部调用 coverage，返回 UNKNOWN。
局部/字段/数组/helper 初始化、recursive/robust/protocol 及更一般 POSIX 失败是后续能力，
此轮没有把它们视为已支持。
源码指纹、native 合同与回归见 §8 的 pthread 初始化证据。

2026-10-12 的静态 ERRORCHECK 对照确认一个旧漏报：第二次 `pthread_mutex_lock`
返回 EDEADLK，不增加真实递归深度；模型却增加深度，随后一次 unlock 后仍留下 MUST，
把真实无锁写排除并返回 SAFE。glibc 的 ILP32/LP64 原生合同测试均确认该行为。
现对已经 MUST 持有的 blocking mutex 再次 acquisition 记录未覆盖的类型/成功效果，
阻止 SAFE，释放摘要也随之撤销。ERRORCHECK 竞争例与真实递归安全对照均为 UNKNOWN。
这不是完整的 mutex-type/返回码语义；恢复这种安全对照需先证明类型、初始化、成功和 owner。
见 [回归与配对](artifacts/ai-precision-2026-10-12/repeated/summary.json)。
冻结集 1029 项两种模式的逐任务判定不变：TP=109/TN=679、FP/FN=0，
UNKNOWN=240、error=1；该集合的零 FN 没有检测出这个集合外最小反例，不能代替语义审查。

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

<a id="m6-design"></a>
### M6 的准入与唯一首选试点（设计，尚未实现）

2026-10-13 重新检查剩余 15 个可信 Goblint TRUE 的任务源码及已有当版诊断：
12 个仍有 coverage 阻断；其余 3 个是两个 heap 节点/数组槽所有权与发布问题、一个动态
join 完成问题。**尚无一项已经建立“语义覆盖完整，纯数值合并导致 UNKNOWN”的证据。**
因此暂不为它们实现泛化 CEGAR；这不等于证明反馈永远无用。逐项来源、hash和缺口见
[设计依据](artifacts/strategic-design-2026-10-13/review.json)。

首选受限试点为 **BMC 依赖提示选择局部控制分区，AI 从初态重新求完整不动点**。
实现基础已经存在：`memory::PartitioningDomain` 按整数变量维护分区，
`numerical.hpp::exec_ikos_partitioning_var` 调用 `partitioning_set_variable` 选择分区变量。但是
`value::AbstractDomain` 是 Data×Lockset，分区仅在 Data 内；共享黑板仍主要为区间和
锁索引区间。这条小路线只能先解决局部值合并造成的不可达访问，不能恢复锁—值相关性、
heap 所有权或关系式干扰。把数值域换强不自动解决这些问题。

| 环节 | 下一轮可执行的要求 |
| --- | --- |
| 准入 | 先找到一个实际任务：访问/API/对象/线程覆盖成立，保存合并前后状态，确认首次精度损失；实际 AR 保留目标分支。现有 15 项尚未入选 |
| BMC 输出 | 新增 advisory `LocalPartitionHint`：源/AR/配置身份、候选、查询状态/界限、控制和数据依赖、局部 selector 与作用区域。当前没有可直接消费的完整拒绝依赖协议，需先实现导出；UNSAT 时没有 SAT model，不编造模型值 |
| AI 消费 | 同一不可变 IR 的分析策略 side table；在 selector 定义后、相关分支和精度损失之前设置分区键，让新一轮分支转移/join 保留相关性。现有 API 不会拆开已合并状态。新 Context/共享环境从初态重算；提示不注入 assume、不把查询状态当 MUST |
| 首版界限 | 一轮重算、每函数一个稳定局部整数 selector、一个无环区域；按现有域的实际不交区间片段计数，总数不超过四。首例先证 unsigned selector∈[0,2]；一般余值也须覆盖，其补集可能需要多个区间，超限以 join 合并 |
| 失效与回退 | 源/hash/位宽/定义或上下文不符则丢提示；共享 load、逃逸、selector 重定义等超出首版范围则拒绝或合并。保留初轮 UNKNOWN，不因失败返回 SAFE |

保持性义务为：每次拆分穷尽其输入，覆盖全部真实初态和行为；每个转移/join/widening保持覆盖。
精化后的不变量无需包含旧凸合并产生的虚假状态，否则就无法恢复精度。所有分区
产生的写和线程入口都进入环境；整个线程模块化外层重新收敛，末轮 dirty 仍按既有规则
回退。提示在一次分析内固定，分区数有限并沿用 sound widening，重算次数有限。
公共持锁可以排除竞争，却不能排除相应 writer 的干扰（Digest 作者扩展版 §5.1 Remark 3）。
不把先前一次 BMC 的写集合当完整环境，也不借旧证书跨摘要变化。

最小诊断设计：三个局部分支在合并处分别令 `(b,x,y)` 为 `(0,0,0)`、`(1,1,2)`、
`(2,2,1)`，随后仅在 `x==1 && y==1` 时写共享变量，另一线程无锁写它。
区间/凸合并包含 `(x,y)=(1,1)`，实际三分支都不满足；按 b 保留分区可表达这一差别。
将第三支改为 `(2,1,1)` 就成为真实竞争控制。**这是未编译、未实测的诊断设计**；若编译器
已经消除目标分支，就不能拿它宣称 AI 恢复精度。遗漏第三分区、错误 selector、陈旧 ID、
共享 selector、预算为零等坏提示都须保持覆盖；另做改名、宽度和已支持 helper 变体。

试点必须与普通强域 portfolio、无条件分区、纯语法/依赖选区，以及“运行 BMC 但丢弃提示”
比较，包含第一轮查询和重算的全部成本。只提高合成例、被更简单策略等成本覆盖、或一直
没有真实抽象损失入口，则记录负结果结束该试点。找到实际入口且控制/全量/保持性通过后，
才在未用于选择机制的任务族上检验泛化；不为完成“双向”叙事强行开展算法工程。

<a id="publication"></a>
### 论文论点与汇报提纲

整体阶段和比赛交付优先级见 [README 的 M0–M7](README.md#roadmap)。
当前研究主线为：**线程模块化 AI 覆盖行为并生成保守候选/受限事实，
按需 BMC 建立有限真实竞争执行，再独立检查所选 AR 执行。**
“AI + BMC”本身不是新颖性；须从具体接口、保持性条件或有效通用机制中确立贡献。
真正双向反馈仍属 M6 的待验证节点，已有默认关闭原型没有冻结集 TN 收益。

| 可以提出的论点 | 已有依据 | 论文仍需补齐的证据 |
| --- | --- | --- |
| SAFE 与 FALSE 的证据合同分离 | 本节三类查询、架构与 BMC 合同；当前 AI/组合配对见 README | 明确支持的 C→AR、位宽、SC/pthread 和库合同，分别论证覆盖、候选过滤与执行保持；有限 FP/FN 观察不代替证明 |
| AI 信息确实影响竞争确认工作 | helper 锁/数据证书减少两个 funloop 的无效查询；helper 发布事实组恢复确认 | 逐事实消融和依赖有效性；事实组一起关闭不能单独归因生命周期机制 |
| AI 事实有局部成本收益 | no-wrap 最小例三轮交替的端到端/solver 对照，见架构 §5 | 比赛任务重复测量、进程树内存和独立任务集；其他样本基本持平，不外推整体提速 |
| 确认结果可独立检查 | 当前所选执行验收和损坏证据校准，见 BMC 执行检查章节 | 源级 witness 映射与可执行 validator 闭环；写清 frontend/snapshot/API 可信边界，不称全语言认证 |
| 新的保护/来源摘要或反馈机制有效 | M2/M3/M6 是候选研究方向，尚非已获贡献 | 原始论文及固定实现的近邻比较、保持性论证、机制消融和泛化；正确移植已有 PBR 不自动成为新算法 |

每次汇报按以下顺序组织，缺少结果的部分明确写“待验证”，无需新增报告文档：

1. **问题与范围**：no-data-race、支持的内存/库语义；为什么保守 AI 警告与真实竞争有距离。
2. **架构与贯穿例**：从 C/YAML 到 AI 状态、候选、BMC 值流/线程实例及独立执行证据。
3. **具体机制与保持性**：一次抽象信息损失，通用修复、成立前提与失败回退。
4. **实验**：同配置基线、逐任务迁移、机制/事实消融、成本与 witness 状态。
5. **近邻和贡献边界**：与 Miné、Goblint PBR/cluster/digest、Dartagnan 编码及 path focusing 的差别。
6. **限制与下一节点**：残余 UNKNOWN、未支持语义和外部验收，区分比赛进展与研究结论。

**本轮主研究问题（待证假设）**：线程模块化 AI 已支付的分析成本，能否通过作用域明确的
候选排除/执行证书，在保留真实竞争执行的条件下，减少按需 BMC 的端到端确认成本？
候选贡献是这类信息的生产—消费—失效合同及有效机制，不是笼统的“AI+BMC”。
M6 是独立备选研究线，不能成为主线交付的前置条件。

已对照原文的最近邻限定如下；这不是穷尽文献检索，也没有认证新颖性：

| 原工作与本轮阅读 | 已经解决的部分 | 对本项目论点的约束 |
| --- | --- | --- |
| Miné LMCS2012 §3.2.3/Thm3.2、§3.3；SAS2021 privatization §2–5 | 干扰语义过近似真实交错；副作用约束向其他分析未知量发布贡献，以最小解或保守后解表达线程模块化分析 | MAY 干扰值不保证存在供值执行；正确移植环境闭包/privatization 不是新算法 |
| [Dartagnan CAV2019](https://hernanponcedeleon.github.io/pdfs/cav2019.pdf) §1、§3 | may/active 关系的双向依赖计算，结合 Andersen may-alias 压缩 CAT 的 SMT 关系编码；差集右操作数不能随意过近似 | 静态分析、按需关系或 alias 加速 BMC 已有先例。需证明 IKOS 的语义/上下文信息带来该配置尚无的有效收益 |
| [CAAT OOPSLA2022](https://hernanponcedeleon.github.io/pdfs/oopsla2022.pdf) §1 | 惰性一致性理论求解；原文也比较 Deagle 的特定内存模型理论 | 不能把 Dartagnan 描述为只有全量急切 SMT；公平比较要固定实际 engine/method，而非只选旧慢配置 |
| [Haas et al., OOPSLA2023](https://doi.org/10.1145/3622855), *Static Analysis of Memory Models for SMT Encodings*，作者原文 §1–§2 前部，PDF pp1–7 | 通过 filters 导入外部程序分析，计算 execution-relative may/must 关系界及执行蕴含，按需简化关系编码 | “外部 AI 事实简化 BMC”已有直接近邻；需要界定具体执行补全的不同对象、合同及增量收益 |
| [ESOP2023 clusters](https://doi.org/10.1007/978-3-031-30044-8_2) §3/Fig3、§4；Digest VMCAI2026 作者版 §3/§5 | 具体 return trace 按线程 ID 发布并在 join 合并；关系集群和 digest 保守抽象历史兼容性 | join 的具体完成来源已有语义基础；抽象兼容不保证具体 trace 共存，局部 Data 分区不是完整 digest 算法 |
| SAS2011 path focusing §3.1–3.4 | SMT 选取逃出当前抽象状态的切点路径，全部闭包与 widening 支持归纳和终止 | BMC 辅助 AI 已有先例；候选有界 race UNSAT 不具有原算法的完整切点路径/环境覆盖 |

固定 Dartagnan 的 `relationAnalysis=lazy` 与 `method=caat` 是两个设置；当前比赛配置的
`method=eager` 不等于特意选择旧慢算法。OOPSLA2023 §1 也说明某些派生关系上的性质
需要较多 eager 编码；比较必须使用能够处理目标性质的实际配置。

<a id="auxiliary-research"></a>
**核查后的研究方向：AI 指导候选所需的辅助执行补全（2026-10-13）。**
这里的辅助线程是程序实际创建、但不承载两个目标访问的线程；它可能为目标路径提供
共享值、同步或 join 完成。不是新建分析用线程，也不是 Goblint 约束系统中的 helper unknown。
“有副作用”指真实读写/同步；SAS2021 的 side-effecting constraint 则指约束右侧向其他
分析未知量发布贡献。可以联系二者的**生产者—观察者依赖**，不能等同这两个概念。

联系有原文依据：Miné §3.2.3 说明，即使使用具体值集合，忘记执行次数的干扰语义也能
让两个无循环增量生成任意正数；摘要取值不能作为 FALSE 的存在性证据。
ESOP2023 §3/Fig3 将实际终止 trace 发布到 `[i]`，join 取匹配 ID 的返回 trace；
§4 的抽象版不保证摘要内某个值具有可组合的具体执行。
Digest 的兼容关系同样是过近似。因而可借鉴依赖索引与历史条件来**选择要补的执行**，
最终仍须重建它的实际事件。Goblint 固定 revision
`bb55b70d91290992629ba3d073185c10cdf356e6` 的 `analyses.ml::sideg`、
`td3.ml::side` 和 `basePriv.ml` 的发布转移也符合这种区分。

实际入口是 `thread-join-array-const-race.i`：main 创建四个 writer，前三个成功 join，
随后读取 data，第四个 writer 的写是另一目标。前三个有锁写入/返回都是 main 到达目标
所需的辅助执行。只添加“完成”边而丢掉它们的写会破坏值源；把 MUST joined 的静态摘要
当作某个孩子的有限终止路径也不成立。这个入口能检验能力，**不能单独检验线程裁剪提速**：
三个被 join 的孩子都必须补全。

当前 `AccessRecord` 已有 MAY 对象/字节范围、语句/上下文、MUST 锁和创建/join 摘要，
共享环境也有 API/未知效果门。它们可成为生产者查找的输入；尚无实例关联完整的效果
摘要或可直接提升为执行的返回摘要。既有常量控制完成证书用于 SAFE 的全实例排除，
不包含孩子的读写 trace，不能充当 BMC 执行。可实施的层次如下：

| 层次 | 所需机制 | 证据与当前边界 |
| --- | --- | --- |
| 具体补全 | 有限 trace 集；实际 create occurrence/实参/handle；每个成功 join 对应完整返回路径；统一值流、地址、锁和生命周期 | 源码接口可扩展，具体实验与实现义务见 [BMC 辅助执行核查](ikos-bmc-complete.md#auxiliary-feasibility)。生产 BMC 尚未支持有副作用辅助完成 |
| AI 指导依赖 | 从目标 guard/地址/读值、join、同步和生命周期查找可能生产者，迭代加入它们的依赖；未知效果保留候选或回退完整受支持展开 | 属于待实现假设。先做搜索优先序；要裁剪则另证保存所需的受支持有界执行，不能仅用“不与目标地址 alias”删除线程 |
| 可展开的结果复用 | 复用已检查的有限事件模板，在新实例/输入/内存/同步语境下重新绑定并核验 | 可选后续假设；MAY 摘要缓存不是执行缓存，不以此作为首版前置工程 |

首版拟论证的受限命题是：在固定 AR/库/SC 合同下，所有 trace 组成同一个合法有限调度，
vector clock 准确刻画所支持同步的 HB，目标是两个不同动态实例的实际冲突访问且
vector clock 不可比，则该前缀证明竞争。
创建/实参、成功 return→join、每次 load 的真实值源及字节对象存活均是前提。
SAT 相邻时间不是这些前提的替代物；扩大到多个 trace 后须计入辅助线程形成的 HB 链。
独立 AR ACCEPT 能检验**所选前缀**，不证明 C→AR、全部编码模型或全部 SAFE 规则。
当前 AR 检查是离线验收，`check_race` 仍由 SAT 产生 CONFIRMED；不能宣称已有在线验证门。
该条件性命题不等于完整实现的保持性证明。AI 仅排序与语义裁剪要分开：错误排序在
预算内可能丢确认；若声称裁剪保留所有相关有界执行，还需完整 MAY 依赖覆盖以及被
省略行为的投影/扩展论证。这比“本次接受的执行真实”更强，有界 UNSAT 仍不证明 SAFE。

研究假设细化为：**已付出的线程模块化分析能否识别具体供值/完成依赖，使有界执行
重建覆盖更多候选或降低相同语义、界限下的确认成本？** 补全本身是能力扩展；论文增量
须由 AI 信息的有效选择、保持性合同及可复现实验确立，不能把借鉴 join 语义称作新算法。
这仍是 AI→BMC；M6 的 BMC→AI 新不动点是另一条尚未验收的路线。

实验先比较旧管线/具体补全的执行覆盖，再在**同一补全实现**中比较 AI 指导开/关，
以及保留相同语义和界限的完整有界展开。增加可不调度线程/替代供值线程的任务族后，
才能测依赖选择；全 join 例不冒充裁剪例。控制包含漏写、假返回、错误实例、反向 guard、
覆盖 handle、传递 join、同步造成 HB、循环依赖及未知 API，另做宽度/改名/控制流变体。
记录有效/拒绝执行、辅助实例/事件、闭包轮数、读源边、公式规模和全部 AI/solver/检查成本。
只增加预算、缩小语义或改变答案取得的时间不能归因于优化。
若 AI 指导无收益或近邻已完整覆盖增量，保留经验证的能力扩展，调整为覆盖/工具贡献；
不强行宣称新颖性、普遍提速或已实现双向分析。

M5 先固定如下实验矩阵，每行只回答一种因果问题：

| 对照 | 固定内容 | 能回答什么 |
| --- | --- | --- |
| AI-only / AI+BMC | 同一前端、AI、任务与总资源 | 增量确认能力、全管线代价，以及 TN 是否回退 |
| bounds / no-wrap 分别开关 | 相同生命周期证书、语义、界限、旧查询优先序 | 范围约束与定义性简化各自的成本；测 solver 与端到端，不能只给一个快例 |
| single-execution/新实例机制开关 | 其他事实固定，记录支持范围差异 | 能力贡献及其成本；关闭后 UNKNOWN 不算同能力的速度对照 |
| 某一候选排除机制开关 | 其余保守过滤、实际执行与预算固定 | 减少候选/查询的效益；完整记录新找到/未找到的执行，不能把不同答案只汇成时间比 |
| 固定版 Goblint / Dartagnan | 输入/YAML/ABI/性质、机器/总资源；另列实际库/内存语义、配置与内部界限 | 工具能力和实际成本；Goblint may-warning 不算具体 FALSE，参考另一个 FAIL 不核验 IKOS 所选执行 |
| M6 四类控制 | 同一 IR、环境覆盖、总资源，费用包括重算 | solver 指导是否优于普通强域/语法选区，见上节 |

旧总开关 `IKOS_BMC_DISABLE_AI_FACTS` 还关闭生命周期来源；M3.0已按 [BMC §3.1](ikos-bmc-complete.md#architecture)
提供独立因素开关并验证范围。没有现成的“完全移除 AI 仍同等功能的 IKOS BMC-only”，不能制造这个基线。
新机制的能力实验与旧语义下的优化实验分别报告。

先冻结配置和任务族：2026 集已经参与开发，不能重新随机划分后称盲测；2027 新增任务或
另一独立来源也要排除同源参数变体/近重复并记录首次查看时间。变形回归验证通用性，
不代替独立工作负载。语义可比子集按事先公开的 API/模型规则选取，并同时保留全任务视图，
不能按谁更快或谁答对事后筛选。

计时串行错峰，预热与正式运行分列，至少五轮交替/随机顺序，固定核心/内存/超时，
记录硬件、版本、配置、进程树 CPU/峰值内存。报告每任务中位数及波动、同答共同完成项
时间比、全部任务 solved/timeout/error 与不同答案；超时是删失结果，不能当精确耗时
参与“普遍加速”均值。记录 AI/展开/编码/solver/证据成本与候选、事件、读源边、公式规模；
现有顶层 assertion 数不等于 SMT AST 大小。新增仪表先核对开关不改判定。

论文继续门槛是：至少一个通用机制有保持性论证、隔离因素后仍有效、收益不完全来自
支持更少语义/增大预算，并在独立任务族复现。如果无成本收益但有经验证的能力扩展，
据实转为精度/覆盖贡献；若近邻已覆盖且没有可辨认的增量，优先工具/经验短文，
不声称首次双向分析、全面优于参考或普遍 soundness。原文版本、阅读范围与代码证据见
[设计依据](artifacts/strategic-design-2026-10-13/review.json)。

研究论文出口是可辨认的新颖贡献、精确条件下的论证、公平实验和可复现 artifact，
不要求 UNKNOWN 清零或双向反馈成功。若新颖性尚不足，先完成如实描述能力的参赛工具/经验短文；
官方参赛贡献论文的 12/17 节点不等于完整研究论文的投稿期限，更不等于录用保证。

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

<a id="m2-exit"></a>
### M2 第一版出口（2026-10-12 已完成）

**完成的是约定的受限机制验收与剩余入口处置，不是全部 TN 能力或全部 SAFE 规则证明。**
稳定发布、release 端值、字符串效果、受证 pthread 初始化、初始句柄 seed、首次发布 HB
均已分别提交并复现收益；MUST 偏序、重复获取边界和未知目的 Store 的正确性修复也已验收。
最后的只读 pointer-array 坐标已实现，最小正例恢复精度，但冻结集未增加 TN。
本节以下保留各机制原理与历史分阶段证据，读者可按需要展开。

从稳定发布前的同一 1029 项基线到当前，净转移恰为 **9 项 UNKNOWN→SAFE**：
TN 678→687，TP 109 保持，FP/FN=0，UNKNOWN 241→232，error=1。
没有新增退回 UNKNOWN 的冻结任务；每阶段有单独配对，不能把一次端到端计时当提速证明。
最后一版 build/install 同步、64 次新双架构对照、216 次相关回归、20 次 P0、
AI/BMC 全量与 109 项/137 条独立 AR 所选执行检查通过。
详细矩阵、逐任务净转移和每个剩余入口的源码片段在
[阶段验收记录](artifacts/ai-precision-2026-10-12/readonly-array/completion.json)；
当前可执行指纹在 [README](README.md#current)。

安全 UNKNOWN 仍有 106 项。24 个可信参考 TRUE 的入口恢复 9 个，余下 15 个已重新诊断
并阅读具体程序；它们尚缺的机制如下，均保留 UNKNOWN，而不是标成“不能实现”：

| 入口数量 | 已检查/尝试的具体路线 | 尚未满足的语义前提 |
| ---: | --- | --- |
| 1 | `list2_nr` 的 A/B 根发布与 next 字段源码/报告 | pre-create 多级 heap field 来源闭包，不能仅有根 pointer 证书 |
| 1 | `arraylist` 的 array 坐标已实现；完整/仅初始化前缀的 debug 对照 | 跨线程可写 next 字段的值、表示、干扰和生命周期覆盖，sticky gate 仍有效 |
| 4 | arraylist/arraycollapse/arrayloop/nested 的源码、固定参考及当前候选 | slot-index → 可达对象区域 → 实际 mutex 的对应；须证无跨槽 alias 和动态实例所有权 |
| 8 | race_reach 链表分配/发布/遍历源码及当前候选 | 10000 次动态创建使现有 lifetime-once 证书不适用；需逐对象 freshness 与首次发布闭包 |
| 1 | join 循环、显式 array 展开、独立 scalar 句柄及缺失/覆盖 join 对照 | 全部动态实例完成证书，加上非零槽偏移的 opaque handle 来源；单纯展开循环仍 UNKNOWN |

其余 91 项已在修正 GCC32 的固定 Goblint 上重跑：UNSUPPORTED 30、UNKNOWN 25、
TIMEOUT 34、ERROR 2。14 个带 Unsound 的 raw TRUE 已另试后续配置，仍无新增可信 TRUE。
这些结果是记录预算内未证实，不是安全性质否定或能力不可能结论；旧来源诊断在台账中
明确标为历史快照，未伪称本轮重新做过所有 transfer 探针。

Goblint `region.ml` 的 bullet/region 查询依赖它自己的 allocation/escape 状态；
`threadJoins.ml:50–62` 只把 singleton 且 unique 的 TID 加入 MUST joined，
不能只移植“join 后安全”的结论。固定源码指纹已记入阶段记录。
本轮 join 回归证实：四个独立 scalar 句柄可 SAFE，四个显式 array 槽在本轮当时因非零偏移来源门
保守拒绝（10/13 M3.2第一增量已解决，见BMC手册§12.4）；原循环在该次阶段仍缺四个动态实例的完成覆盖（10/13常量控制证书另已验收）。缺最后 join、覆盖旧句柄的负例均未 SAFE。

因此按原路线可进入 M3 的有限语义/成本优化；更一般 heap-region 与动态 join 是后续 TN
专题的具体入口，已有源码、观测和前提，无需靠放宽 gate 强行完成。
依赖稀疏化、普遍关系域增强及真正 BMC→AI 归纳精化也未在 M2 中实现。
官方源 witness 验收、全语言 soundness 与公平优势实验继续按各自节点处理。

### M2：稳定指针发布（2026-10-12 已实现）

`race-3_1-container_of-global.i` 的发布值是 main 栈对象的字段指针，ILP32 偏移为 24。
原有 pointer board 的 NULL 本身以 bottom offset 表示；不能说它把 offset join 成了 `[0,24]`。
损失发生在读取时保留初始内存、与 may-write 内存合流，子线程因而不能确定
`container_of` 后的锁地址。原始诊断显示 callback 的锁取得数为 0；修复后该入口为 SAFE。

实现见 `execution_engine/stable_publication.hpp`、`on_load_pointer_restore` 和
`ConcurrentGlobalEnv` 的 publication writer board。它先证明整个 bundle 中的 slot 地址
只用于整宽 pointer load/store 和透明零偏移别名；只有一个非 initializer writer，
writer 函数生命周期至多调用一次，所在 BB 不在循环中。目标 worker 的全部地址使用
必须是同一 BB、writer 后的 literal pthread_create；普通调用、独立 entry、逃逸及其他 writer 拒绝。
AR 形参被重新赋值也拒绝，避免把隐式入参定义错当单一定义。

所有到达 writer 的 RHS 逐次 join/widen，并以 dirty 驱动全局重算。
认证 worker 对整槽的 singleton、offset=0 读取才强恢复该 writer 摘要：真实子线程一定在
发布后运行，且没有后续覆盖，所以排除初始化值有依据。未知目标写、重叠字节写、
未覆盖调用/创建撤销精化；最终检查中新变化仍走既有 UNKNOWN 出口。
静态证书按 slot/worker 缓存，运行期摘要和失效条件不缓存为永久真。
未经缓存的试跑导致三个 Weaver SAFE 超时，已拒绝该版本；缓存版恢复原判定。

1029 项同配置配对仅有该入口 UNKNOWN→SAFE：AI TN 678→679；AI+BMC
TP 109 不变、TN 678→679、UNKNOWN 241→240、FP/FN=0、error=1。
双架构/改名/宽度变体 44 次、既有 pointer-byte 对照 72 次和 P0 均检查。
新 FALSE 为 0。fixture CASE6 的 BMC FALSE 在基线与当前均为独立 AR
`UNSUPPORTED: load representation/type`，明确作为旧类型边界，不能称已验证竞争。
计时只是一轮墙钟观察，不构成性能优势结论。

证据：[汇总](artifacts/ai-precision-2026-10-12/stable-publication/summary.json)、
[逐任务配对](artifacts/ai-precision-2026-10-12/stable-publication/paired.json)、
[回归](artifacts/ai-precision-2026-10-12/stable-publication/fixtures.json)。
本地原 Goblint 的 CIL 缺 GCC32 machine definition；重建独立 CIL/Goblint 后，
原 24 个候选全部重新得到不带 Unsound 的 TRUE，见
[参考记录](artifacts/ai-precision-2026-10-12/stable-publication/reference-24.json)。
这不是官方 2027 Goblint binary 的成绩。

### M2：保护相关 release 端值（2026-10-12 已实现）

两个入口均由 UNKNOWN 恢复 SAFE。`13-privatized_03-priv_inv` 的 I 为 1，
main 在同一 mutex 下写 0 再写 1，真实 R 为 1，因此 worker 的 `x==1` 恒真，
解锁后的 `x=-1` 不可达。`28-race_reach_60-invariant_racefree` 的 I 为 0；
外层 mutex 排除 worker，内层 global lock 释放 x++ 的中间值时，R 仍带外层锁，
不污染 worker；外层释放时 x-- 已恢复 0，因此 worker 的 `x==0` 恒真。
修复 ILP32 环境的固定 Goblint 在两个源输入上也报告对应条件恒真及危险分支 dead code，
见 [参考观察](artifacts/ai-precision-2026-10-12/release-values/reference-targets.json)。
这些是实际断言/不可达诊断，不冒充 transfer-function 的逐次运行 trace。

机制和覆盖条件见 §3.4；原 `record_privatized_write` 仍属于 may-write 通道，
新 R 只在真正 unlock 转移产生，没有把旧队列重新命名为 release 证明。
`pthread_join(..., NULL)` 不触及共享标量，不再无条件撤销新摘要；
局部输出的来源证书与 memory havoc 同时保持，TOP 共享输出仍走 coverage gate。

新回归 72 次（ILP32/LP64、AI/BMC、改名/宽度变体及 exception bitcode），
含嵌套锁、迟到未保护写、不同 mutex、条件锁、模糊 unlock、partial write、
cond_wait、TOP random-byte/create 输出和局部/零长度对照。
其中 10 次 FALSE 的所选执行均为独立 AR ACCEPT；获取历史、R 嵌套过滤、迟到标签 dirty
及局部来源/formal/load/多定义边界另有域/AR 断言。原有读干扰、写并集、条件锁、锁身份、
稳定发布、mutex attributes 和双架构 P0 检查通过，旧 pointer fixture CASE6 类型边界仍如上保留。

1029 项同配置全量只有两个入口 UNKNOWN→SAFE：AI TN 679→681；
AI+BMC TP 109 不变、TN 679→681、UNKNOWN 240→238、FP/FN=0、error=1。
所有 109 个标签 TP 重新导出并检查 137 条所选执行，全部独立 AR ACCEPT；
仍非官方源 witness 验收，也不证明所有 SAT model。
AI/BMC 本轮墙钟为约 165/379 秒，只有一轮观察，不能推出性能优于旧版或参考工具。

证据：[汇总](artifacts/ai-precision-2026-10-12/release-values/summary.json)、
[逐任务配对](artifacts/ai-precision-2026-10-12/release-values/paired.json)、
[定向回归](artifacts/ai-precision-2026-10-12/release-values/fixtures.json)、
[所选执行](artifacts/ai-precision-2026-10-12/release-values/selected-executions.json)。

### M2：字符串内存效果（2026-10-12 已实现）

`bigshot_s.i` / `bigshot_s2.i` 的 main 先 join 分配线程，再创建、join 复制线程，
最后读 `v[0]`；创建/join 历史已能排除并发，原先仅 `strcpy` 库效果未覆盖而 UNKNOWN。
固定 ILP32 Goblint 对相同 `.i` 输入均返回 TRUE；空指针/断言诊断属于其他性质，
此轮只评估 no-data-race。实现是 §3.5 的通用字节效果及受限无新 pointer 逃逸证书，
没有按输入名删除警告，也没有新增字符串精确值域。

68 项双架构、AI/BMC 定向检查包含并发复制、结尾 NUL、非零字段偏移、多个 may-target、
修改共享整数守卫、TOP 目标、句柄字节写、mutable 源写冲突及 fortified 复制；
32/64 位还有改名和不同守卫宽度变体。6 次 native 控制实际验证 NUL 改变 global/heap
pointer 的有效表示及字符/控制依赖到 `%p` 的重建。格式化/解析用例同时受既有库覆盖
gate 限制，其 UNKNOWN 不能单独证明新逃逸证书的拒绝分支有效。
已有六组相关回归 448 项及双架构 P0 20 项通过；stable-publication CASE6 的两条执行
仍是既有 AR `UNSUPPORTED load representation/type`，不包含在“全部 ACCEPT”声明中。

1029 项全量配对只有两个字符串入口 UNKNOWN→SAFE：AI TN 681→683；
AI+BMC TP 109 不变、TN 681→683、UNKNOWN 238→236、FP/FN=0、error=1。
109 个标签 TP 重新导出的 137 条所选执行均为独立 AR ACCEPT。
两模式全量 wall time 为 155.86 / 355.39 秒；工作站运行非隔离且短时并行了定向检查，
这些是运行记录，不作为提速结论。BMC 对复制效果仍保留 unsupported 边界。

证据：[汇总](artifacts/ai-precision-2026-10-12/string-effects/summary.json)、
[逐任务配对](artifacts/ai-precision-2026-10-12/string-effects/paired.json)、
[定向回归](artifacts/ai-precision-2026-10-12/string-effects/fixtures.json)、
[所选执行](artifacts/ai-precision-2026-10-12/string-effects/selected-executions.json)。

### M2：受证 pthread 初始化（2026-10-12 已实现）

`28-race_reach_23-sound_unlock_racing.i` / `40-trylock_racing.i` 的共享 data 始终由
独立 `__global_lock` 保护。属性在 main 入口中初始化、选择 ERRORCHECK、用于初始化
其他 mutex，然后才创建线程；原 UNKNOWN 是这些外部调用的效果缺口。
固定 ILP32 Goblint 对相同 `.i` 新复核均 TRUE；本轮核对其库分类、attribute 赋值源码，
没有运行 transfer 探针。Goblint 的 `sem.lock.fail` 默认 false，`MutexAttrDomain.of_int(2)`
返回 TOP；它的 TRUE 是交叉参考，不是 IKOS 初始化成功前提的证明。
真正依据为 §5.1 的 glibc 原实现、ABI/native 检查与完整存储使用证书。

新回归 156 次（39 用例 × ILP32/LP64 × AI/BMC），修改前后 C 输入相同；
包含 NORMAL 默认/显式设置、ERRORCHECK、公用锁与多定义 unlock 正例，以及
未检查 trylock、不同锁、重复获取、局部/helper/循环/迟到初始化、raw 表示修改与复制、
多定义别名写、aggregate 发布、pointer load、robust/protocol 和 constructor 等边界。
unsupported 例仍有真实 coverage 原因；4 次 FALSE 的所选执行均为独立 AR ACCEPT。
新增原始存储回归覆盖默认/结构体/数组 mutex、局部发布、spin/rwlock/cond/once/sem，
以及外部 FILE 表示读取；结构体/数组默认 mutex 的普通受锁数据访问对照仍是 SAFE。
提交前的初版曾对默认 mutex 的 owner 字节读误报 SAFE：只关闭新初始化对象遗漏了
另一把锁的内部写。glibc 中非原子 owner 写与并发 unsigned-char 表示读确实可竞争，
不是非法初始化或越界。扩展整个 runtime 存储闭包后回退 UNKNOWN；其他控制对象
用例验证能力边界，不将每个边界用例都称为已证明真实竞争。
2 次 native 控制确认大小、标志、初始化/销毁返回及 ERRORCHECK 的 EDEADLK 行为。
已有八组相关回归 338 次、双架构 P0 20 次通过；旧 stable-publication CASE6 的两条
AR `UNSUPPORTED load representation/type` 仍保留，不称其已独立验证。

1029 项全量只有上述两项 UNKNOWN→SAFE：AI TN 683→685；
AI+BMC TP 109 不变、TN 683→685、UNKNOWN 236→234、FP/FN=0、error=1。
重新导出的 137 条所选执行来自 109 个标签 TP，全部独立 AR ACCEPT。
AI/BMC 本轮 wall time 为约 154/356 秒，非隔离单轮观察，不作性能优势结论。
该子集未修改 BMC 属性/失败编码；BMC 仍按自己的生命周期边界决定是否确认 FALSE。

证据：[汇总](artifacts/ai-precision-2026-10-12/pthread-initialization/summary.json)、
[逐任务配对](artifacts/ai-precision-2026-10-12/pthread-initialization/paired.json)、
[定向回归](artifacts/ai-precision-2026-10-12/pthread-initialization/fixtures.json)、
[runtime 原始依据](artifacts/ai-precision-2026-10-12/pthread-initialization/runtime-contracts.json)、
[存储闭包反例](artifacts/ai-precision-2026-10-12/pthread-initialization/storage-boundary.json)、
[所选执行](artifacts/ai-precision-2026-10-12/pthread-initialization/selected-executions.json)。

### M2：可信初始句柄 seed（2026-10-12 已实现）

`09-regions_06-ptra_nr.i` 只有一个 coverage 告警，来自 main 最前面的合成 `pthread_t`
初始化 Store；之后的普通 data 访问已由 `A_mutex` 和启动顺序覆盖。
共享黑板的未知逃逸来源使旧 `is_escaped(local)` 无法证明私有，从而把这个无旧身份值
消费的 frontend 写也当作句柄表示使用。新机制为 §4.4 的局部 seed 证书，只有该 Store
表示告警可被豁免；不把逃逸 TOP 降级，不更改普通读写的内存或竞争访问记录。

对 `ptra_nr.i` 和 `arraylist.c` 重新运行固定 ILP32 Goblint，均为 TRUE；没有新 transfer
探针或官方 2027 binary 验证。`arraylist` 的 seed 告警消失，但三处 TOP heap Load
告警仍在。原先考虑的“句柄地址不显式逃逸 + pointer-return API 白名单”被放弃：
volatile union 的整数写再 pointer Load、堆 pointer 字段经整数指针覆盖的实际 AR
都没有 inttoptr；heap 整数 alias 案例只作 AR 边界，不依赖其有效 C 语义。
合法的字符表示复制与私有输入也可重建未知指针。
要排除这些读写，须证明 pointer-load 存储的类型/字节来源或恢复保守的精确 points-to。

48 项双架构/改名 AI/BMC 同源 A/B：安全控制从 UNKNOWN→SAFE，无锁控制不报 SAFE；
数值观察、创建后覆盖、字节/表示复制、worker 参数逃逸、任意 pointer 来源、union、
字节重建、heap 整数覆盖和私有输入保留 UNKNOWN，且仍有句柄表示 coverage 原因。
已有八组相关回归 524 项、双架构 P0 20 项通过。旧 stable-publication CASE6 的两条
AR UNSUPPORTED 仍保留，不包含在所选 FALSE 的 ACCEPT 声明中。

1029 项配对只有 `ptra_nr.i` UNKNOWN→SAFE：AI TN 685→686；
组合 TP=109 不变、TN 685→686、UNKNOWN 234→233、FP/FN=0、error=1。
109 个标签 TP 重新导出的 137 条所选执行全部独立 AR ACCEPT；该范围不等于官方
源 witness 验收或全语言证明。两种全量模式约 157/356 秒，非隔离单轮记录，不作提速结论。
frontend 文件、BMC 编码及 join 身份规则均未改。

证据：[汇总](artifacts/ai-precision-2026-10-12/handle-seeds/summary.json)、
[逐任务配对](artifacts/ai-precision-2026-10-12/handle-seeds/paired.json)、
[定向回归](artifacts/ai-precision-2026-10-12/handle-seeds/fixtures.json)、
[参考输入复核](artifacts/ai-precision-2026-10-12/handle-seeds/reference-targets.json)、
[AR 来源边界](artifacts/ai-precision-2026-10-12/handle-seeds/pointer-origin-boundaries.json)、
[所选执行](artifacts/ai-precision-2026-10-12/handle-seeds/selected-executions.json)。

### M2：未知目的 Store 的正确性边界（2026-10-12 已修复）

继续定位 `arraylist` 的 TOP heap 来源时，最小用例发现一个冻结集之外的真实 FN：
`selected` 初始指向 `left`，main 经未知 `int **destination` 写入 `&right` 后创建线程，
一个线程执行 `*selected=1`，另一个执行 `right=2`。合法选择
`destination=&selected` 即有竞争，旧 AI/BMC 在两种位宽均报 SAFE。
整数/字节守卫和函数指针改写、持锁写及 helper 变体又复现同类遗漏。

根因在 `NumericalExecutionEngine::exec(Store)`：TOP 目的只置
`record_unknown_pointer_write` 后返回，既未忘记本地 memory，也未登记遗漏的效果。
普通 pointer getters、整数干扰和其他精化通道尚可恢复陈旧摘要，导致访问不可达或
零候选。因此“没有候选对”不能证明没有竞争；仅修改某个 getter 也不构成完整修复。

当前最小通用修复对并发模式下的全部 RHS 类型执行三步：保留 unknown-write/dirty，
登记 `unknown destination write effects not covered`，再 `mem_forget_all`。
既有 checker 独立于候选枚举输出这条持久 warning，demote 模式回退 UNKNOWN。
这是未知足迹的支持边界；未完成 escaped-local、入口初始化、FPA、整数/指针黑板及
强精化的统一失效证明前不能删除 warning。没有拓宽 heap/array 支持或修改 frontend。

[回归](review-repros/ai-unknown-writes/run.py)包含 10 个情况：未知 pointer、整数、字节、
heap 字段、函数指针、持锁、helper、浮点守卫，以及直接竞争和无修改安全对照。
32/64 位共 40 次观察，LP64 使用改名输入；另有 20 次原生前缀断言验证合法目标选择，
原生检查不运行竞争。六种旧 FN 形状共 24 次错误 SAFE 全部回退 UNKNOWN；
直接竞争仍 AI UNKNOWN/BMC FALSE，无修改对照仍 SAFE。heap/浮点情况此前已 UNKNOWN，
不把它们计为新修复 FN。新 FALSE 的所选执行均 AR ACCEPT。

1029 项 AI/BMC 配对逐任务判定不变：AI TN=686，组合 TP=109/TN=686，
FP/FN=0、UNKNOWN=233、error=1；137 条所选执行重新检查均 ACCEPT。
这也说明冻结集 FP/FN 为零不能排除集合外的正确性缺口。
相关 pointer-bytes/write-union/stable-publication 共 132 次观察及 P0 的 20 次观察通过；
stable-publication CASE6 的两条历史执行检查仍为 `UNSUPPORTED: load representation/type`，
不能将这一回归套件称为全部独立执行已验收。

证据：[汇总](artifacts/ai-precision-2026-10-12/unknown-writes/summary.json)、
[反例配对](artifacts/ai-precision-2026-10-12/unknown-writes/fixture-transitions.json)、
[全量配对](artifacts/ai-precision-2026-10-12/unknown-writes/paired.json)、
[所选执行](artifacts/ai-precision-2026-10-12/unknown-writes/selected-executions.json)、
[24 项当前处置](artifacts/ai-precision-2026-10-12/unknown-writes/dispositions-24.json)。

### M2：初始化到首次发布的 HB 证书（2026-10-12 已实现）

入口 `09-regions_02-list_nr.c` 中，worker 的 `init(p,7)` 尚未持锁，但 p 是本次
malloc 返回的新对象；随后在 `A_mutex` 内写 `A->next=p`，再解锁。main 在同锁内
读取 `A->next` 再读 datum。证明目标是：读者取得这个对象必须经过首次发布后的
mutex handoff，而不是从“以后持锁”猜私有。

实现为 `checker/init_publication.hpp::InitPublication`，在最终离线 pair 检查中消费
AccessRecord 和已有调用/创建证书。只支持普通 Store 初始化、经普通 pointer Load
取得对象的 Load 读端；没有修改 transfer、frontend 或 BMC 值编码。

- 沿透明 copy/bitcast、常量偏移及真实 CallContext 的不可重定义 formal 绑定，
  追溯到直接 LibcMalloc，核对 allocation call/context。分配所在函数至多执行一次、
  分配块无回边；不把 allocation site 汇总当成单个动态对象。
- allocation、该具体初始化/调用、首次 publication 同基本块且源语句依次发生。
  publication 前对象别名只用于可追溯派生、scalar/NULL 初始化或受证 pure helper。
  地址/表示存到其他内存、未知调用、load、整数转换、不可追溯 phi/下标均拒绝。
  pure helper 只有一个 block，参数不可改写；写入为界内 constant offset scalar/NULL，
  允许纯 scalar 运算和 canonical scalar nondet，不允许 pointer Load/逃逸。
- publication 为整宽 pointer Store，RHS 是本次 allocation base 0。读端 address
  必须来自实际 pointer Load，其对象/offset/width 与 publication 相同。source Load
  与最终 reader 都持 publication 的同一个 physical exclusive lock，并有同块后续
  普通 mutex_unlock；unlock 地址是完整 global mutex、offset 0。symbolic/pseudo lock
  不足以建立本证书。
- publication 槽的 container 也必须是单次、不回收的独立 LibcMalloc 分配；
  `DynAlloc` 还包括 FILE 等对象，不能统一继承 malloc 生命周期合同。main 在全部
  possible create 前给槽合法完整 pointer 初值；最终记录中全部可能重叠写只准同一
  整槽的 NULL 或单次 malloc base。unknown、partial、byte、copy、其他 pointer 来源
  或不确定 width 均拒绝。此轮支持这条闭合来源链，尚不支持任意 pointer graph。

全局要求唯一 main 根、无 ctor/custom initialization、无未覆盖效果/线程体；
整 bundle 拒绝 free/realloc 和 pointer/integer 表示转换。固定工作预算耗尽只保留 pair。
证书只排除具体 initialization/context 的候选，不标整个 site 私有，不传播给子节点，
不给后续无锁写同步。旧 `kEnablePrivateInit=false` 保持；allocator、may-points-to、
MUST 持锁、动态唯一性和最终 effects 覆盖仍是它的使用前提。

固定 Goblint 的 `region.ml:35–57` 查询 fresh bullet，`special:157–168` 产生分配
bullet，`threadspawn:179–186` 发布 escape，是解释差异的参考逻辑。IKOS 此轮未移植
region domain，而是使用局部 HB 充分条件；依据为合法新分配的独立性、发布前的
用途闭包和 mutex release/acquire 排序。参考 TRUE 不代替这些前提。

11 个 [定向情况](review-repros/ai-init-publication/run.py)双 ABI/两模式共 44 次观察：
正确 init→publish 四次 UNKNOWN→SAFE；提前 publish、晚一次 helper 初始化、无锁
publish、不同 source/lock、helper 泄漏均未变 SAFE。表示写、loop generation 和 free
控制保留能力边界；LP64 同时改名。既有 unknown-write/pointer-bytes/write-union/
stable-publication 共 172 次观察及 P0 20 次观察通过；stable-publication CASE6 的两条
历史 `UNSUPPORTED: load representation/type` 仍单独列出。

1029 项逐任务配对仅入口 UNKNOWN→SAFE：AI TN 686→687、UNKNOWN 342→341；
组合 TP=109 不变、TN 686→687、UNKNOWN 233→232，FP/FN=0、error=1。
109 项的 137 条所选执行重新独立 AR ACCEPT；这不是官方源 witness 或全语言证明。
证据：[汇总](artifacts/ai-precision-2026-10-12/init-publication/summary.json)、
[逐任务配对](artifacts/ai-precision-2026-10-12/init-publication/paired.json)、
[反例配对](artifacts/ai-precision-2026-10-12/init-publication/fixture-transitions.json)、
[所选执行](artifacts/ai-precision-2026-10-12/init-publication/selected-executions.json)。

### M2 首批出口与当前 24 个候选的处置

三个固定入口均已恢复 SAFE；24 项的源/YAML 指纹、当前报告、逐任务 AI/BMC 判定、
修正后的参考结果及语义前提已逐项更新到
[处置记录](artifacts/ai-precision-2026-10-12/init-publication/dispositions-24.json)，
并同步原安全台账。后续字符串与 pthread 初始化各恢复两项，初始句柄 seed 再恢复一项，
初始化发布证书再恢复一项，累计 9 项；余下 15 项仍为 UNKNOWN，具体缺口如下：

| 数量 | 候选机制 | 当前不能证明的条件 |
| ---: | --- | --- |
| 13 | heap region / freshness / publication（5 个 regions、8 个 race_reach） | 多来源/多步 pointer graph 闭包、list/slot 所有权与字段来源；实际报告仍有候选对，并有 handle coverage 缺口，单次初始化证书不能自动推广 |
| 1 | handle 存储覆盖（arraylist） | 初始 seed 已覆盖；其余 TOP heap Load 的字节/类型来源仍未证，无残余候选对也不能删除 gate |
| 1 | dynamic join array（thread-join-array-const） | 全部动态线程实例的 array-slot 来源和完成证书；main 的 joined digest 仍为空，保留两个候选对 |

固定 Goblint 的六配置 portfolio 最终均 TRUE；join-array 前四配置为 UNKNOWN，
第五配置才 TRUE，保留这一序列，不能说所有候选都由默认配置证明。
这些缺口是现有实现/证书边界，不能据此声称 IKOS 无法实现对应机制。

首批验收完成的依据是：各机制分别提交，build/install 与双架构/P0 通过，
两种模式全量配对，现有标签 TP 的所选执行重新检查，24 项都有明确去向。
安全 UNKNOWN 仍有 106 项，其中本轮余下 15 项参考 TRUE，另 91 项参考未证实。
后续 TN 轮需恢复保守的 heap pointer 来源或证明 pointer-load 的存储布局，再处理动态 join array；
通用 heap-region 算法仍须独立设计，不能靠放开 TOP 或仅凭参考 TRUE 清除告警。
全语言 soundness、混合锁集域义务和官方源 witness 验收仍是各自独立的工作，
不能由这次首批出口推断它们已完成。

**10/13 M3后的当前状态补充：** 上述表格是M2历史快照。typed槽来源与常量控制全实例
证书已恢复固定join数组，当前TN688、TP115、安全UNKNOWN105、可信Goblint TRUE余14。
13个heap/region与1个arraylist覆盖缺口继续保留；见[BMC阶段出口](ikos-bmc-complete.md#m3-exit)。
完成证书来自完整受限控制协议，未使用BMC反例分区提示；M6研究准入仍需另外成立。

### M2 下一机制：实施前参考复核与剩余证书（2026-10-12）

本轮将其余 91 个安全 UNKNOWN 在修正 GCC32 machine definition 的固定 Goblint 上
重新运行：六配置 portfolio 每项 30 秒、3 jobs，得到 UNSUPPORTED 30、UNKNOWN 25、
TIMEOUT 34、ERROR 2，无新增可信 TRUE，也没有 machine-definition fallback。
其中 14 项原始输出为 TRUE，但均有 `Unsound`；官方脚本式 portfolio 会在该 TRUE 上
停止，不能将它们登记为已证明。因此另外尝试后续配置，每配置 30 秒、每项总预算
90 秒，63 次配置运行最终仍为 UNSUPPORTED 5、TIMEOUT 9，无新增可信 TRUE。
超时与警告不证明不可实现；以上是本地固定版本/预算的观察，非官方 2027 工具结果。
[参考证据](artifacts/ai-precision-2026-10-12/next-bases/references.json)保留命令、指纹和警告摘录。

以下是实施前设计依据，当时有 16 项可信 TRUE 待恢复。初始化发布受限子集已按上一节
单独实现与验收，余下 15 项；pointer-array 坐标的最新实施见下节；更广来源图尚未实现，
不同机制不能混入同一 behavioral commit。

**初始化到首次发布的 HB 证书。** 入口为 `09-regions_02-list_nr.c`：worker 的
`malloc→init(p,7)→A->next=p→unlock(A_mutex)`，读端在同锁下经 `A->next` 读取 datum。
证书只作用于具体 `init Store + CallContext` 的候选对。必须把 formal 沿真实实参
追溯到直接 LibcMalloc，核对 allocation call/context、单次动态激活与生命周期；
分配、初始化调用和首次发布按同基本块顺序且无回边。纯初始化 helper 仅支持界内
常量 offset 的整 scalar/NULL 写。发布前全部 alias/表示使用必须闭合且不逃逸，
发布 Store 持物理 exclusive L，并有后续普通 unlock；读端 pointer Load 必须确实
读取相同 publication slot，源 Load 与最终访问都持 L。

还须证明该槽在相关线程创建前有合法完整 pointer 初值、全部后续写/未知效果有覆盖，
否则其他来源可能在正式发布前把对象暴露给读者。结构证明可缓存，最终消费时仍检查
迟到效果，不能缓存早期“没看见逃逸”的结论。拒绝 TOP 读端、错误锁/来源、generation
复用及晚初始化；不把证明传播给整个 allocation site、后续无锁写或子节点。
现有 AccessRecord、allocation context、同块顺序和完整 pointer-write 失效是基础，
当前已实现上一节的更窄槽来源闭包；更广来源图、allocator/helper/生命周期支持仍需
后续证明。旧 `private_init` 与宽松 `is_fresh_alloc` 不可复用。

**初始化后只读的 pointer-array 摘要。** `arraylist` 在 interval、interval-congruence、
DBM-congruence、octagon 和 widening-delay=10 下均 UNKNOWN、零候选，两个 coverage
告警相同。现有只读 debug intrinsic 探针显示：循环写的偏移 ILP32 为 `[0,36]`、
LP64 为 `[0,72]`，main/worker 的 j/i 读偏移却都为 TOP。所以仅调整数值域或放开
pointer footprint invalidator 不能得到完整 pointer 值。

最小候选实现是额外 array-smashing 坐标 S：完整非 TLS global `[N x pointer]`，
typed NULL 初始化，main prefix 的每次完整 pointer Store 即时 `S←S⊔RHS`，
含重复 site 的值扩大、dirty 与 widening；所有 possible create 后均不可到达任何 writer。
数组地址用途、初值及所有写必须闭合；字节/部分/opaque/未知写永久撤销该坐标。
Load 保留 native/interference 读取，再将结果与完整 S 相交，不能只弱写 source memory，
也不能收窄旧 access records 或清除既有持久 warning。一般并发可写数组不属于此子集。

为处理 TOP j/i，需要另一个已核验的访问前提：实际 LLVM Load/Store 的目的为直接
`inbounds GEP @完整GlobalVariable,0,index`，完整 pointer leaf、真实 global 类型与
GEP source type 一致，并核对 AR/LLVM 来源与布局。正常有定义的完整解引用不允许
one-past；该条件仅用于当前正常读取的值投影，不能追溯删除此前竞争前缀。
不能把较大 allocation 经 bitcast/GlobalAlias 伪装的子数组当完整数组，也不能接受
freeze/phi/select/额外位移。若这一局部 LLVM 合同不能建立，则先修 helper 布尔关系
或证明实际 bounds，继续 UNKNOWN。原输入两 ABI 的八个 GEP 已重新核实并保存指纹；
AR 保留 frontend GEP，可直接审查，无需先改 frontend 或新增 metadata。
本轮已补读精确 LLVM 14.0.6 LangRef 的 Pointer Aliasing/GEP/Freeze/Capture 语义，
[版本与原文指纹](artifacts/ai-precision-2026-10-12/init-publication/llvm-reference.json)可核对；
实施前的旧设计只读到 14.0.0，历史 design.json 保留当时事实。数组转移的最新状态见下一节。

这段保留实施前设计证据；最新初始化发布 TN 收益见上一节，数组实施仍无冻结集新增 TN。
两者均不构成全语言保持性定理或性能收益证明。
验收需覆盖早发布/晚初始化、其他读源/锁、字节破坏、迟到 writer、布局伪装及双 ABI，
再按既定 AI/BMC 配对和独立所选执行检查完成节点。
证据：[汇总](artifacts/ai-precision-2026-10-12/next-bases/summary.json)、
[配置/地址/LLVM 检查](artifacts/ai-precision-2026-10-12/next-bases/array-source.json)、
[实施前提](artifacts/ai-precision-2026-10-12/next-bases/design.json)。

### M2：只读指针数组的值投影（2026-10-12 已实现）

实现见 `execution_engine/readonly_pointer_array.hpp`、Load 执行器和
`ConcurrentGlobalEnv`。没有新建另一个 heap domain：S 复用现有 global pointer board，
其每次完整 pointer Store 的 RHS 已即时 join/widen 并置 dirty；查询时加入已证明的
初始 NULL。S 是所有槽、所有可能写值的并集，不证明槽独占、对象新鲜或链表区域。

静态前提为完整非 TLS homogeneous pointer array、LLVM NULL initializer、AR 对应的
整对象 zero Store、唯一普通 main 入口及无 ctor/custom init。LLVM use 图与 AR 用途
分别闭合，地址只能透明派生后作整宽 pointer Load/Store；字节/部分/bulk 写、地址逃逸、
alias/phi/freeze/opaque transport 拒绝。全部非 initializer writer 必须在 main，任何
possible create 的后继都不能到达 writer；helper/间接 create 和证明预算耗尽保守拒绝。
实际访问必须是直接 `inbounds GEP @完整global,0,index`，叶类型为普通对象指针，
LLVM/AR 元素数、整对象大小及指针宽度一致；不能用 struct 子数组或 bitcast 伪装布局。
AR 指向类型的 signedness 视图可能不同，不能仅凭类型对象不相等拒绝正常整宽 Load。

在 native/interference 读取前取得完整 S，读取后才对当前正常结果作交集投影。
有定义的完整 inbounds 解引用排除 one-past；不收窄旧访问记录或删除竞争前缀。
unknown write、unmodeled effect/create 出现即停止使用 S；静态证书可缓存，运行期有效性
每次重查。现有 singleton footprint invalidator、memory forget 和 sticky warning 保持。
源码没有路径名、标签或 benchmark 形状例外，也没有 frontend 改动。

双架构 AI/BMC 共 64 次新回归，含改名、64-bit datum、无锁/不同锁、跨槽 alias、
字节/bulk 破坏、helper escape、struct 布局伪装、晚初始化和晚发布。
两个只读整数组正例有 8 次 UNKNOWN→SAFE；28 次竞争标签对照均未误判 SAFE。
另复查未知写/字节/稳定发布/首次发布/写并集共 216 次以及 P0 20 次。
既有 stable-publication CASE6 的两条 AR 类型边界仍保留，不能把整套回归称为 ACCEPT。

原始 `09-regions_09-arraylist.c` 仍 UNKNOWN：原型探针曾观察到有限 array may-value，
但完整程序的内层 heap 访问产生未覆盖效果，随后该投影停止使用；零候选不能消除告警。
只保留 main 初始化前缀的 debug intrinsic 对照可以恢复有限 list/next 值，这不是原始任务
通过的证据，也没有定位/解决所有跨线程 field transfer。需要闭合字段来源、干扰、表示与
生命周期，不能撤销 warning 或重新启用 private_init 换取 TN。

1029 项 AI/BMC 全量配对逐任务无变化：TP=109、TN=687、FP/FN=0、UNKNOWN=232、
error=1；109 个标签 TP 的 137 条所选执行重新独立 AR ACCEPT。
墙钟 AI 162.5 秒、BMC 364.4 秒，工作站非隔离运行，不据此宣称提速或变慢。
该机制的收益是受限可复用能力和最小正例精度，冻结集 TN 增量为 0。
证据：[汇总](artifacts/ai-precision-2026-10-12/readonly-array/summary.json)、
[配对](artifacts/ai-precision-2026-10-12/readonly-array/paired.json)、
[回归](artifacts/ai-precision-2026-10-12/readonly-array/fixtures.json)、
[所选执行](artifacts/ai-precision-2026-10-12/readonly-array/selected-executions.json)、
[更新的 24 项处置](artifacts/ai-precision-2026-10-12/readonly-array/dispositions-24.json)。


<a id="evidence"></a>
## 9. 按机制查证据

以下索引保存历史实验的版本与转移；表中旧编号仅用于回溯原报告，不是阅读顺序。
原始长文可用 README 中的 Git 命令取回。每次新机制更新其对应章节及证据行，
不再追加一份完整阶段报告。

| 原编号 | 机制或实验 | 原始依据 |
| --- | --- | --- |
| M2-readonly-array | 完整只读数组的 may-value 投影；原始链表入口仍 UNKNOWN，冻结集 TN 增量 0 | [记录](artifacts/ai-precision-2026-10-12/readonly-array/summary.json)、[目录](artifacts/ai-precision-2026-10-12/readonly-array/) |
| M2-init-publication | 单次分配、逐 context 首次发布闭包、确定槽来源与 physical mutex HB | [记录](artifacts/ai-precision-2026-10-12/init-publication/summary.json)、[目录](artifacts/ai-precision-2026-10-12/init-publication/) |
| M2-next-bases | 91 项可信参考复核、14 项后续配置、受限发布/数组设计，尚无新语义 | [记录](artifacts/ai-precision-2026-10-12/next-bases/summary.json)、[目录](artifacts/ai-precision-2026-10-12/next-bases/) |
| M2-unknown-write | TOP 普通 Store 的 memory havoc 与独立 coverage 回退，不宣称完整值模型 | [记录](artifacts/ai-precision-2026-10-12/unknown-writes/summary.json)、[目录](artifacts/ai-precision-2026-10-12/unknown-writes/) |
| M2-pthread-init | 受限 glibc 初始化、opaque 存储闭包与有界效果 | [记录](artifacts/ai-precision-2026-10-12/pthread-initialization/summary.json)、[目录](artifacts/ai-precision-2026-10-12/pthread-initialization/) |
| M2-handle-seed | 可信 main 初始抽象写，不过滤 TOP 或扩大 join 语义 | [记录](artifacts/ai-precision-2026-10-12/handle-seeds/summary.json)、[目录](artifacts/ai-precision-2026-10-12/handle-seeds/) |
| M2-string | 未知长度字节效果、pointer bounds 失效及受限逃逸证书 | [记录](artifacts/ai-precision-2026-10-12/string-effects/summary.json)、[目录](artifacts/ai-precision-2026-10-12/string-effects/) |
| M2-release | 首次获取证书、实际释放端点及嵌套保护 | [记录](artifacts/ai-precision-2026-10-12/release-values/summary.json)、[目录](artifacts/ai-precision-2026-10-12/release-values/) |
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
| Gavrilenko 等，[BMC for Weak Memory Models: Relation Analysis for Compact SMT Encodings](https://hernanponcedeleon.github.io/pdfs/cav2019.pdf)，CAV 2019 | 本轮读 §1/§3：may/active 关系、按需传播、may-alias 及差集方向；没有复现论文性能实验 |
| Haas 等，[CAAT: Consistency as a Theory](https://hernanponcedeleon.github.io/pdfs/oopsla2022.pdf)，OOPSLA 2022，DOI 10.1145/3563292 | 本轮读 §1：惰性 SMT/一致性理论的动机、贡献与 Deagle 对照；未审完算法证明或复现实验 |

阅读章节和 PDF/hash 见 [publications.json](artifacts/ai-audit-2026-10-10/publications.json)、
[digest 来源](artifacts/ai-goblint-reference-2026-10-10/sources.json)及
[本轮设计依据](artifacts/strategic-design-2026-10-13/review.json)。这次设计审查未重做论文实验。

固定 Goblint：/home/ruan/goblint-analyzer，commit bb55b70d91290992629ba3d073185c10cdf356e6。
按任务读 basePriv.ml、mutexAnalysis.ml、threadId.ml/threadIdDomain.ml、mHPAnalysis.ml、commonPriv.ml、mCPAccess.ml。
digest 是线程计算历史摘要，不是校验哈希或单纯函数摘要；IKOS 已有类似 operational 组件，尚未实现完整 digest/PBR 框架。
不能由“还未完整采用论文框架”推断必然弱多少，也不能由近似 TN 计数推断能力等价。
POSIX/API 布局依据固定声明、平台 header/native 断言和实际参数效果；完整 C11 弱内存不在当前认证范围。
