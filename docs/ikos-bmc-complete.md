# IKOS BMC：当前实现、语义契约、验证与扩展计划

更新：2026-10-11。本文是 BMC 的统一维护入口，合并原有设计、审查、精度恢复、
参考工具、模型验证和 UNKNOWN 台账报告。实现事实、有限实验、历史记录和未实现方案
分别说明；本文不是全项目 soundness 证明。原报告正文保留在 Git 历史，测试与小型证据仍在原路径。

阅读导航：

- [当前状态与任务范围](#current)：先判断数字和能力是否适用于本次工作。
- [判定契约与参考理论](#contract)、[架构与代码导航](#architecture)。
- [值与 SC 来源](#values)、[地址与对象](#objects)、[线程与调用](#threads)、[同步](#sync)。
- [搜索与性能](#search)、[正确性修复](#correctness)、[反例与 witness 验证](#validation)。
- [逐用例实施和回归流程](#workflow)、[下一阶段与结束条件](#roadmap)。
- [阶段历史](#history)、[证据索引](#evidence)、[文献与版本](#references)。

<a id="current"></a>
## 1. 当前状态与任务范围

最新基线、二进制、任务人口和剩余 UNKNOWN 只维护在 [README](README.md#current)。
本手册描述候选驱动的受限 pthread/SC 有界确认器；2027 官方 manifest 尚未固定。
本地 2026 扫描集合全部 ILP32，定向双架构回归不等于 LP64 全量。

BMC 第一版于 2026-10-10 完成有限执行/结果接口、共享回调、受限 join、独立 AR 检查
与 AI 事实使用契约；后续 AI 正确性和精度工作也改变候选和确认能力。
§12 的第一版验收和 §14 的阶段数字是历史证据，不是当前基线。
最近扩展包括隐式 main 的共享栈对象及借助 AI 生命周期证书的 helper 发布，见 §5。

TP 是与 YAML 标签一致的 FALSE；独立 AR ACCEPT 是另一种证据。
检查以可信前端、导出快照和明确 API 合同为前提，不等于官方源码/witness2.2 验收，
不证明所有 SAT 模型或全部 SAFE 过滤。正常分析器导出证据仍标 UNVALIDATED，
离线验收另存。不能由冻结集 FP/FN=0 宣称全语言 soundness。

安全 UNKNOWN 进入 [AI 路线](ikos-ai-complete.md#roadmap)，竞争 UNKNOWN 按标签筛选历史台账。
固定参考 FAIL 只帮助选任务，不证明 IKOS 具体模型；有界 PASS 不证明无界安全。
参考状态、实际阻塞和已尝试机制都要绑定输入 hash/架构/预算，不能用旧分桶预测可恢复数量。

<a id="contract"></a>
## 2. 判定契约与参考理论

### 2.1 SAFE、FALSE、UNKNOWN 需要的证据

| 结论 | 必要前提 | 前提不足时 |
| --- | --- | --- |
| TRUE / SAFE | 抽象解释覆盖所有相关具体执行；转移、不动点、干扰和每条候选过滤均保守 | 保留 may-warning / UNKNOWN，不能删掉候选冒称安全 |
| FALSE / RACE | 所选模型能对应约定语义下真实有限执行；创建、控制流、值、对象、同步与目标冲突均成立 | 不确认，记录 unsupported、solver unknown 或预算不足 |
| UNKNOWN | 当前能力或资源不能给出上述结论 | 保留具体原因；不解释成无竞争 |

BMC 在本架构中只恢复 FALSE，**不从有界 UNSAT、候选为空、截断或超时推出 TRUE**。
`confirm_race` / `check_race` 返回结构化 `Result`。CONFIRMED 带所选执行证据；
BOUNDED_UNSAT、UNSUPPORTED、SOLVER_UNKNOWN、BUDGET_EXCEEDED、NOT_SEARCHED 均不证明安全。
聚合保留 observed 状态，遇到未支持分支或预算耗尽不能总结为全部 UNSAT。

有限展开限制了路径集合；省略值流、调用或实例约束却会增加虚假执行。
所以“BMC 是欠近似，因此自动 FP=0”不成立。真实 nondet 可以是自由符号，
共享 load 必须由真实来源供值；两者不能混同。
每条过滤或确认规则都应写清语义前提、近似方向、建立前提的证据、失败回退和最小正反例。
“只丢 TP”的注释不是证明。

### 2.2 固定 Dartagnan 的实际竞争查询

研究参照为 Dartagnan/Dat3M 4.3.1，提交
`7caef5c850dc2dbf81c5554dcf41e402007f7a19`。
这一版 `svcomp.cat` 的关系定义是：

```text
SVCOMP
let com = (rf | fr | co)
let hb = po | (com & int) | (rmw^-1? ; (com & ext))
acyclic hb as sc
```

`rf` 为读来源，`co` 为同址写顺序，`fr` 为从读关系；`int/ext` 区分线程内外通信。
这里的 CAT `hb` 不等于 POSIX/C11 happens-before，也不能简写为 `po | com | rmw`。
assume/取值约束是执行条件，不是二元排序边。

固定版 no-data-race 查询在 `PropertyEncoder.encodeDataRaces`，不在 CAT 的 `dr/conflict`：
枚举不同线程的非初始化、非原子写 `w` 与内存访问 `m`，先做 may-alias 预筛，
排除双 RMW 候选，再要求真实 execution、sameAddress，以及
`hb(m,w) && clock_hb(w) == clock_hb(m) + 1`。
“相邻”是该完整模型中的时钟查询，**不等于没有 hb**，不能单独作为竞争定理。
普通原子访问不满足 `canRace`；双 RMW 另有显式排除，不能把 RMW 与 MemoryOrder 混为一项。

IKOS 的有限前缀、scheduler clock 和受限 SC 来源并未实现这个完整 CAT 模型。
普通目标试两种 clock 相邻方向；atomic 目标使用受限的无中间内存/真实同步查询。
查询形状相近不能证明等价。固定参考还按起始地址判同址，存在混合宽度重叠覆盖边界。

2026-06-25 的 `c802dc442` 才引入 CAT `dr/conflict/sw/hb-race`。
历史记录的 2027 开发提交 `b81ec6c…` 也不是本地当前版本或正式比赛配置。
不能把不同版本的机制和分数拼成同一结论。

### 2.3 C / LLVM / AR 与库契约

当前实现是受限 pthread / SC 有界前缀，不自动覆盖 C11 弱内存、硬件 RMW、所有 extern 或全部 C UB。
机器整数位宽、signed/unsigned、LLVM exact/no-wrap/poison、对象范围、对齐、寿命、
线程/函数 ABI 和初始化来源都可能影响反例存在性。
`freeze_uninit` 的抽象初始化不是源程序的合法 nondet 输入。

有竞争的有限前缀通常不需要程序最终终止，但省略的行为必须不影响该前缀的合法性。
pre-main constructor、必要创建、guard、未知调用或相关写不能随意省略。
涉及 UB 的最小用例用于检查能力边界，不标作合法 race-free 程序的 FP 铁证。

<a id="architecture"></a>
## 3. 架构、信息流与代码导航

```text
C → LLVM → AR → 线程模块化抽象解释 → AccessRecord / may 候选
                                      ├─ 保守规则证明 SAFE
                                      └─ demote UNKNOWN → 候选前缀 BMC → 有依据的 FALSE / UNKNOWN
```

BMC 复用 IKOS 的候选和 AR，不另建 C 前端。上游漏掉访问或删除候选时，更强的 SMT 无法补救。
线程/内存语义尽量与 race 查询分开，后续死锁、可达性可复用已证明的执行机制；
当前 race adapter 不自动支持这些性质。

| 模块 | 文件及搜索锚点 |
| --- | --- |
| 访问记录、宽度、过滤、候选排序与报告 | `analyzer/include/ikos/analyzer/checker/data_race.hpp`；`analyzer/src/checker/data_race.cpp`：`AccessRecord`、`confirm_race`、`private_init` |
| BMC 调度、预算、诊断 | `analyzer/include/ikos/analyzer/analysis/bmc/bmc.hpp` |
| 事件分类、路径、动态定义和指针解析 | 同目录 `unroll.hpp`：`BmcEvent`、`classify_event`、`symbolic_byte_offset`、`unroll_to`、`unroll_completed` |
| 值/地址/同步与竞争 SMT | 同目录 `encode.hpp`：`check_race`、`encode_value`、`prefix_values_defined` |
| 抽象事实、类型初值、结构化导出 | 同目录 `facts.hpp`、`initial.hpp`、`replay.hpp` |
| pthread 分派与抽象执行 | `analysis/execution_engine/numerical.hpp`、`concurrent_semantics.hpp`、`concurrent_inliner.hpp`，位于 analyzer include 下 |
| 锁域、跨线程摘要与不动点 | `core/include/ikos/core/domain/lockset/lockset_domain.hpp`；`concurrent_global_env.hpp`；`analyzer/src/analysis/value/thread_modular.cpp` |
| 前端、结果和 witness | `frontend/llvm/`；`analyzer/python/ikos/`；`benchexec/tools/ikos-conc.py` |
| 研究验证与诊断 | `scripts/bmc_diagnostics.py`、`scripts/validate_bmc_witnesses.py`、`scripts/dartagnan_strict/` |

`BmcEvent` 记录 AR 语句、事件类别、对象/分配身份、offset 仿射项及定义上下文、
访问 width、锁 reader 模式、线程创建信息和目标标记等。
Read/Write、Lock/Unlock、Assume、Create/Join、Local/Unsupported 是基础事件类别；
合成 atomic 和 condition 边界另带 synthetic 标记。
静态语句或 `InternalVariable*` 不是动态执行身份，必须保留线程、调用、循环和定义上下文。

`confirm_race` 的关键流程：

1. checker 重新检查所选候选的对象、范围、线程排序和可支持上下文，提供 `CandidateFacts`。
2. 建立实际 main 创建前缀及独立动态线程实例，展开 A/B 的 CFG、调用和必要完成尾部。
3. 保留已发出的完整前缀；枚举中断组在原 deadline 的后续轮次查询，未完成分支不发出。
4. 检查调用、定义性、地址、初始化与库控制存储；编码 guard、机器值、SC 读源和同步。
5. 在预算内查询实际冲突对，保存成功模型、目标索引、`confirmed_pair_id` 与创建序列。
6. 报告和 witness 指向真正成功的候选，不能继续使用第一对访问或按入口名猜线程实例。

### 3.1 IKOS 向 BMC 提供信息：已有贡献与尚未证明的收益

| 信息 | 允许的用途 | 限制 |
| --- | --- | --- |
| may-points-to、offset、width | 筛选潜在对象/访问、约束可能范围、安排依赖和查询 | may-alias 不证明实际同址；singleton heap cell 不证明同一次分配 |
| 真实创建前缀 | 实例存在、共同初始化、实际 guard | may-spawn/multiplicity 不能代替执行证据 |
| AR typed initializer / DataLayout | 精确初始化位图、对象 extent、访问宽度和字节序 | padding、undef、无来源 malloc 字节不能补成任意值或零 |
| 受限局部 no-wrap 证书 | 省去重复定义性检查；使用的结果仍精确编码 | 要有 owner/控制上下文和来源前提，不适用于未知共享读 |
| MUST 同步/局部关系、may-loop invariant | 未来的公式简化、依赖裁剪和搜索提示 | 必须先建立条件化契约及保持性，不能把 must-edge 当两端必执行 |

历史 TP35 全量曾观察到候选 bounds 256 次/49 任务、SC 来源 648 次/43 任务，
no-wrap 证书消费为 0；这是信息流观测，不是因果收益。
后来对所选 21 个恢复任务关闭 bounds/no-wrap 两项事实，TP **21→21**。
此消融保留 typed initializer、创建和候选筛选，不能称为关闭全部 IKOS。
这一历史样本未证明这两个数值证书增加TP或稳定加速。

第一版补充普通结果内的`metrics`，不依赖diagnostics：尝试的path queries、输入prefix events
（synthetic展开前）、真正solver queries、累计顶层SMT assertions、solver/evidence微秒和
消费的bounds/no-wrap数。跨deferred轮次累积结果、失败状态及计数；不是只报最后一轮。
顶层assertion数量不等于内部AST/bit-vector复杂度，事件数也不是全程序事件数。
`scripts/ablate_bmc.py`串行运行、交替A/B顺序和三次重复，保留逐项时间、最大RSS与所选
执行验收。`IKOS_BMC_DISABLE_AI_FACTS`的存在只关闭BMC offset bounds和no-wrap消费，
仍保留AI SAFE、may候选筛选、typed initializer和真实创建；不能称为关闭整个IKOS。

10输入60次受控运行：7个比赛标签TP、2个UNKNOWN、1个最小正例，A/B判定完全一致，
所有所选执行AR ACCEPT。局部unsigned-char推广后加1的unused结果，AI证明范围[1,256]
不溢出，避免重复混合Int/BV求解：中位总耗时**0.184s→0.464s（开→关）**，
solver **3379µs→279123µs**，最大RSS85624→85888KiB。两个query输入均16事件、
1solver query、25顶层assertions；计数相同不意味着公式复杂度相同。
此类未被使用的结果仍需要定义性前提，不能为了快直接忽略；被guard/地址/值流使用的
结果仍走精确编码。双架构正例确认实际消费1条no-wrap证书，关事实后为0，执行均ACCEPT。
其他比赛样本耗时基本持平或只有小幅波动，例如shared callback 0.199/0.201s、
fib5 0.845/0.871s、NVRAM 2.080/2.077s；没有全项目普遍大幅加速的依据。
范围证书目前主要增加两个约束，样本的queries/events保持不变，尚未证明其一般收益。
`/usr/bin/time`测的是报告的maximum RSS，不是整个cgroup合计峰值。
证据：[ablation-summary](artifacts/bmc-v1-ai-facts-2026-10-10/ablation-summary.json)、
[逐次结果](artifacts/bmc-v1-ai-facts-2026-10-10/ablation-results.json)。

**依赖覆盖契约**：AI may-bounds不能替代rf来源。完整选择的startup/A/B前缀中，
真正执行的store、地址、库控制存储、动态调用/返回、guard和可能trap逐项核对；读取的
每个可能重叠写必须能解析并纳入SC最新写选择，未知别名/源/width/部分指针写拒绝。
未选择的其他线程保持未调度，仅允许§6.4核实的无副作用完成。这个存在性执行策略不
要求整个程序所有写进入查询，但不能忽略已经执行或为了Join必须完成的第三线程。
导出byte rf、具体值/地址/调用和实际创建，AR检查从全部执行事件重新计算，
不能把abstract range、may-callee或interval摘要当作某个实际store已执行的证据。
SAFE仍依赖抽象域与保守过滤覆盖，本文不把有限回归视为其完整证明。

共享读 interval 可能 stale，历史递增例两个线程读区间都为 `[0,0]`。
不能直接限制 BMC 具体读值；需要跨线程不动点、读时干扰及上下文契约。
可靠 may 集合缩小待编码范围、可检查 must 证书简化公式，是可完善的架构方向；
收益须用同语义、同界限的逐项消融与时间/内存/公式规模证明。

<a id="values"></a>
## 4. 值编码、定义性与 SC 读来源

### 4.1 A0 → A1 的正确性前提

A0 先封住未知值 guard、未知下标和失败的 base 提取。
A1 仅开放有合法来源的值：整数常量、核实 API 的真实 nondet、实际参数/返回、
受支持的转换和机器运算，再逐步开放有 SC 来源的 load。
历史“函数名包含 nondet 就自由化”已经换成 `IkosNonDet` 或精确声明 API 集合，
核对实参、返回类型与来源；用户定义函数不能凭名字跳过函数体。
旧式 C `extern int f()` 可以实际零实参调用，不能只因 AR vararg 表示而一律拒绝。

同一路径的 guard 和地址共用值 cache；不同线程/调用/迭代按动态定义区分。
共同 startup nondet 只产生一个真实符号，供两个 worker 使用。
值符号 `value_a_*` / `value_b_*` 与 `a*` / `b*` 时钟分离，避免错误相关。
定义须在本次实际前缀中早于使用；先核对定义身份再命中 cache，不能复用前次调用的值。
bitcast/assignment 剥层有界，失败保持 UNKNOWN。

### 4.2 机器整数与所有消费点

按位宽和符号处理算术、比较、SExt/ZExt、截断、bitcast、位运算、除余和移位。
signed MIN/-1、除零、移位范围、exact/no-wrap 等执行前提必须成立。
结果不被 guard/address 使用的运算仍检查 trap/poison；不能省略 unused 算术。
满足范围及 no-wrap 前提的加减乘可用数学整数，wrapping/位操作保留精确机器位模式。
大常量、系数和 SMT 常量用 `ZNumber` / 十进制字符串，不截断到宿主 int64。

`prefix_values_defined` 覆盖 store、return、call 实参、startup 和局部读。
合成 call/store 带 provenance，允许抽象 bookkeeping，但拒绝真实消费 synthetic/undef。
局部 load 只认可本次 allocation 之后真实覆盖写或受支持 create 输出；
有初始化的证明不意味着可以把读值自由化。
普通 assume wrapper 展开实际函数体，按实参的非零条件编码，不能因提取不到 Comparison 而跳过。
浮点、未知调用或不支持来源继续不确认。

### 4.3 整数全局对象与符号地址的按需 SC 来源

所有建模内存事件使用共同 SC 全序和不同 clock。同线程 po、create 和同步约束同时成立。
被 guard、地址或定义性消费的 load 绑定读前最新的兼容写，来源可为：

- AR 的确切 typed initializer，包括可支持的嵌套 struct/array/aggregate zero。
- 实际 startup 中此前的 main 写；startup load 不读未来写。
- 所选 A/B 前缀的实际写，写值继续递归检查来源。

单仿射地址的 rf 同址条件参与来源选择，“最近写”只考虑实际同一位置的写。
小对象 typed initializer 可在至多 256 个 stride 位置枚举。
`var_before` 固定保存指针时的下标定义，随后改变 i 不改变旧指针。
未映射来源不能自由化；未知副作用或必要其他线程需要补摘要/依赖闭包。
这仍是受限按需 SC 模型，不是完整 K 线程 CAT/rf/co 或弱内存实现。

### 4.4 全局整数的混合宽度字节来源

支持原始整数 global、常量 offset、8 的倍数且不超过 64 位，读写位宽等于实际字节宽度×8。
按目标 DataLayout 字节序，从真实初值位图/startup/A/B 的最近覆盖写为每个读字节供值，
concat 后按 load 类型解释。整次 load 共享一个 clock，不为每字节选择不同读取时刻。
缺初值字节只有被真实先行写覆盖才可读；不得自由补值。

普通同宽和符号数组保留整值快速路径，避免强制增加逐字节公式成本。
aggregate 混合宽度、symbolic 部分写、pointer bytes、padding/undef、heap byte rf 和更大对象仍有边界。
26 条双架构断言覆盖低/高字节、signed 扩展、双源拼接、main 部分初始化及不可能组合对照，
冻结集逐项不变。固定参考对部分拼接正例给 PASS：其 word/rf 同址模型不支持一般字节拼接，
不能把这类 PASS 当作 C 字节表示反证，也不能算独立验证通过。
gcc 双架构 native assert 只验证表示；大端分支未在真实大端机器测试。

### 4.5 堆整数读的实际来源

单次 main 分配支持对齐、精确 offset、1–8 字节整数 load 和同宽写，按真实 allocation 身份供值。
静态初值只用于 global；malloc 没有默认值。
**每个堆读都要编码来源，即使结果未使用**；整条前缀堆访问还检查 ABI 对齐。
无来源、循环自供值、partial/pointer rf、复杂地址、不对齐仍拒绝；packed 合法低对齐访问尚未建契约。

`02-base_24/25-malloc` 与 `26-malloc_struct` 的参考竞争依赖重要配置差异：
固定版 `WmmEncoder.memoryIsZeroed=true`，允许无来源读取零；malloc 显式初始化选项另为 false。
IKOS 没有导入这一默认假设。cast 修复后 malloc_struct 的 x/y 锁与真实来源公式仍 UNSAT。
参考原生 y 竞争对探针在 x=0 时双架构 SAT、x=3 时双架构 UNSAT_AT_BOUND。
这是具体模型差异，不说明标签错误、程序 SAFE 或 IKOS 永远无法支持；先核实 C/LLVM/比赛初始化契约。

<a id="objects"></a>
## 5. 地址、宽度与对象身份

### 5.1 范围重叠与符号下标

`AccessRecord.width` 独立于起始 offset 区间，load/store 来自 DataLayout store size；未知宽度不能判不相交。
SMT 冲突要求同一真实对象且
`off_a < off_b + width_b && off_b < off_a + width_a`。
不同起点也可能重叠。符号 offset 为 `const_off + coeff * encode_value(index)`，
与 guard 共用 cache，要求对象 extent、访问 width 和界内范围可证。
字段 offset 不能替代未知基址证明；失败提取不得继续查缺少同址约束的 SAT。

有限非空 global/可支持 heap may 集合可以提供候选；实际路径解析 base 和动态 allocation。
TOP、未支持的 TLS 来源、未知基址和复杂指针来源仍保守。
`06-symbeq_10-equ_rc` 恢复来自 main 实际选 B、worker 锁 A 却写 B；
不是因为 may-alias 就确认同对象。

### 5.2 参数、实际返回和已发布指针槽

指针沿本次实际调用参数、赋值/cast 和实际 Return 到达定义追踪；重复 helper 不能混用返回。
解析到无寿命证明的局部返回地址时拒绝，调用者局部透传也尚有保守边界。

global pointer slot 的不可变发布需要：完整 users/逃逸检查、唯一真实完整指针 store、
所有相关 create 位于发布之后、实际 load 不早于发布。纯 AR 地址 cast 可归一化到
pointer global、offset0、无变量下标；非零偏移、未知来源等仍保守。
这不是一般共享 pointer rf/co，也不能从 AI 的无序 initializer 区间猜实际发布值。

后续支持 helper 内初始化时，checker 向 `CandidateFacts` 提供 `single_execution` 合同：
静态唯一、无环的调用生命周期根植于隐式 main，实际 helper 形参绑定到本次实参；
slot 不逃逸且仅有一次实际 full pointer store，在所有相关 create 前执行。
AI 证书证明生命周期，BMC 仍展开实际 store/地址和取值，并导出供独立检查的执行。
不能把“函数只出现一次”当全局 single execution；重复调用、递归、slot 逃逸或其他写者拒绝。
证据见 [helper publication](artifacts/ai-precision-2026-10-11/publication/summary.json)。

### 5.3 单次 main allocation

核实真实 malloc intrinsic/外部声明、calling ABI、pointer 返回与目标 size_t 参数。
实际执行在首次 create 前一次分配，大小由已执行定义求得，正且不超过 1MiB。
保留 allocation/extent 独立身份，选择合法非空成功分支；目标必须解析到同次分配并满足范围。
两个同大小 abstract heap cell 不能因此合并。

单次 main 堆写和有真实来源的同宽堆整数读已支持；worker/helper 动态分配、重复站点、
free、一般寿命、可变槽和 heap pointer rf 尚未完成。
`09-regions` 列表需要这些能力，不能仅凭参考 FAIL 就开放对象门。

### 5.4 隐式 main 的共享栈

单次实际 Allocate 位于隐式 main 的活动 frame，先于所选 creates，main 在竞争前缀中仍存活。
实际线程实参、唯一外部指针链及 helper 绑定解析到同一对象，load/store 复用字节 SC 来源。
因此不能把“stack”一概视为每线程私有，也不能把任意名为 main 的 callee 栈当共享对象。
子线程私下调用 main、已返回 frame、重复 allocation 或未证实的链继续不确认。
双架构正例/反例/损坏执行和比赛配对见
[main-stack](artifacts/ai-precision-2026-10-11/main-stack/summary.json)。

<a id="threads"></a>
## 6. 线程激活、动态调用与完成端点

### 6.1 真实创建前缀

不同非 main 入口需要 main 的实际创建事件；同入口两实例需要两次不同动态 create。
main/worker 候选中所选 main 路径必须包含对应 create。
may-multiplicity、静态 create site 数量、互斥分支内两个站点均不能证明实例同时存在。
早期仅直线创建的 gate 已被受限 CFG、循环、helper 创建替代，不能继续写成当前只支持直线。

保留直线快路径，另最多 8 个创建 CFG 前缀，按真实 guard、动态定义和资源界限展开。
默认属性、合法输出槽、匹配 API/返回类型下选择成功创建；成功返回值为 signed32 的 0。
失败分支不会与活跃 child 拼接；一般失败/属性语义仍未建完。
startup 的共同初值、实际实参和条件与两路径一起编码，孩子可暂不调度。

枚举耗尽仍保留真正完成的创建前缀。nondet-array 中 main 可创建两孩子后暂停，
但原 n>=5 guard 必须成立，不能把循环上限改成2或假造退出。
startup 的同步区间须满足当前完整性检查，未知调用/无法支持的上下文不确认。

### 6.2 动态定义与实际 callee

确定函数常量、assignment/bitcast 链和本次已执行调用的 callback 参数可展开。
helper 在真实 Return 后接调用方续点；未知/bodyless 调用不是 no-op。
事件保留实际 callee、call/return 位置及 invocation，参数、返回与局部值按这一上下文解析。
同静态变量的两实例不能共享符号，后一次调用不能复用前一次 nondet/定义。

**有限global函数指针 load→间接调用已实现**，实际SC来源/签名/调用等式与边界见[共享回调实现](#callback-plan)。
FPA may-callee 仅能安排候选，不能任选函数体来确认实际调用。

### 6.3 受限线程完成

`unroll_completed` 支持真实 root Return，另支持非 main worker 根函数中的
直接 `pthread_exit(NULL)`：核实 C ABI、void 返回、单 pointer 参数、LLVM noreturn。
不能沿 exit 后继制造返回；helper/main exit、非 NULL payload、错误 ABI、未知 callback 注册继续拒绝。
这是有限完成端点，不是一般线程生命周期模型。

### 6.4 受限 Join 实例/完成匹配

已删除“每个Join都排序对侧last”的 blanket 边及对应剪枝。首版只支持从实际root-main
未逃逸标量栈槽读取的句柄，反查此前真正执行的Create occurrence；句柄重写时取真实
来源，不能按入口函数或静态槽名猜实例。辅助函数必须只有单块、单条实际`return NULL`，
不含任何共享/同步副作用；Join返回存储须为NULL，选取合法成功返回0，可用于守卫。
其完成可在Join前插入，与两条竞争前缀不产生额外共享效果；事件导出creator occurrence
和return statement，独立检查器从实际句柄及原程序快照复核，并合并真实终止VC。
这是可检查的极小完成摘要，不是一般第三线程执行器。

其他完成、句柄逃逸/数组/未知来源、此前Join/detach、递归main与atomic内Join仍拒绝。
特别是atomic内创建后立即Join，子线程不能运行，不能凭无副作用推定已经完成。
Condition release/reacquire插入事件后重新计算创建索引，避免绑定到旧位置。
创建句柄的普通身份读取和具体值读取仍须区分；库输出不能随意供值。
恢复Join数组/多级线程需动态handle、真实完成、其他线程效果及寿命，不能只放行gate。

十类对照/变体、双架构20次检查包括句柄copy/重用/逃逸、共享副作用、成功返回守卫、
condition展开和atomic阻塞；确认执行均AR ACCEPT，损坏creator/return被拒绝。
固定Dartagnan双架构对无关idle Join报FAIL，对真正等待worker及重用句柄对照报PASS。
本阶段1029配对 TP106→106 / TN696→696 / UNKNOWN227→227 / FP/FN/error0，逐任务无变化。
此特性在冻结集尚未增加TP，不能把最小例恢复数当比赛收益。
证据：[fixtures](artifacts/bmc-v1-join-2026-10-10/fixtures.json)、
[reference](artifacts/bmc-v1-join-2026-10-10/reference.json)。

<a id="sync"></a>
## 7. 同步与库效果

### 7.1 mutex / rwlock 的完整历史

沿实际参数解析对象和 offset，保存每段完整或未闭合临界区；旧 held/function-level 提前门已替换。
同对象区间互斥；rwlock reader-reader 不互斥，涉及 writer 的区间互斥。
单个 outstanding 符号锁支持实际地址表达式、unlock 同址与所有权等式；复杂符号嵌套仍拒绝。
未知身份、重入、没有本路径持有依据的 unlock 不能当成功释放。
抽象锁集 MUST 分量在 join 取交、MAY 取并，不能凭集合名字推断 definite 同步。

受限 **ERRORCHECK 已实现**：glibc GNU/Linux x86 ILP32/LP64，type=2、attr4、mutex24/40，
原 LLVM 真实类型/布局证明 global subobject、大小和对齐，支持常量成员及受限数组仿射索引。
首次创建前实际 attr_init→settype(2)→mutex_init；destroy 后禁止再使用属性，
但 attr destroy 不使已复制属性的 mutex 失效。
未初始化/重复 init、已使用 mutex 的重初始化、显式篡改、未知类型或并发控制变化拒绝。
只建成功 owned-unlock，失败 unlock 不产生释放效果。

恢复的 sound_unlock 两任务所选 main 分别 nondet=0/4，确实解锁自己持有的 mutex1/m[4]，
之后发生不受共同锁保护的访问。目标后的重复 unlock 不在所选有限前缀。
参考 setter/unlock 摘要省略 ERRORCHECK 所有权，不能直接移植；本地双架构 native 合同另验证 EPERM。
这不是所有平台、属性或错误路径的完整模型。

### 7.2 verifier atomic 与内部目标

依据核实的 `__VERIFIER_atomic_` API 前缀及具体函数体，保留真实调用并合成 begin/end，
实际 return 后才闭合。不可分割区间排除另一线程**所有事件**，不只排斥另一个 atomic 段。
synthetic 标记避免把一条源调用算成多次调用/定义。
嵌套、bodyless、atomic root 或未知硬件/C11 原子仍有 unsupported 边界。

目标在 atomic 内时不只删 open gate：显式标记动态目标，保留实际尾部，
当前要求双方都完成真实入口 Return/受支持完成路径，所有尾部 guard/值/调用继续编码。
普通 helper Return 枚举不能错误固定某一次循环目标，避免遗漏后续调用。
双 atomic 目标在搜索前排除；单侧 atomic 查询保留两方向，禁止其他内存/真实同步夹在目标之间，
局部/assume/合成边界不作为障碍，但执行约束仍在。

该查询比完整参考关系受限；目标前后都有内存障碍的中间目标可能仍 UNKNOWN。
两侧完成要求也可在未来用已证明的控制依赖闭包替代，不能当永久设计定律。

### 7.3 condition 的有限虚假唤醒

显式默认 cond_init：可解析非 TLS global 范围、大小/对齐可证，main 创建前初始化一次。
wait 需本路径实际持有可解析 global mutex，以 synthetic release/reacquire 分割区间，
选择一次有限虚假唤醒和成功返回0。条件谓词/循环 guard 仍编码，signal/broadcast 不凭空创造唤醒 HB。
同一 condition 的所选 waiter 绑定同一 mutex；顺序变更绑定也暂拒绝，是比 POSIX 更保守的能力边界。
普通用户访问覆盖控制存储则拒绝，不能以静态零充当库状态。

静态初始化仅核实 glibc x86 Linux/GNU 48-byte `union.pthread_cond_t`、非 TLS 直接 global，
控制字段 offset/size 为 0/8/16/24/32/36/40 与 8/8/8/8/4/4/8，LLVM 和 AR typed 初值均全零。
不把任意零数组、未知 ABI 或存储子范围当合法 condition。
destroy、timedwait、非默认属性、堆/局部/嵌入 condition 等仍有边界。
`condvar_spurious_wakeup` 与两个静态计数器任务已恢复；其他 barrier 不能因此放行。

### 7.4 隐式库写与 opaque 控制存储

mutex 内部字段、condition/rwlock 控制状态、pthread_create 输出和 join 返回槽有隐式库写，
不一定存在普通 AR Store。不能把调用前的静态初值当调用后的最新读源。
普通内存与控制存储可能别名时拒绝确认；按真实对象、常量范围/共同 stride 字段余数证明不相交。
输出句柄用于 guard/算术/地址时缺少具体摘要仍拒绝，正常 join 身份读取不因此封死。

初版门只挡 offset0，AR 把 mutex 参数 opaque 化为 `si8*`，错误 footprint=1；
offset8 的 glibc owner 字段仍产生假 guard。
当前从原 LLVM CallBase 参数和 DataLayout 恢复完整 allocation size（含 padding），
condition wait 第二参数也用真实 mutex 类型。原类型缺失/opaque/byte/不定长/零大小拒绝。
普通 pthread_t/返回输出槽沿原输出宽度逻辑。
这修复控制宽度门，尚未实现库内部字段具体值和全部生命周期。

### 7.5 stdio 与未支持 API

仅极小常量格式子集有 BMC 摘要：终止的不可变 puts/文本/`%%` 和核实的 promoted32 整数格式。
`%n`、`%s`、动态宽度/格式、返回值依赖不能当 no-op。
受支持的stdout调用按单点成功输出摘要选择严格顺序；与内存事件不能共享clock。
若A目标在A输出前、B输出在B目标前，则A输出先于B输出会通过POSIX stream锁建立
目标HB，查询要求相反输出顺序；另一方向对称。保留输出的Local分类，不把单个
中间输出一律视为HB障碍，否则会无端丢失atomic写后读其值、却仍无HB的真实竞争。
旧公式可能容许stream已排序目标的模型；手工构造的串行完成满足旧clock/rf条件，
独立执行检查明确以targets ordered by pthread synchronization拒绝。旧默认所选模型
在该例上本来ACCEPT，此证据不是新发现的冻结集FP。本阶段1029配对 TP106→106 / TN696→696 / UNKNOWN227→227 / FP/FN/error0，无判定变化。
最小例、puts/printf、值守卫、
改名和真正join的源码对照在双架构通过，所选模型ACCEPT，串行stream证书REJECT。
证据：[calibration](artifacts/bmc-v1-stdio-order-2026-10-10/calibration.json)、
[fixtures](artifacts/bmc-v1-stdio-order-2026-10-10/fixtures.json)。
抽象解释也需记录实际库内存效果；sem_getvalue 共享输出和其他同步 API 输出/超时输入另有 SAFE 审查。
未知 API、混合 callee、C11 原子和其他未建模副作用在实际前缀中拒绝；
未调用函数或目标之后无关同步不应直接否决所有候选。

<a id="search"></a>
## 8. 展开、搜索与性能优化

### 8.1 有界真实 CFG 与资源限制

DFS 保留真实分支条件、动态定义和调用栈，后继复制前缀/栈。
Unreachable/abort/exit 终止对应前缀；不能猜循环次数、删除 guard 或在截断回边上制造 exit。
到普通目标可发出有限前缀，atomic 内目标先保留必要完成尾部。
中断 DFS 分支不发出，已到达真实 target/Return 的完整前缀可保留。

| 资源 | 当前限制/作用 |
| --- | --- |
| 普通展开 | 原 bound1 后 bound16；实际度量是基本块访问，不必等于 C 循环次数 |
| 额外界限安排 | 暂缓组 bound2；创建先小界限、需要时较大界限；真实循环常量提示至多34 |
| 构造 | 路径256、事件1024、DFS步16384、栈深256、helper深8 |
| 查询 | 每组32候选、每轮256候选/1024路径查询，阶段共用原10秒文件 deadline |
| SMT | 每个方向1000ms，UNSAT/UNKNOWN 都不证明 SAFE |
| 诊断细节 | 每候选最多16语句例子、每组前256候选详情；aggregate 覆盖被省略候选 |

这些是内部 BMC 预算，外部 30 秒 task timeout 另行统计。
耗尽/未搜索与 UNSAT 不同；诊断第一阻塞也不能代表被遮挡阶段已经成功或失败。

### 8.2 已采用的公式与查询优化

| 机制 | 前提及保持性 | 已保存观察 |
| --- | --- | --- |
| 动态仿射守卫 | 同一变量及实际定义事件的叶子、同型赋值/实际helper参数、带no-wrap的同符号加减常量；比较消去公共项，不能用于unsigned回绕或不同load | fmaxsymopt恢复；无同源/无溢出证据时保留原搜索 |
| 首方向 SAT 短路 | 保存首个模型后停止，另一方向仅在未确认时执行 | TP58基线逐项不变，正例SAT查询2→1 |
| 两方向 fresh solver | 复制全部基础约束，各方向独立求解，取实际SAT模型 | 回绕例恢复，冻结集TP61逐项不变；不是扩大超时 |
| 同路径 SC 简化 | 完整来源核对后，相关写全是本路径先行精确同区间写，po已确定最新源 | fib5读源吞吐改善，本阶段仍UNKNOWN |
| 创建前 main 写源简化 | 原po/create已证明 main 写先于 child 写，保留全部来源/类型检查 | 六个fib恢复；post-spawn/符号写仍general rf |
| 确定值传播/false guard | 保留read==source等式，再精确simplify；恒false直接拒绝 | 四个fib恢复；未知读不能变自由值 |
| guard优先 | 可行路径仍检查全部unused算术/trap/poison，失败来源不复用cache | ticket恢复，禁止unused溢出/除零对照仍拒绝 |
| atomic约束延后 | 仅拒绝路径省构造，可行路径原排斥约束完整添加 | 全量不变，四fib局部耗时改善 |
| atomic中间结构UNSAT | 闭合段内target前后各有真实障碍，对侧只能段外，必违当前相邻公式 | lu恢复；不证明源程序无竞争 |
| 历史Join提前UNSAT（已替换） | 旧过约束公式在Join前缀上剪枝 | 已随受限实例/完成匹配撤销；无法证完成仍Unsupported |

同路径/main-source 简化证明针对当前有限编码，不能升级成全程序 private/K线程无干扰证明。
fresh solver 之前试过 Int/BV 改整数模表达式，未恢复正例而撤回。
unsigned-wrap 的独立非增量公式 SAT、push 模式超时是本地 Z3 观察，不是所有版本定理。

### 8.3 有依赖前提的 false guard cache

仅同一次 `confirm_race` 的 fixed facts/startup、同展开路径组/界限内缓存非 startup 恒false：
编码开始该路径 cache 为空，guard前无 unencodable；读依赖完整收集。
消费 formal/非直接 global load 禁止复用；本路径所有写与整个对侧路径集合 store union 均须精确 direct global。
只有读集合与对侧 writer union 不相交才复用，不能只看第一次不写 flag 的对侧分支。
递归读依赖一并记录，不跨候选/创建上下文/界限复用，不缓存一般 SMT UNSAT/unsupported。

这减少重复不可行编码，保留 deadline，不消耗额外 SMT 次数；
22轮最小例恢复，TP92全量逐项不变，较大 fib 当时仍卡实际 SMT。
缓存是两前缀内不变性检查，不是一般全程序 private 证明。

### 8.4 分轮搜索与预算公平性

完整路径稳定按较短前缀排序，不删路径；计数器所选创建数15→1降低验证成本，旧15次模型仍未验证。
枚举中断组先暂缓，原查询集合结束后在同一 deadline 查询已有完整前缀；
创建新增短前缀保留旧/主线程候选优先，再必要时重试 bound16。
新短创建候选中不同入口优先于同入口，相同优先级稳定，报告编号仍对应原候选。
这是启发式排序，不是不可行证明；不能永久删掉同入口竞争。

实际循环比较中的15..32常量可提示 n+2，上限34；仅自身在循环中的比较参与，
扫描根及受限helper至多64函数/深8。提示不是循环次数或终止证明，所有真实guard仍成立。

诊断保留 `first_pass_candidates` 与最终 `candidates`，aggregate 两轮合计不重复。
否则 qrcu 只见末轮 search_budget，掩盖第一轮已执行的360次UNSAT/构造耗尽，
不能据此断言延时就能恢复。诊断内容有上限，不等于完整执行证据。

### 8.5 撤回的搜索实验与性能边界

| 撤回实验 | 定向效果 | 全量暴露的代价 |
| --- | --- | --- |
| 全候选中间界限1/2/4/8/16及补充变体 | fmaxsymopt恢复 | TP60总数不变，lamport-b回退；共享文件预算被挤占 |
| 按A/B总前缀长度堆排序 | lu恢复 | TP78不变，fmaxsymopt回退；保留笛卡尔积不保证有限预算更好 |
| helper静态调用图跳过目标搜索 | 原helper控制通过，qrcu/gcd仍UNKNOWN | TP80不变，07_rand替换ticket；撤回源码 |
| 完整前缀直接混入/四对试探 | 部分新候选可查询 | TP76/79均有旧TP回退；改为分轮后才采用 |
| 新激活候选直接提前 | 新数组恢复 | ticket47的旧main/worker确认被挤出deadline；最终分轮后保留 |

冻结集 TP 增减必须比较逐项集合，不能只报净总数。
资源敏感 lu、ticket、fmax 全量/隔离结果均保存；隔离 FALSE 不替换全量 UNKNOWN。

历史 35→58 sweep 的逐任务 wall 合计为631.4→719.7秒，中间版1530.7秒；
它是并行任务耗时合计，不是整体墙钟，也不是受控全局提速实验。
qw、回绕、lu、fib 等有局部改善：例如回绕 wrapper 中位数11.23→0.187秒，
涉及 UNKNOWN→首查询SAT，不能声称全工具60倍加速。
Dartagnan 也可用 CAAT 惰性一致性检查；“全程序必然指数大公式/必然慢”缺乏依据。
同资源、同性质、同界限及相同验证标准的端到端性能比较仍待做。

<a id="correctness"></a>
## 9. 正确性修复、SAFE 覆盖与 TN 代价

### 9.1 最初三个反例及正确性收紧

| 用例/机制 | 旧错误 | 当前处理与依据 |
| --- | --- | --- |
| exclusive-flag | 两worker的共享selector各取1/2，自由load造假FALSE | `4910339`先gate，后SC共享真实初始化；不可同时满足仍不确认 |
| published-before-init | private_init未证明首写前未发布，删掉真候选 | `df63329`禁用 private_init；默认不再SAFE，BMC仍可因动态堆边界UNKNOWN |
| overlapping-widths | 只比起始offset，漏掉不同起点重叠 | `df63329`增加width/范围；两架构default/BMC确认竞争 |
| 未知base仍查询 | 缺实际同址证明的SAT被当竞争 | base/地址前提失败不确认 |

P0 用例和控制保留在 [review-repros/bmc-2026-10-08/](review-repros/bmc-2026-10-08/)。
runner 的20条模式/位宽输出须逐项核对真实 verdict，不能只检查退出码。

后续53→27是激活/实例前提收紧；新增 scull，但撤回27个旧确认。
27→7继续收紧调用、锁历史、整数定义性、真实assume、合成来源和循环到达；
旧27保留4、撤回23，新增lamport/szymanski/Boop早期真实访问。
撤回不等于标签错，也不意味着每条旧反例已经被证明虚假。
保留下来的早期 flag/turn 竞争不等于证明协议最终互斥失败。

最小反例还覆盖 helper互斥共享guard、helper assume、嵌套、默认mutex重入、
整数256转uchar再assume、普通未知call及返回/实参来源。
有些未初始化/volatile局部负例涉及UB，只用于能力边界。
7→35以后恢复靠补合法数据/调用/SC来源，不能恢复自由load或猜循环退出。

### 9.2 新变体持续暴露的缺口

| 审查对象 | 问题与已采用处理 | 仍须追踪 |
| --- | --- | --- |
| helper重复create | 静态站点去重误证单实例；改为保守可重复并核对具体输出/handle读取 | 一般动态创建/祖先和join对应 |
| aggregate入口写 | 只materialize标量，静态数组初值掩盖main真实写；有发布写时先forget非pointer aggregate | 带offset/width字段摘要与关系精度 |
| 锁分区写值 | unlock发布到`_lock_partition`，入口只读平坦黑板，flag被误判0 | 入口合并已发布平坦/锁分区；嵌套保护关系尚不完整 |
| partial-width干扰 | 窄写未覆盖宽读guard，可能删真路径 | 已独立修复；不是当前仍在修复的待办 |
| 动态父实例过滤 | 创建/排序摘要把多个父实例混同，吞真竞争 | 已有独立修复与对照，不归因资源波动的全量+1 |
| TLS与锁分配身份 | 假共享对象FP、假共同锁FN；同入口heap/stack锁摘要身份坍缩 | 独立修复后仍需一般动态对象/寿命证明 |
| stdio/sem/同步参数 | 库访问、共享输出、超时输入或混合callee遗漏 | 已补覆盖；完整API语义仍待审查 |
| pre-main constructor | 静态flag1实际被constructor改0，BMC仍制造flag==1路径 | 有`ar.global_ctors`暂拒绝确认；未来展开真实初始化顺序 |
| 库内部字段 | 锁成功修改控制字，静态零不再是合法读源 | 别名库控制门+原LLVM完整宽度；具体值摘要未实现 |

constructor race-free 控制例在固定 Dartagnan 也报 SOURCE_RACE，processed事件缺少constructor写0。
这不是双工具独立验证通过，更说明参考也需核实源语义。
owner offset8假反例在TP102基线外仍能复现，冻结集FP0没检出；宽度修复后direct/helper双架构退UNKNOWN。
这些新反例说明“先确认没有任何正确性问题再开始精度”无法仅靠有限基准完成；
工程上持续审查明确前提，发现缺陷优先修复，精度增量同时补足证据。

上游 SAFE 规则、这些专项的前提/回退及复现统一见
[AI 手册](ikos-ai-complete.md#safe-rules)与 [证据索引](ikos-ai-complete.md#evidence)。
BMC 不能恢复被上游漏掉的访问或误删的候选。

### 9.3 TN 回退与前端归因

35→58曾有11个TN退UNKNOWN，与更保守动态实例/joined覆盖和aggregate干扰有关，不能只报+23TP。
简单正答计数TRUE+2/FALSE+1净+1，不是正式比赛得分，后者还依赖官方集合/witness。
历史上 stdio 保守覆盖退4TN，单次执行证明恢复3TN，锁入口摘要另退1TN；
这些是第一版时期的转移，后续 SAFE 审查/恢复见 AI 手册，最新 TN 只看 README。

**`mark_internal_inline` 的独立 A/B 已完成**：只撤回 remove NoInline，其他管线相同，
两端1029项逐项一致TP58/TN698，历史11项均UNKNOWN。
调用链显示它位于`if (InlineAll)`，默认全量未开启inline-all；AlwaysInline本来就在上游。
活动控制证实开启时noinline/alwaysinline冲突确实不同，当前修复应保留。
这排除那一行造成11项下降，未给其他历史改动全部归因，也不证明11项已恢复。

锁入口合并唯一退TN的 `28-race_reach_60-invariant_racefree`：
内层锁释放发布x中间值，外层锁保证worker实际只读0，合并丢失嵌套保护相关性。
恢复它需要发布上下文/嵌套保护证明，不能重新忽略所有锁分区写。
Goblint PBR read/unlock是参照，IKOS当前采用保守入口合并，并未实现完整PBR。
摘要类型统一后再逐源cast，避免窄unsigned渐进cast错误符号扩展；已有六种排列C++断言。

<a id="validation"></a>
## 10. 反例记录、witness 与独立验证

### 10.1 四种证据不能混称

| 证据 | 能说明什么 | 不能说明什么 |
| --- | --- | --- |
| YAML标签TP | verdict与预期一致 | 所选模型真实 |
| Dartagnan原生SOURCE_RACE | 固定参考模型找到了某条竞争 | 与IKOS同一动态实例/值/交错 |
| GraphML ACCEPT / 部分约束SAT | 投影或已传入条件可被参考满足 | 被省略的IKOS执行内容已验证 |
| 完整所选执行/witness验证 | 在明确导入/语义契约下验证具体执行 | 全系统普遍soundness，或未被验证的前端语义 |

“反例”是违反性质的执行；“witness”是对它的可交换描述。
一个TP任务可能保存多对访问、多条反例，故任务数、候选数、模型数不能混用。
validator验证witness/指定执行，replayer按具体步骤重算执行，是验证的一种机制；
自写重放器不是唯一可用路线。Dartagnan固定内存模型检查保留作交叉证据；
第一版新增具体AR执行检查补CFG、局部值和动态实例绑定（见§10.6）。

### 10.2 结构化 AR 导出

诊断 SAT 候选的 `replay` 保存AR函数CFG、指令/操作数、类型/布局、global初值；
动态步骤引用实际语句和creator trace/step，保留SMT已编码结果、地址和目标索引。
未编码值不冒充给定输入。`replay.hpp`直接序列化AR，不再调用BMC编码器生成指令语义。
导出标记 `UNVALIDATED`，仍信任前端与快照本身，不是独立C导入；正式YAML2.2导出是另一路径。
TP58阶段58任务全部重新导出并保持FALSE，全量行为不变。

内部长期证据应统一动态thread/create/call/loop身份、对象/extent/offset/width、值、rf、
同步、guard分支、完整创建依赖及源位置一对多映射，让checker和exporter消费同一成功记录。
startup_N标记不是已求出的全局clock，内部函数token也不能冒充实际C地址。

### 10.3 旧 GraphML validator 的校准与失败

CPAchecker4.2.2与Dartagnan4.3.1本地GraphML已接通，32/64位race/locked/exclusive控制通过。
但二者都接受损坏thread ID；Dartagnan还接受不存在目标行和程序另一处竞争。
`WitnessGraph`按行号收集所有线程事件，缺目标时可能不加约束；因此ACCEPT不严格绑定目标/实例。
CPAchecker调用必须同时保留`datarace.spc`和GraphML自动机，不能只`--spec witness.graphml`覆盖性质。

TP35阶段44对ILP32源码前缀投影：

| 工具 | ACCEPT | REJECT | UNKNOWN | TIMEOUT | ERROR |
| --- | ---: | ---: | ---: | ---: | ---: |
| CPAchecker4.2.2 | 20 | 6 | 17 | 1 | 0 |
| Dartagnan4.3.1 | 41 | 1 | 0 | 2 | 0 |

两工具投影联合覆盖34任务，第35个funptr通过原生参考竞争印证；不能称35个原模型验证。
CPAchecker拒prefix但接targets、qw拒投影但独立找到race、funptr不支持指针、fkp超时均单列。
同源行AR合并/排序是源码抽象，会丢列号/表达式内访问及细粒度交错。
targets弱于prefix，不可把降级通过写成更严格证据。

35→58阶段26对投影修正create被挪到初始化循环前的问题后，CPA17ACCEPT/7UNKNOWN，
Dartagnan24ACCEPT，另2对实例歧义拒导出。
新增outer-counter另一次CPAUNKNOWN/DartagnanREJECT，包括读写翻转对照也拒绝；
不能断言只是翻转顺序就修好。其源码探针仍找到真实data竞争。
源码投影把相邻行强制成CAT hb，而IKOS总调度先后不等于该关系，是已知不匹配。

CPAchecker4.2.2解析正式YAML2.2 violation_sequence失败，没有验证结论。
现代CPA源码`bd91e2fd…`仅是实现参照，不代表已构建运行或官方指定版本。
正式并发witness2.2验收须固定真正支持它的validator及BenchExec配置，再校准坏实例/值/地址/目标。
现有ACCEPT不等于官方验收。

### 10.4 Dartagnan 指定候选研究接口

`scripts/dartagnan_strict/`在隔离目录核对固定AssumeSolver源码hash，编译hook并复用原JAR依赖，
不覆盖本地参考包。程序/内存模型仍由Dartagnan编码，证书约束指定事件、动态线程/creator、
目标、值、对象相对地址、rf/co及已映射内存顺序。
核对IR指纹、事件descriptor、类型/宽度/范围，不接受未知字段/错误身份。
证书模式关闭线程对称性破缺，避免指定实例被对称选择删除；保留许可`DAT3M-LICENSE`。

无证书保留原生性质查询。证书模式需检查指定对的双方向相邻可行性，
不再叠加原生单方向存在性公式：固定读到新写值的合法方向不能随意翻转。
read-after-write控制的旧接口读0SAT/读7UNSAT，修正后两种合法取值可满足；
fkp2014/13_unverif三组旧固定值UNSAT随后SAT，改store值仍UNSAT。
旧冲突被保留和解释，不能因标签命中忽略，也不能把映射错误UNSAT当原模型假。

| 映射增量 | 明确绑定的内容 | 仍有边界 |
| --- | --- | --- |
| 相对地址/间接访问 | 唯一静态来源+object base/byte offset/width，不靠打印字符串猜地址 | 同行多静态访问歧义仍拒绝 |
| 类型descriptor | 实际IntegerType区分同源行pointer/int load | 固定参考ILP32 pointer存储64bit，不能把IKOS4字节槽绑定成8字节 |
| 内存occurrence | 唯一pre-unroll身份及已执行前序clone，区别重复访问 | 跳迭代/复杂call尚未完整绑定；每次重新定义先清旧模型值 |
| nondet | 精确声明白名单交集、实际Local/NonDetValue、位宽/符号及源位置 | size_t、参考long64与ILP32long32、多个动态occurrence/优化来源有边界 |
| 显式rf/co | 按已映射同区域实际clock绑定读源/写序，不只值相等 | unknown地址、缺/并列clock、混合宽度仍显式遗漏 |
| 初始rf | 唯一同对象/offset/位宽的实际Init，不能充当race目标 | padding/部分重叠初始字段拒绝 |
| 内存相对顺序 | 独立ikos_schedule延伸完整已编码SC图 | 不把clock直接加成CAT hb边，未绑定完整总调度 |
| 单次heap Alloc | 同源码唯一allocation、大小/creator/descriptor、实际地址base+offset | 动态/重复分配、pointer值及完整寿命未绑定 |
| 跨分支创建序列 | 按实际父trace全创建顺序选forward CFG clone，绑定执行occurrence和前缀创建总数 | 完整CFG/call/一般内存occurrence仍有限制 |

SC顺序绑定需要完整原生hb边，不能用Acyclicity只保留SCC/must推导后的削减图当排序证书。
研究接口在预处理前增加`ForceEncodeAxiom(hb)`，只要求编码，不增删原内存模型约束。
校准覆盖“相同值的不同写仍不能替代来源”和“独立对象执行先后不意味着CAT hb边”。

FK旧mapper把首次consumer和producer映射到同一循环互斥分支，导致双架构UNSAT；
修复沿实际consumer22→producer38而非producer16，部分SAT，坏值/创建数UNSAT、坏creator拒绝。
这项修改属于研究adapter，不能算分析器能力或额外TP。

### 10.5 历史部分模型覆盖与当时缺口

| 历史模型集合/检查阶段 | 结果 | 解释 |
| --- | --- | --- |
| TP58，71候选，bound2 | SAT38 / UNMAPPED31 / UNSUPPORTED2 | 部分固定内存模型 |
| TP77，95候选，最初 | SAT68 / UNMAPPED20 / UNSUPPORTED2 / UNSAT2 / TIMEOUT3 | 全候选参与，不等于全模型验证 |
| 同集合，nondet/重复访问增量 | SAT80 / UNMAPPED8 / UNSUPPORTED2 / UNSAT2 / TIMEOUT3 | 14次局部nondet绑定 |
| 同集合，rf/co增量 | SAT82 / UNMAPPED8 / UNSUPPORTED2 / UNSAT2 / TIMEOUT1 | 254条通信边，原超时转SAT不宣称加速 |
| 重导TP77，顺序检查 | SAT83 / UNMAPPED7 / UNSUPPORTED2 / UNSAT1 / TIMEOUT2 | 只验证已传入条件，不是83个完整反例 |

fmax界限2不足，6可部分SAT。实际N次创建不等于bound=N：循环退出常需再访问头部，
最小count3在bound3不满足、bound4满足。旧counter15创建模型扩大bound后仍超时；
后来短前缀选择真实1次创建，部分SAT，不是修改旧模型成较小值。
airline15/20超时、25未导IR/未映射，以及未支持funptr/object等记录均保留。

后续typed37、heap38、condition、数组、ERRORCHECK和field模型有局部SAT/改值UNSAT，
仍遗漏完整CFG/call occurrence、全部局部值、同步历史和总调度。
fib5早先bound16超时，后来新模型bound13双架构部分SAT；fib10/12指定模型仍有60秒超时。
**坏模型被拒绝不能把原模型超时算成验证通过。**
状态继续为`PARTIAL_FIXED_MEMORY_MODEL`，`full_counterexample_validated:false`。

第一版前已有未跟踪 `scripts/replay_bmc.py` 只读条件试验：9个ILP32 byte模型及FK可ACCEPT，
改读值/creator均REJECT；byte的LP64因NULL pointer-byte tuple与整数零检查不一致UNSUPPORTED。
创建数组曾因main(argc,argv)零实参初始化失败，尚未执行事件。
它依赖同IKOS AR快照与glibc合同，没有独立C导入；脚本未提交，证据以本地hash为条件，
不能声称全新checkout即可复现，更不能算正式2.2验证通过。

### 10.6 第一版具体执行检查（2026-10-10）

`validate_bmc_execution.py` 独立于 Z3/编码器，按程序快照的完整 CFG、动态调用栈和
线程创建执行证据。字节内存从 typed 初值/真实 store 计算 load；证据内的值、地址、
宽度和逐字节 read_from 必须与实际执行一致。vector clock 检查 spawn、mutex/rwlock、
join 和 stdio stream 同步；命名/verifier atomic 检查完整边界与不交错。condition 仅选择
合法 spurious wake，并实际 release/reacquire 已拥有的 mutex，不能制造 signal HB。
单次 pre-spawn main malloc 不默认为零；属性仅支持已核实 glibc ERRORCHECK 生命周期。
库 ABI 由快照直接导出原 LLVM target/calling convention/参数对象大小和对齐后核对，
不会把编码器认定“支持”当作验证前提。未知 API/入口签名/初值/对象返回 UNSUPPORTED。

快照与证书分别用 SHA256 绑定；CLI 另核对输入 hash。源→AR 前端、快照真实性、
成功的库调用与 SC/verifier 合同仍是显式可信边界。ACCEPT 的准确含义是具体 AR 执行，
**不是官方 C-source / witness2.2 验收或普遍 soundness 证明**。正常分析器仍输出 UNVALIDATED，
离线 sweep 单独保存验收结论，不能把未运行过的证据升级为已验证。

第一版执行检查初始阶段结果：[129组逐执行记录](artifacts/bmc-v1-execution-2026-10-10/results.json)、
[汇总](artifacts/bmc-v1-execution-2026-10-10/summary.json)、
[全量配对](artifacts/bmc-v1-execution-2026-10-10/paired.json)。104项标签TP共129组：
128 ACCEPT、1 UNSUPPORTED、0 REJECT；103项全部所选组通过，另1项的函数指针
非简单初值尚未导入。没有将这一项计为假反例，也没有把它计为已验证。

**2026-10-10 第一版最终发布检查当时106项的132组：132 ACCEPT、0 REJECT、0 UNSUPPORTED。**
共享函数初值/真实callee、stdio目标顺序等后续机制都已包含在这次最终证据中；
不是沿用初始128组验收来覆盖新增模型。见[最终汇总](artifacts/bmc-v1-final-2026-10-10/execution-summary.json)
和[逐组记录](artifacts/bmc-v1-final-2026-10-10/execution-results.json)。

242条双架构断言使用真实分析器输出，篡改创建步骤、目标实例、值、偏移、宽度、
read_from、重复trace、synthetic边界和快照hash必须REJECT。命名atomic的调用事件
发生在合成begin之前，检查器不能提前禁止这一间隔的其他线程；初轮6个误拒绝已按
AR调用/atomic边界更正，不能据此撤回6个TP。stdio保留POSIX stream同步，不能仅当作
无效果调用。AR展示字段含ASLR地址，仅供阅读，不用作对象/实例身份。

本模块在保留未跟踪原草稿不变的前提下，经审查和校准另行维护；原草稿hash
`c430b1d07824db796008688f5d768bb04b7086a33c468ee7630a5e9a8d6422ec`。
`evidence.hpp` 从查询函数分离所选证据导出；纯序列化改动build/install、P0双架构、
诊断开关对照、1029全量均通过，TP104/TN696/UNKNOWN229不变，无逐任务转移。

2026-10-11 后续重新导出的全部当前执行结论见 [README](README.md#current)。
隐式 main 栈、helper 发布和递归类型诊断已加入导出/检查边界；递归类型诊断修复只解决
快照序列化，不能称新增语义或 verdict 收益。新增确认不能沿用第一版132组结论。

复现：

```bash
python3 scripts/test_bmc_execution.py
python3 scripts/sweep_bmc_execution.py --reports <本轮确认任务报告目录> --output /tmp/NEW
```

<a id="workflow"></a>
## 11. 具体实施、回归与提交流程

### 11.1 按差异用例迁移通用能力

1. **固定任务与差异**：从最新UNKNOWN台账选任务，记录YAML/input hash、ABI、性质、CAT、
   参数、预算和两端实际二进制。先让固定Dartagnan确认SOURCE_RACE；没有确认时记录PASS/UNKNOWN/TIMEOUT/ERROR。
2. **事先推导有限执行**：从C逐步列创建、guard、局部值、共享rf、地址范围、同步及必要其他线程。
   不要求两工具选相同反例，不从总bound猜实际创建次数。
3. **观察参考机制**：保存LLVM/processed IR、动态事件、creator、rf及所选访问。
   必要时隔离env-gated探针，同时保留无探针结果，不覆盖比赛工具包。
4. **定位IKOS真正阻塞层**：AI覆盖、候选过滤、展开、编码还是输出；上游零候选须先修覆盖。
   保留C→LLVM→AR一对多来源，不凭行号/函数名合并动态实例。
5. **实现一个完整通用增量**：写清前提/方向/证据/失败回退，联动必要依赖。
   复用已有AR、CandidateFacts、展开和SC机制；不照搬参考限制、默认假设或整套慢编码。
6. **验证执行与泛化**：所选模型部分/完整验证范围分列；最小正例、对照、改名/helper/控制流/宽度变体。
   重点检查只有一次create、互斥创建、共同锁、顺序create/join和错误值/来源。
7. **逐项A/B与台账更新**：执行下面的适用检查，记每条变化、错误/超时和证据覆盖。
   FP/FN或假反例先处理，不能用TP净增抵消。不能实现的记录具体缺口，不写“数学不可能”。
8. **立即独立commit**：验证通过后提交此阶段，message写机制及前→后配对数。
   不等待全部UNKNOWN处理完；一个commit一件事，前端单独A/B和提交。

这符合 `ikos-conc-semantics` 的语义契约、原始文献/版本核对、泛化与保守回退，
也符合 `ponytail` 的最小完整改动。没有新增审批步骤或一般框架要求。

### 11.2 构建与行为回归

以下命令均在 `/home/ruan/ikos-conc` 根目录；不能先cd build再使用build/analyzer相对路径。
运行全量前保存旧二进制/同配置结果，并确认可用资源；不要并发跑两套互相干扰的全量。

```bash
cmake --build build --target ikos-analyzer -j"$(nproc)"
cp build/analyzer/ikos-analyzer install/bin/ikos-analyzer
cmp build/analyzer/ikos-analyzer install/bin/ikos-analyzer

# 单文件：默认may分析，与BMC确认模式区分
install/bin/ikos --analyses=race --concurrency=auto <file.c>
IKOS_BMC_RECOVER=1 install/bin/ikos --analyses=race --concurrency=auto \
  --demote-race-to-unknown <file.c>

# 原P0反例/控制，需核对输出中的verdict
python3 docs/review-repros/bmc-2026-10-08/run.py --bits 32
python3 docs/review-repros/bmc-2026-10-08/run.py --bits 64

# 冻结集，固定两端jobs/timeout/任务人口
IKOS_BMC_RECOVER=1 python3 svcomp_race_test.py --demote --jobs 4 --only-fail
# 滚动集仅作补充，不替代冻结集配对
IKOS_BMC_RECOVER=1 python3 svcomp_race_test.py /home/ruan/sv-benchmarks --demote --jobs 4
```

恢复同时需要demote选项和`IKOS_BMC_RECOVER`存在；`getenv`检查使值0也会开启，关闭A/B应unset。
`install/bin/ikos`是wrapper，真二进制为install/bin/ikos-analyzer；
Python改动需同步wrapper实际加载的 `install/libexec/lib/python3.12/site-packages/ikos/`。
前端改动还要核对真实ikos-pp和管线，并单独测试AI SAFE影响，不能混进BMC恢复阶段。

按改动选择现有针对性套件，基础入口包括：

```bash
python3 analyzer/test/regression/bmc-correctness/runtest
python3 analyzer/test/regression/bmc-address/runtest
python3 analyzer/test/regression/bmc-candidates/runtest
python3 analyzer/test/regression/bmc-values/runtest
python3 analyzer/test/regression/bmc-instances/runtest
```

同步/对象/搜索回归入口集中在 [review-repros/](review-repros/)，各阶段证据索引保留具体套件。
文档或研究mapper变化不影响analyzer行为时，检查对应链接/映射校准即可，不重复不相关全量。
不要写镜像实现的无意义测试；语义增量的正反例必须真正区分合法/虚假执行。

### 11.3 诊断与验证复现

输出使用新临时目录；脚本保存具体命令、返回码、分类、输入/工具hash。

```bash
python3 scripts/bmc_diagnostics.py --output /tmp/ikos-bmc-diagnostics-NEW
python3 scripts/bmc_diagnostics.py \
  --input /home/ruan/sv-benchmarks-2026/c/pthread-atomic/scull.i \
  --output /tmp/ikos-bmc-scull-NEW --jobs 1

python3 scripts/test_bmc_witnesses.py
python3 scripts/validate_bmc_witnesses.py \
  --pairs docs/artifacts/bmc-precision-2026-10-09/confirmed-pairs.json \
  --output /tmp/ikos-witness-NEW --jobs 3 --timeout 60

python3 scripts/dartagnan_strict/calibrate.py --output /tmp/ikos-strict-NEW
python3 scripts/dartagnan_strict/sweep_ikos.py \
  --reports <本轮导出的confirmed-reports目录> --output /tmp/ikos-fixed-model-NEW
python3 scripts/dartagnan_strict/calibrate_heap.py --output /tmp/ikos-heap-calibration-NEW
python3 scripts/dartagnan_strict/test_indirect_addresses.py
```

参考wrapper的 `-Xmx2g` 不一定限制它另启动的engine JVM；历史UNKNOWN遍历只限制launcher，
不能声称两进程统一2GiB。研究hook的直接JVM设置与官方资源配置也不同。
超时杀进程组，分类核对ExitCode：固定版BOUNDED_RESULT=1、UNKNOWN_ERROR=30，
内部FAIL后launcher失败不能算有效竞争。

### 11.4 证据与文档维护

只提交小型summary/results/transitions/confirmed-pairs、源码回归和结论；
大日志、数据库、编译类和二进制放临时目录并gitignore。历史/tmp可能清空，
不可声称它们长期可得；核心结论需由Git内小证据、输入hash和复现流程支撑。
未经验证/未跟踪脚本不能混入文档commit。临时探针用完删除，必要观测保持env-gated。
`log::debug/info/...`返回void；流式日志用`log::msg()`。
稳定顺序按语义值/源身份而不是指针地址，防止ASLR改变搜索。

后续 BMC 机制与证据只更新本文；SAFE/干扰/反向反馈更新 AI 手册，
最新基线更新 README，工程命令更新开发速查。不再增加平行阶段 Markdown。

<a id="roadmap"></a>
## 12. 第一版验收、后续范围与结束条件

当前没有必须用户补源码/安装工具才能推进的阻塞。
官方2027任务清单、validator版本及实际BenchExec参数需要组织者明确；暂不阻塞2026回归。
普遍soundness的学术声明需要完整契约和独立同行审查，不能由有限TP/模型试验替代。

| 阶段 | 小目标/实施落点 | 验收与暂时停止条件 |
| --- | --- | --- |
| P0持续正确性/证据 | 审查新规则与SAFE覆盖；完善指定实例/值/地址/同步/CFG绑定 | 发现假模型或FN先修；部分SAT不升级完整验证 |
| 第一版已完成：共享回调 | 有来源function-pointer SC + 实际callee等式 + 动态调用/导出 | 最小正反变体、参考probe、所选证据、全量旧TP/TN变化明确后提交 |
| 对象/句柄/依赖闭包 | 动态allocation/lifetime、join实例、必要第三线程 | 每次只补当前具体任务所需通用子集；来源/副作用不可证继续UNKNOWN |
| 同步/搜索 | 有限属性/失败API、atomic闭包、公平调度与公式规模 | 先合同和保持性；预算实验逐项A/B，不能替换旧成功任务却称提升 |
| IKOS协同与TN恢复 | 字段化interference、嵌套保护、条件化may/must证书 | 不恢复旧漏报；消融测候选/依赖规模、结果、时间/内存 |
| 最终外部验收 | 固定官方集合和现代并发witness validator | 坏实例/值/地址/目标校准、完整源witness验收与受控性能比较 |

不是一次证明“没有所有正确性问题”以后才允许恢复精度，也不是做完validator就停止优化。
每轮精度扩展同时建立执行依据，遇到新正确性缺陷立即优先处理。
遍历当前UNKNOWN后，每项必须有参考状态、实际阻塞、已尝试机制、结果和剩余前提；
新UNKNOWN要进入台账。只在这一轮可实施的通用增量均尝试、缺口具体记录且基线复现后总结，
不因单次超时或TP平台期宣称达到数学能力上限。
不能实现的边界可以暂放过，但须写需要哪类语义/证明/官方信息，而不是归因“大模型能力不足”。

### 12.1 第一版已完成：实施与验收

第一版是候选驱动的 SC 有界确认器：保留 IKOS 抽象解释负责 SAFE、may 候选、
范围与局部无溢出事实的分工；不以清空 UNKNOWN 或复刻 Dartagnan 为结束条件。

第一版范围与验收要求：

1. **结果/证据接口**：普通运行也返回所选模型的程序快照、动态前缀、创建绑定、
   值/地址与目标；诊断仅追加计数与示例。未确认结果显式分类，不宣称 SAFE。
2. **执行证据检查**：独立于 Z3/编码器规则执行 AR 前缀，校准坏实例、值、地址、
   读源和路径；支持外的 API/对象返回 UNSUPPORTED。AR 验收不冒称官方 C/witness 验收。
3. **核心语义**：有限共享函数指针的真实 SC 来源与调用等式、受限 join 实例匹配；
   分配/字段干扰仅扩展有具体任务与前提的通用子集，未知来源/寿命保守不确认。
4. **IKOS 协同**：标明 AI 事实的适用条件与依赖覆盖，受控消融记录候选/事件/查询、
   结果、时间/内存。字段/嵌套保护的 TN 回退单列，不能只报告 TP 增长。
5. **发布状态**：每阶段 build/install、双架构 P0、相应语义回归、1029 配对后立即独立
   commit。剩余 UNKNOWN 均有参考状态、具体阻塞与后续所需语义。

第一版验收结论如下；行为阶段分别build/install、双架构P0、相关回归和全量配对后提交，
未改frontend。最终发布只补证据/文档，不改变已验证二进制。

| 完成项 | 实现/验收 | 独立提交 |
| --- | --- | --- |
| 结果/证据接口 | 普通运行保留counterexample，诊断开关同证据；全量104→104 | `71599ad` |
| 独立AR执行检查 | 原CFG/调用/实例/字节值/地址/rf/同步逐事件检查，242条双架构断言及损坏校准 | `5ad8814` |
| 有限共享回调 | 实际SC函数指针值绑定实际callee；28个双架构语义案例，比赛104→106 | `e40db83` |
| 受限join完成 | 实际创建句柄匹配无副作用return-NULL子线程；20个双架构案例，比赛106→106 | `539a0b4` |
| stdio查询正确性 | 排除会建立目标HB的stream顺序，保留合法其他顺序；8个双架构案例和串行坏证据拒绝 | `b8a2762` |
| AI契约与可测协同 | 依赖覆盖、数值证书适用条件；普通metrics/跨轮累积；10输入60次消融 | `8c0ec5a` |
| 第一版最终发布 | 1029普通/诊断逐项相同；106项132组AR ACCEPT；当时227项UNKNOWN有记录 | 历史文档/小型JSON |

本版只新增两个比赛TP：`04-mutex_27-base_rc.c`与
`char_generic_nvram_read_nvram_write_nvram-race.i`，均有固定版Dartagnan竞争依据。
join改善最小反例，不冒称比赛TP增益；stdio修模型可行性，不冒称发现了新的冻结集FP。
阶段TP/TN数字、前后任务与测试限制均保留在各阶段JSON。

**可以进一步完善IKOS向BMC供信息的架构，且第一版已做有限实证。**
AI提供may候选/对象范围、AR布局/精确初值、受限局部no-wrap证书；BMC负责实际值源/实例执行，
不把抽象事实充作真实store。no-wrap正例总时间开/关为0.184/0.464秒，solver为
3379/279123微秒，证明这类证书可以减少重复求解。其他比赛样本基本持平，
尚无全局大幅加速、更快于Dartagnan或显著减少依赖规模的证据（§3.1）。
字段化干扰、嵌套保护、条件化may/must摘要仍是后续工作，未混入第一版成果。

**第一版当时在这里结束**：具备有限候选确认、可独立核对的所选执行、AI使用契约/受控测量、
可复现基线和完整当前UNKNOWN记录。这个停止点是已授权工程范围完成，
不意味着全部UNKNOWN不可恢复、所有正确性问题已证明不存在或普遍分析能力到达上限。
一般 K 线程/堆图/寿命、弱内存、公平性、CAT 引擎和官方 witness2.2 验收可留到后续版本。

**结果接口阶段**：本阶段1029项固定jobs4/30秒A/B为 TP104→104、TN696→696、UNKNOWN229→229、
FP/FN/error0，无逐任务判定变化；见 [results证据](artifacts/bmc-v1-results-2026-10-10/summary.json)。
值回归双架构100条与P0双架构20条通过。旧候选回归的paths期望已在原版复现过时：
保留的完整前缀能确认竞争，不能把枚举预算耗尽等同于所有候选无法确认。

`result.hpp` 将结果与 `Counterexample` 分离，check/search/checker 传递
证据，普通报告 `info.bmc_result` 与确认时的 `info.counterexample` 无需诊断开关；历史
诊断候选仍保留 replay 以兼容已有脚本。证据在 SAT 时生成，不为失败路径复制整个程序。
证据状态仍为 UNVALIDATED。诊断关/开同证据对照覆盖五个值/调用/未初始化变体、双架构；
AR 展示字符串含 ASLR 名称，比较时只排除这一展示字段，保留全部语义内容。

<a id="callback-plan"></a>
### 12.2 共享回调的已实现子集与参考依据

目标 `04-mutex_27-base_rc`：f初始good，main创建后持fm写bad；worker持fm读f再调用，
main持gm读global。读bad后调用的写不持gm，能够竞争；读good后的写仍受共同gm保护。
此前IKOS卡call_summary；现已展开兼容的有限addressed targets，并在查询中绑定实际函数值，不能任选may-callee。

固定7caef5c、bound2双架构新跑SOURCE_RACE；参考原生探针结果：

| 值源与调用目标 | ILP32 | LP64 |
| --- | --- | --- |
| f从main bad-store读取，执行bad写 | SAT | SAT |
| f从初始good读取，却执行bad写 | UNSAT_AT_BOUND | UNSAT_AT_BOUND |
| f从初始good读取，执行受共同锁保护的good写 | UNSAT_AT_BOUND | UNSAT_AT_BOUND |

`NaiveDevirtualisation`枚举addressed functions并加`funcPtr==target`分支，
`Inlining`复制动态register scope，WmmEncoder的rf/co约束读值。
该参考还删除自递归候选并打印“unsoundly assumes”，不迁移这一限制。
这些是固定参考探针；本轮IKOS模型另由独立AR执行检查验收。

本轮已实现的通用范围：

1. IKOS may-callee只排候选/搜索，旧查询优先；先支持精确global固定offset函数指针槽。
2. 来源仅真实function/null静态初值和startup/A/B完整宽度function-store；
   TLS、partial bytes、普通数据指针转换、未知初值/源继续拒绝。
3. 复用SC clock和最新写源，每次调用约束实际函数身份等于本次展开callee，
   核对原LLVMsignature、calling convention、参数及返回类型。
4. 保留动态调用、定义、实参和return上下文，先非递归；未知/递归需要有界真实上下文或UNKNOWN。
5. 导出函数语义身份和pointer rf，更新mapper；内部token不是可直接送validator的C地址。
6. 正例、初始good、不可执行bad-store、签名不兼容、未写pointer、helper/改名/多实例，
   然后build/install、P0、相关回归、1029配对、独立commit。

`function_values.hpp` 单独管理 startup/A/B 的动态函数值缓存和SC读源；
`unroll.hpp` 枚举原LLVM签名/ABI兼容的被取址函数，`encode.hpp` 绑定本次实际调用。
startup也重新解析实际地址，避免helper中的初始写被遗漏。函数/null相等/不等守卫
复用同一缓存；函数指针返回值、递归、TLS及部分字节写尚不支持。
快照补充变参标记；只在实际定义为variadic且没有执行va_arg等未支持语句时允许额外实参。

本阶段1029配对为 **TP104→106 / TN696→696 / UNKNOWN229→227 / FP0 / FN0 / error0**，
两个新增项为`04-mutex_27-base_rc`和`char_generic_nvram_read_nvram_write_nvram-race`，
均有固定Dartagnan竞争记录；全部旧TP保留。14类改名正/反例与helper、startup、守卫、
变参、多实例变体在双架构通过，所有确认的callees/pointer身份篡改均被拒绝。
106任务132组所选执行全部AR ACCEPT；该结论不冒称官方C/witness验证。
NVRAM第一次被检查器拒绝是快照遗漏合法variadic元数据，补足后重新导出并验收，
不是忽略拒绝或根据标签放行。行为数字见[配对](artifacts/bmc-v1-shared-callback-2026-10-10/paired.json)，
[执行检查](artifacts/bmc-v1-shared-callback-2026-10-10/summary.json)，
[变体](artifacts/bmc-v1-shared-callback-2026-10-10/fixtures.json)。

### 12.3 后续语义与停止条件

当前竞争 UNKNOWN 数量和参考状态见 README；旧混合台账包含安全任务，不能整表当竞争池。
按以下真实缺口选择有限通用子集，记录参考竞争、实际 AR、阻塞及前提，不承诺新增 TP 数：

| 方向 | 必须补足的前提 |
| --- | --- |
| 动态创建/激活 | 真实 entry、实参、创建 occurrence 与祖先前缀；静态 site 不等于唯一线程 |
| 对象/地址 | worker/helper 动态 allocation、栈/TLS、extent、失败、发布与寿命 |
| 字节 API | calloc/memset/memcpy 的真实大小、重叠、初始化与定义性合同 |
| 有副作用第三线程 join | 实际句柄、完整终止和相关读写，不能任选另一端或假完成 |
| 同步/操作 | 有限失败返回、原子/RMW 闭包、AR 操作/调用与机器定义性 |
| 搜索/IKOS 事实 | 保留旧成功路径；同语义、同 bounds 消融查询/事件/时间/内存，不用预算代替来源 |

TN 的嵌套保护/发布相关性独立见 [AI 后续机制](ikos-ai-complete.md#roadmap)。
真正 BMC→AI 归纳反馈仍是研究设计，不能消费普通候选有界 UNSAT 证明 SAFE。
每轮结束于通用机制前提明确、最小正反变体/相关回归/全量配对和所选执行检查完成并提交；
不可实现或零收益部分记录具体义务，不把暂停称为数学能力上限。
官方 manifest、现代 validator 和实际 BenchExec 合同确定后再做外部验收。

<a id="history"></a>
## 13. 阶段历史与不再采用的判断

历史成绩变化和逐提交因果保留在 Git 及 §14 的小型 JSON，不再复制整条流水账。
需要记住的架构变化是：A0 封自由 load → 实际值源/激活收紧 → 有来源 SC/调用/同步恢复 →
结构化执行与独立检查 → AI 覆盖审查及受限协同。各阶段任务/预算不同，不能拿旧数字估上限。

多轮未提交 diff 曾由用户拆为 e8bab77…3e36320，后续必须每个机制验证后独立提交。
未知状态历史已多次更新，旧台账/参考标签分歧仍保留，不能盲目跟随参考改答案。

明确撤回/修正：

- “标签FALSE正确即可证明反例真实”“独有TP就是可靠优势”：不成立。
- 旧Dartagnan176/IKOS80/交集57/独有23（另报告改19）未同配置核实，旧ERR分类也有歧义；不作当前收益依据。
- “约15个lost TP都是初始化后只读”“地址一项可救约6个”“TP≈140”“最后约35个数学上不可恢复”：缺乏逐项证据。
- “hb=po+com+mutex+assume”“相邻等价无hb”“两工具时钟相同所以语义相同”：错误。
- “Dartagnan浮点必crash、数组join全UNKNOWN、IKOS自然覆盖C11重排优势”：缺少固定配置对比与完整反例证据。
- `target/classes/git.properties`遗留4.1.0曾误导版本判断；实际运行launcher+library均4.3.1/7caef5c。
- 历史workstealqueue null-deref本轮未复现，ASAN未报错；不把路径栈修正冒称已证明原崩溃根因。
- 脚本Recall排除UNKNOWN；FN0/Recall1不能解释成检出全部235个racy任务。

<a id="evidence"></a>
## 14. 证据索引

每条对应一个原报告的证据来源，正文已按机制合并。目录内保留实际summary、配对、
probe/校准/所选模型与hash；它们的日期/数字是**历史阶段**，不是当前基线。
原报告正文可从本次合并提交的父提交查看；不再维护平行Markdown。
JSON中旧报告路径是不可改写的实验来源元数据，查结论请按本文章节和以下索引定位。

### 后续 AI 协同与当前执行验收

- [main-stack](artifacts/ai-precision-2026-10-11/main-stack/summary.json)：隐式 main 的真实共享栈对象。
- [publication](artifacts/ai-precision-2026-10-11/publication/summary.json)：AI 生命周期合同用于实际 helper 发布。
- [diagnostic-types](artifacts/ai-precision-2026-10-11/diagnostic-types/summary.json)：递归类型导出与重新检查，当前基线入口。
- 上游覆盖/SAFE、双向反馈原型的零收益及后续 TN 工作：[AI 机制索引](ikos-ai-complete.md#evidence)。

### 第一版实现与发布

- **结构化结果**：[summary](artifacts/bmc-v1-results-2026-10-10/summary.json)。
- **独立执行检查初始阶段**：[summary](artifacts/bmc-v1-execution-2026-10-10/summary.json)。
- **第一版共享回调（2026-10-10）**：[paired.json](artifacts/bmc-v1-shared-callback-2026-10-10/paired.json)；[summary.json](artifacts/bmc-v1-shared-callback-2026-10-10/summary.json)；[results.json](artifacts/bmc-v1-shared-callback-2026-10-10/results.json)；[fixtures.json](artifacts/bmc-v1-shared-callback-2026-10-10/fixtures.json)。
- **受限join**：[paired](artifacts/bmc-v1-join-2026-10-10/paired.json)、[fixtures](artifacts/bmc-v1-join-2026-10-10/fixtures.json)、[reference](artifacts/bmc-v1-join-2026-10-10/reference.json)。
- **stdio目标顺序**：[paired](artifacts/bmc-v1-stdio-order-2026-10-10/paired.json)、[calibration](artifacts/bmc-v1-stdio-order-2026-10-10/calibration.json)。
- **AI事实/度量消融**：[paired](artifacts/bmc-v1-ai-facts-2026-10-10/paired.json)、[ablation-summary](artifacts/bmc-v1-ai-facts-2026-10-10/ablation-summary.json)、[ablation-results](artifacts/bmc-v1-ai-facts-2026-10-10/ablation-results.json)。
- **最终发布**：[diagnostic-paired](artifacts/bmc-v1-final-2026-10-10/diagnostic-paired.json)、[execution-summary](artifacts/bmc-v1-final-2026-10-10/execution-summary.json)、[execution-results](artifacts/bmc-v1-final-2026-10-10/execution-results.json)、[unknown-summary](artifacts/bmc-v1-final-2026-10-10/unknown-summary.json)、[unknown-results](artifacts/bmc-v1-final-2026-10-10/unknown-results.json)。

### 架构、审查与参考驱动

- <a id="record-ikos-bmc-review-2026-10-08"></a>**IKOS BMC 文档与实现审查（2026-10-08）**：[bmc-2026-10-08](review-repros/bmc-2026-10-08/)。
- <a id="record-ikos-bmc-correctness-2026-10-09"></a>**BMC 正确性审查与回归（2026-10-09）**：[metadata.json](artifacts/bmc-correctness-2026-10-09/metadata.json)；[source.patch](artifacts/bmc-correctness-2026-10-09/source.patch)；[baseline-dirty.patch](artifacts/bmc-correctness-2026-10-09/baseline-dirty.patch)；[transitions.tsv](artifacts/bmc-correctness-2026-10-09/transitions.tsv)；[confirmed-pairs.json](artifacts/bmc-correctness-2026-10-09/confirmed-pairs.json)；[audited-tasks.json](artifacts/bmc-correctness-2026-10-09/audited-tasks.json)；[minimal-ab.json](artifacts/bmc-correctness-2026-10-09/minimal-ab.json)；[consumption-final.json](artifacts/bmc-correctness-2026-10-09/consumption-final.json)。
- <a id="record-ikos-bmc-precision-2026-10-09"></a>**BMC 精度恢复与 IKOS 事实接口（2026-10-09）**：[bmc-precision-2026-10-09](artifacts/bmc-precision-2026-10-09/)；[manifest.json](artifacts/bmc-reference-guided-2026-10-09/manifest.json)。
- <a id="record-ikos-bmc-reference-guided-plan"></a>**BMC 后续实施方案：以具体差异用例验证并迁移参考语义**：[manifest.json](artifacts/bmc-reference-guided-2026-10-09/manifest.json)。
- <a id="record-ikos-bmc-unknown-sweep-2026-10-09"></a>**UNKNOWN 全量遍历与参考驱动精度恢复**：[results.tsv](artifacts/bmc-unknown-sweep-2026-10-09/ledger-pruned/results.tsv)；[results.json](artifacts/bmc-unknown-sweep-2026-10-09/ledger-pruned/results.json)；[full-before-paired/summary.json](artifacts/bmc-unknown-sweep-2026-10-09/full-before-paired/summary.json)；[full-pruned/summary.json](artifacts/bmc-unknown-sweep-2026-10-09/full-pruned/summary.json)；[reference/summary.json](artifacts/bmc-unknown-sweep-2026-10-09/reference/summary.json)；[reference-supplemental/summary.json](artifacts/bmc-unknown-sweep-2026-10-09/reference-supplemental/summary.json)；[reference-entry-supplemental/summary.json](artifacts/bmc-unknown-sweep-2026-10-09/reference-entry-supplemental/summary.json)；[manifest.json](artifacts/bmc-unknown-sweep-2026-10-09/manifest.json)；[transitions.json](artifacts/bmc-unknown-sweep-2026-10-09/transitions.json)；[restored-tp.md](artifacts/bmc-unknown-sweep-2026-10-09/restored-tp.md)；[task-source-changes.patch](artifacts/bmc-unknown-sweep-2026-10-09/task-source-changes.patch)；[reference-probe](artifacts/bmc-unknown-sweep-2026-10-09/reference-probe/)；[witness-validation-fixed/summary.json](artifacts/bmc-unknown-sweep-2026-10-09/witness-validation-fixed/summary.json)；[witness-validation-prune-extra/summary.json](artifacts/bmc-unknown-sweep-2026-10-09/witness-validation-prune-extra/summary.json)；[counter](artifacts/bmc-unknown-sweep-2026-10-09/reference-probe/counter/)；[pruned-normal-suite.log](artifacts/bmc-unknown-sweep-2026-10-09/pruned-normal-suite.log)；[pruned-normal-summary.json](artifacts/bmc-unknown-sweep-2026-10-09/pruned-normal-summary.json)。
- <a id="record-dartagnan-svcomp2026-reference"></a>**Dartagnan SV-COMP 2026 权威参照（版本 + no-data-race 原理）**：[manifest.json](artifacts/bmc-reference-guided-2026-10-09/manifest.json)。
- <a id="record-ikos-vs-dartagnan-gap"></a>**IKOS vs Dartagnan 差距分析（no-data-race，SV-COMP 2026 官方参照）**：[dartagnan_official_tp_176.txt](artifacts/dartagnan_official_tp_176.txt)；[ikos_unique_real_19.txt](artifacts/ikos_unique_real_19.txt)。
- <a id="record-ikos-inline-ab-2026-10-09"></a>**mark_internal_inline 单独 A/B**：[summary.json](artifacts/ikos-inline-ab-2026-10-09/summary.json)。

### 值、地址与对象

- <a id="record-ikos-bmc-a1-address-2026-10-08"></a>**BMC A1 地址编码：实现与验证（2026-10-08）**：[bmc-a1-address-2026-10-08.json](artifacts/bmc-a1-address-2026-10-08.json)；[bmc-a1-address-2026-10-08.tsv](artifacts/bmc-a1-address-2026-10-08.tsv)。
- <a id="record-ikos-bmc-affine-guards-2026-10-09"></a>**动态仿射守卫：减少不可行循环路径**：[summary.json](artifacts/bmc-affine-guards-2026-10-09/summary.json)。
- <a id="record-ikos-bmc-startup-address-2026-10-09"></a>**启动前缀的逐次数组地址**：[summary.json](artifacts/bmc-startup-address-2026-10-09/summary.json)。
- <a id="record-ikos-bmc-pointer-returns-2026-10-09"></a>**实际 helper 返回的全局指针**：[summary.json](artifacts/bmc-pointer-returns-2026-10-09/summary.json)。
- <a id="record-ikos-bmc-global-choice-2026-10-10"></a>**有限全局 points-to 集合用于候选选择，实际路径证明对象**：[summary.json](artifacts/bmc-global-choice-2026-10-10/summary.json)；[global-choice-final-full-repeat-paired.json](artifacts/bmc-global-choice-2026-10-10/global-choice-final-full-repeat-paired.json)。
- <a id="record-ikos-bmc-main-heap-2026-10-10"></a>**实际初始化的不可变指针槽与单次main堆分配**：[paired.json](artifacts/bmc-main-heap-2026-10-10/paired.json)；[summary.json](artifacts/bmc-main-heap-2026-10-10/summary.json)；[confirmed-targets.json](artifacts/bmc-main-heap-2026-10-10/confirmed-targets.json)。
- <a id="record-ikos-bmc-byte-rf-2026-10-10"></a>**原始整数全局对象的字节级 SC 读源（2026-10-10）**：[before.json](artifacts/bmc-byte-rf-2026-10-10/before.json)；[fixtures.json](artifacts/bmc-byte-rf-2026-10-10/fixtures.json)；[reference.json](artifacts/bmc-byte-rf-2026-10-10/reference.json)；[selected-pairs-32.json](artifacts/bmc-byte-rf-2026-10-10/selected-pairs-32.json)；[selected-pairs-64.json](artifacts/bmc-byte-rf-2026-10-10/selected-pairs-64.json)；[paired.json](artifacts/bmc-byte-rf-2026-10-10/paired.json)；[summary.json](artifacts/bmc-byte-rf-2026-10-10/summary.json)；[partial-validation.json](artifacts/bmc-byte-rf-2026-10-10/partial-validation.json)；[conditional-replay-probes.json](artifacts/bmc-byte-rf-2026-10-10/conditional-replay-probes.json)；[conditional-replay.json](artifacts/bmc-byte-rf-2026-10-10/conditional-replay.json)。
- <a id="record-ikos-bmc-heap-scalar-read-2026-10-10"></a>**真实来源的堆整数读（2026-10-10）**：[paired.json](artifacts/bmc-heap-scalar-read-2026-10-10/paired.json)；[summary.json](artifacts/bmc-heap-scalar-read-2026-10-10/summary.json)；[fixtures.json](artifacts/bmc-heap-scalar-read-2026-10-10/fixtures.json)；[reference.json](artifacts/bmc-heap-scalar-read-2026-10-10/reference.json)；[partial-validation.json](artifacts/bmc-heap-scalar-read-2026-10-10/partial-validation.json)；[targeted.json](artifacts/bmc-heap-scalar-read-2026-10-10/targeted.json)。
- <a id="record-ikos-bmc-pointer-slot-casts-2026-10-10"></a>**BMC 已发布指针槽的纯地址 cast（2026-10-10）**：[summary.json](artifacts/bmc-pointer-slot-casts-2026-10-10/summary.json)；[paired.json](artifacts/bmc-pointer-slot-casts-2026-10-10/paired.json)；[fixtures.json](artifacts/bmc-pointer-slot-casts-2026-10-10/fixtures.json)；[heap.json](artifacts/bmc-pointer-slot-casts-2026-10-10/heap.json)；[targets.json](artifacts/bmc-pointer-slot-casts-2026-10-10/targets.json)；[reference-value-probes.json](artifacts/bmc-pointer-slot-casts-2026-10-10/reference-value-probes.json)；[selected-field.json](artifacts/bmc-pointer-slot-casts-2026-10-10/selected-field.json)。

### 线程与同步

- <a id="record-ikos-bmc-candidates-2026-10-08"></a>**BMC 阻塞诊断、多候选搜索与线程激活审查**：[bmc-candidates-2026-10-08.json](artifacts/bmc-candidates-2026-10-08.json)；[bmc-candidates-2026-10-08.tsv](artifacts/bmc-candidates-2026-10-08.tsv)。
- <a id="record-ikos-bmc-create-result-2026-10-09"></a>**成功 Create 的返回值与守卫**：[summary.json](artifacts/bmc-create-result-2026-10-09/summary.json)。
- <a id="record-ikos-bmc-named-atomic-2026-10-09"></a>**完整命名 atomic helper 前缀**：[summary.json](artifacts/bmc-named-atomic-2026-10-09/summary.json)。
- <a id="record-ikos-bmc-atomic-target-2026-10-09"></a>**atomic 内部目标、完成路径和预算保护**：[summary.json](artifacts/bmc-atomic-target-2026-10-09/summary.json)。
- <a id="record-ikos-bmc-thread-exit-2026-10-10"></a>**实际 pthread_exit 完成端点（2026-10-10）**：[paired.json](artifacts/bmc-thread-exit-2026-10-10/paired.json)；[summary.json](artifacts/bmc-thread-exit-2026-10-10/summary.json)；[models-partial.json](artifacts/bmc-thread-exit-2026-10-10/models-partial.json)。
- <a id="record-ikos-bmc-condition-prefix-2026-10-10"></a>**条件变量的有限虚假唤醒前缀**：[summary.json](artifacts/bmc-condition-2026-10-10/summary.json)；[paired.json](artifacts/bmc-condition-2026-10-10/paired.json)。
- <a id="record-ikos-bmc-static-condition-2026-10-10"></a>**静态条件变量初始化的ABI证据**：[paired.json](artifacts/bmc-static-condition-2026-10-10/paired.json)；[summary.json](artifacts/bmc-static-condition-2026-10-10/summary.json)；[fixtures.json](artifacts/bmc-static-condition-2026-10-10/fixtures.json)；[partial-models.json](artifacts/bmc-static-condition-2026-10-10/partial-models.json)。
- <a id="record-ikos-bmc-constructors-2026-10-10"></a>**构造函数遗漏导致的 BMC 假反例（2026-10-10）**：[before.json](artifacts/bmc-constructors-2026-10-10/before.json)；[fixtures.json](artifacts/bmc-constructors-2026-10-10/fixtures.json)；[reference.json](artifacts/bmc-constructors-2026-10-10/reference.json)；[paired.json](artifacts/bmc-constructors-2026-10-10/paired.json)；[summary.json](artifacts/bmc-constructors-2026-10-10/summary.json)。
- <a id="record-ikos-bmc-library-origin-2026-10-10"></a>**BMC 隐式库写入的值来源边界（2026-10-10）**：[abi.json](artifacts/bmc-library-origin-2026-10-10/abi.json)；[before.json](artifacts/bmc-library-origin-2026-10-10/before.json)；[fixtures.json](artifacts/bmc-library-origin-2026-10-10/fixtures.json)；[reference.json](artifacts/bmc-library-origin-2026-10-10/reference.json)；[paired.json](artifacts/bmc-library-origin-2026-10-10/paired.json)；[summary.json](artifacts/bmc-library-origin-2026-10-10/summary.json)；[first-paired.json](artifacts/bmc-library-origin-2026-10-10/first-paired.json)。
- <a id="record-ikos-bmc-sync-storage-width-2026-10-10"></a>**BMC 同步控制存储的完整宽度（2026-10-10）**：[before.json](artifacts/bmc-sync-storage-width-2026-10-10/before.json)；[fixtures.json](artifacts/bmc-sync-storage-width-2026-10-10/fixtures.json)；[p0.json](artifacts/bmc-sync-storage-width-2026-10-10/p0.json)；[abi.json](artifacts/bmc-sync-storage-width-2026-10-10/abi.json)；[paired.json](artifacts/bmc-sync-storage-width-2026-10-10/paired.json)；[summary.json](artifacts/bmc-sync-storage-width-2026-10-10/summary.json)。
- <a id="record-ikos-bmc-mutex-attributes-2026-10-10"></a>**BMC ERRORCHECK 属性与成功 owned-unlock 前缀（2026-10-10）**：[paired.json](artifacts/bmc-mutex-attributes-2026-10-10/paired.json)；[summary.json](artifacts/bmc-mutex-attributes-2026-10-10/summary.json)；[fixtures.json](artifacts/bmc-mutex-attributes-2026-10-10/fixtures.json)；[targets.json](artifacts/bmc-mutex-attributes-2026-10-10/targets.json)；[reference.json](artifacts/bmc-mutex-attributes-2026-10-10/reference.json)；[selected-models.json](artifacts/bmc-mutex-attributes-2026-10-10/selected-models.json)。
- <a id="record-ikos-bmc-lock-interference-2026-10-10"></a>**已释放锁分区写值的入口覆盖（2026-10-10）**：[paired.json](artifacts/bmc-lock-interference-2026-10-10/paired.json)；[summary.json](artifacts/bmc-lock-interference-2026-10-10/summary.json)；[run.py](review-repros/lock-interference/run.py)；[native.json](artifacts/bmc-lock-interference-2026-10-10/native.json)。
- <a id="record-ikos-bmc-shared-callback-feasibility-2026-10-10"></a>**共享回调值流的下一阶段依据（2026-10-10）**：[reference.json](artifacts/bmc-shared-callback-feasibility-2026-10-10/reference.json)；[probes.json](artifacts/bmc-shared-callback-feasibility-2026-10-10/probes.json)。

### 搜索、性能与撤回实验

- <a id="record-ikos-bmc-search-short-circuit-2026-10-09"></a>**竞争方向 SAT 后停止重复求解**：[summary.json](artifacts/bmc-search-short-circuit-2026-10-09/summary.json)。
- <a id="record-ikos-bmc-fresh-solver-2026-10-09"></a>**独立竞争方向查询：保留非增量预处理**：[summary.json](artifacts/bmc-fresh-solver-2026-10-09/summary.json)。
- <a id="record-ikos-bmc-short-prefix-2026-10-09"></a>**优先较短的实际前缀**：[summary.json](artifacts/bmc-short-prefix-2026-10-09/summary.json)；[paired.json](artifacts/bmc-short-prefix-2026-10-09/paired.json)；[schedule-calibration.json](artifacts/bmc-short-prefix-2026-10-09/schedule-calibration.json)；[schedule-selected.json](artifacts/bmc-short-prefix-2026-10-09/schedule-selected.json)。
- <a id="record-ikos-bmc-loop-search-2026-10-09"></a>**中间循环界限搜索实验：暂不落地**：[bmc-loop-deepening](review-repros/bmc-loop-deepening/)。
- <a id="record-ikos-bmc-pair-order-experiment-2026-10-10"></a>**总前缀长度排序实验：撤回**：[summary.json](artifacts/bmc-pair-order-experiment-2026-10-10/summary.json)。
- <a id="record-ikos-bmc-helper-search-experiment-2026-10-10"></a>**helper目标搜索实验：配对回退后撤回**：[paired.json](artifacts/bmc-helper-search-experiment-2026-10-10/paired.json)；[summary.json](artifacts/bmc-helper-search-experiment-2026-10-10/summary.json)。
- <a id="record-ikos-bmc-completed-enumeration-2026-10-10"></a>**枚举中断后的完整竞争前缀与分阶段搜索**：[summary.json](artifacts/bmc-completed-enumeration-2026-10-10/summary.json)；[paired.json](artifacts/bmc-completed-enumeration-2026-10-10/paired.json)；[run.py](review-repros/completed-enumeration/run.py)。
- <a id="record-ikos-bmc-small-bound-2026-10-10"></a>**暂缓候选组的小界限补充搜索**：[paired.json](artifacts/bmc-small-bound-2026-10-10/paired.json)；[summary.json](artifacts/bmc-small-bound-2026-10-10/summary.json)；[partial-models.json](artifacts/bmc-small-bound-2026-10-10/partial-models.json)；[fixture-reference.json](artifacts/bmc-small-bound-2026-10-10/fixture-reference.json)。
- <a id="record-ikos-bmc-atomic-interior-2026-10-10"></a>**提前判定atomic区间中间目标的查询UNSAT**：[paired.json](artifacts/bmc-atomic-interior-2026-10-10/paired.json)；[summary.json](artifacts/bmc-atomic-interior-2026-10-10/summary.json)；[lu-partial.json](artifacts/bmc-atomic-interior-2026-10-10/lu-partial.json)。
- <a id="record-ikos-bmc-sc-same-path-2026-10-10"></a>**同路径SC读源公式简化（2026-10-10）**：[paired.json](artifacts/bmc-sc-same-path-2026-10-10/paired.json)；[summary.json](artifacts/bmc-sc-same-path-2026-10-10/summary.json)；[reference-partial.json](artifacts/bmc-sc-same-path-2026-10-10/reference-partial.json)。
- <a id="record-ikos-bmc-constant-guard-2026-10-10"></a>**确定读值传播与不可行守卫（2026-10-10）**：[paired.json](artifacts/bmc-constant-guard-2026-10-10/paired.json)；[summary.json](artifacts/bmc-constant-guard-2026-10-10/summary.json)；[bench-native.json](artifacts/bmc-constant-guard-2026-10-10/bench-native.json)；[reference-partial.json](artifacts/bmc-constant-guard-2026-10-10/reference-partial.json)；[fib-32-partial.json](artifacts/bmc-constant-guard-2026-10-10/fib-32-partial.json)；[fib-64-partial.json](artifacts/bmc-constant-guard-2026-10-10/fib-64-partial.json)。
- <a id="record-ikos-bmc-guard-first-2026-10-10"></a>**守卫先于未使用算术的完整检查（2026-10-10）**：[paired.json](artifacts/bmc-guard-first-2026-10-10/paired.json)；[summary.json](artifacts/bmc-guard-first-2026-10-10/summary.json)；[reference-partial.json](artifacts/bmc-guard-first-2026-10-10/reference-partial.json)。
- <a id="record-ikos-bmc-atomic-delay-2026-10-10"></a>**延后不可行路径的atomic约束构造（2026-10-10）**：[paired.json](artifacts/bmc-atomic-delay-2026-10-10/paired.json)；[summary.json](artifacts/bmc-atomic-delay-2026-10-10/summary.json)；[fib-32-partial.json](artifacts/bmc-atomic-delay-2026-10-10/fib-32-partial.json)；[fib-64-partial.json](artifacts/bmc-atomic-delay-2026-10-10/fib-64-partial.json)。
- <a id="record-ikos-bmc-loop-hint-2026-10-10"></a>**依据实际循环守卫安排搜索（2026-10-10）**：[paired.json](artifacts/bmc-loop-hint-2026-10-10/paired.json)；[summary.json](artifacts/bmc-loop-hint-2026-10-10/summary.json)；[bench-native.json](artifacts/bmc-loop-hint-2026-10-10/bench-native.json)；[fib7-32-partial.json](artifacts/bmc-loop-hint-2026-10-10/fib7-32-partial.json)。
- <a id="record-ikos-bmc-guard-cache-2026-10-10"></a>**固定候选组内复用确定false守卫（2026-10-10）**：[paired.json](artifacts/bmc-guard-cache-2026-10-10/paired.json)；[summary.json](artifacts/bmc-guard-cache-2026-10-10/summary.json)。
- <a id="record-ikos-bmc-spawn-source-2026-10-10"></a>**创建前main写源的SC顺序简化（2026-10-10）**：[paired.json](artifacts/bmc-spawn-source-2026-10-10/paired.json)；[first-paired.json](artifacts/bmc-spawn-source-2026-10-10/first-paired.json)；[summary.json](artifacts/bmc-spawn-source-2026-10-10/summary.json)。
- <a id="record-ikos-bmc-join-query-prune-2026-10-10"></a>**提前删除当前 Join 查询必然 UNSAT 的前缀（2026-10-10）**：[paired.json](artifacts/bmc-join-query-prune-2026-10-10/paired.json)；[summary.json](artifacts/bmc-join-query-prune-2026-10-10/summary.json)；[fixtures.json](artifacts/bmc-join-query-prune-2026-10-10/fixtures.json)。
- <a id="record-ikos-bmc-activation-prefix-2026-10-10"></a>**完整创建前缀与分阶段查询（2026-10-10）**：[early-paired.json](artifacts/bmc-activation-prefix-2026-10-10/early-paired.json)；[paired.json](artifacts/bmc-activation-prefix-2026-10-10/paired.json)；[summary.json](artifacts/bmc-activation-prefix-2026-10-10/summary.json)；[reference.json](artifacts/bmc-activation-prefix-2026-10-10/reference.json)；[partial-validation.json](artifacts/bmc-activation-prefix-2026-10-10/partial-validation.json)；[ticket48-validation.json](artifacts/bmc-activation-prefix-2026-10-10/ticket48-validation.json)；[confirmed-pairs.json](artifacts/bmc-activation-prefix-2026-10-10/confirmed-pairs.json)；[targeted.json](artifacts/bmc-activation-prefix-2026-10-10/targeted.json)；[fixtures.json](artifacts/bmc-activation-prefix-2026-10-10/fixtures.json)。
- <a id="record-ikos-bmc-activation-query-order-2026-10-10"></a>**新激活候选的查询顺序（2026-10-10）**：[paired.json](artifacts/bmc-activation-query-order-2026-10-10/paired.json)；[summary.json](artifacts/bmc-activation-query-order-2026-10-10/summary.json)；[targeted.json](artifacts/bmc-activation-query-order-2026-10-10/targeted.json)；[lp64.json](artifacts/bmc-activation-query-order-2026-10-10/lp64.json)；[fixtures.json](artifacts/bmc-activation-query-order-2026-10-10/fixtures.json)；[fixture-reference.json](artifacts/bmc-activation-query-order-2026-10-10/fixture-reference.json)。
- <a id="record-ikos-bmc-stage-diagnostics-2026-10-10"></a>**保留暂缓候选的两轮搜索证据**：[summary.json](artifacts/bmc-stage-diagnostics-2026-10-10/summary.json)；[paired.json](artifacts/bmc-stage-diagnostics-2026-10-10/paired.json)。

### 反例导出与研究验证

- <a id="record-ikos-bmc-witness-validation-2026-10-09"></a>**本地 GraphML 验证与下一步优化（2026-10-09）**：[validate_bmc_witnesses.py](../scripts/validate_bmc_witnesses.py)；[test_bmc_witnesses.py](../scripts/test_bmc_witnesses.py)；[bmc-witness-validation-2026-10-09](artifacts/bmc-witness-validation-2026-10-09/)；[task-results.tsv](artifacts/bmc-witness-validation-2026-10-09/task-results.tsv)。
- <a id="record-ikos-bmc-replay-2026-10-09"></a>**BMC 结构化前缀与独立重放（2026-10-09）**：[export-summary.json](artifacts/bmc-replay-2026-10-09/export-summary.json)。
- <a id="record-ikos-dartagnan-strict-validation-2026-10-09"></a>**Dartagnan 指定候选约束接口：校准阶段**：[calibration.json](artifacts/dartagnan-strict-2026-10-09/calibration.json)；[ikos-simple-fixed-memory.json](artifacts/dartagnan-strict-2026-10-09/ikos-simple-fixed-memory.json)；[relative-address-calibration.json](artifacts/dartagnan-strict-2026-10-09/relative-address-calibration.json)；[fixed-memory-sweep.json](artifacts/dartagnan-strict-2026-10-09/fixed-memory-sweep.json)；[creation-occurrence-calibration.json](artifacts/dartagnan-strict-2026-10-09/creation-occurrence-calibration.json)；[creation-occurrence-real-candidates.json](artifacts/dartagnan-strict-2026-10-09/creation-occurrence-real-candidates.json)；[fixed-orientation-calibration.json](artifacts/dartagnan-strict-2026-10-09/fixed-orientation-calibration.json)；[fixed-orientation-evidence.json](artifacts/dartagnan-strict-2026-10-09/fixed-orientation-evidence.json)。
- <a id="record-ikos-bmc-tp77-validation-2026-10-09"></a>**当前77项标签TP的验证覆盖**：[export-summary.json](artifacts/bmc-tp77-validation-2026-10-09/export-summary.json)；[initial-fixed-memory.json](artifacts/bmc-tp77-validation-2026-10-09/initial-fixed-memory.json)；[selected-retries.json](artifacts/bmc-tp77-validation-2026-10-09/selected-retries.json)；[memory-calibration.json](artifacts/bmc-tp77-validation-2026-10-09/memory-calibration.json)；[nondet-calibration.json](artifacts/bmc-tp77-validation-2026-10-09/nondet-calibration.json)；[nondet-selected.json](artifacts/bmc-tp77-validation-2026-10-09/nondet-selected.json)；[nondet-fixed-memory.json](artifacts/bmc-tp77-validation-2026-10-09/nondet-fixed-memory.json)；[nondet-bound-retries.json](artifacts/bmc-tp77-validation-2026-10-09/nondet-bound-retries.json)；[communication-calibration.json](artifacts/bmc-tp77-validation-2026-10-09/communication-calibration.json)；[communication-selected.json](artifacts/bmc-tp77-validation-2026-10-09/communication-selected.json)；[communication-fixed-memory.json](artifacts/bmc-tp77-validation-2026-10-09/communication-fixed-memory.json)；[communication-fmax-bound6.json](artifacts/bmc-tp77-validation-2026-10-09/communication-fmax-bound6.json)；[loop-exit-calibration.json](artifacts/bmc-tp77-validation-2026-10-09/loop-exit-calibration.json)；[communication-loop-retries.json](artifacts/bmc-tp77-validation-2026-10-09/communication-loop-retries.json)。
- <a id="record-ikos-bmc-schedule-validation-2026-10-10"></a>**TP77 已映射内存顺序的检查结果**：[fixed-memory.json](artifacts/bmc-schedule-validation-2026-10-10/fixed-memory.json)；[export-summary.json](artifacts/bmc-schedule-validation-2026-10-10/export-summary.json)；[fmax-bound6.json](artifacts/bmc-schedule-validation-2026-10-10/fmax-bound6.json)。
- <a id="record-ikos-bmc-initial-rf-2026-10-10"></a>**严格检查中的隐式初始写来源**：[initial-calibration.json](artifacts/bmc-initial-rf-2026-10-10/initial-calibration.json)；[full-calibration.json](artifacts/bmc-initial-rf-2026-10-10/full-calibration.json)；[simple.json](artifacts/bmc-initial-rf-2026-10-10/simple.json)；[unverif.json](artifacts/bmc-initial-rf-2026-10-10/unverif.json)；[lu.json](artifacts/bmc-initial-rf-2026-10-10/lu.json)。
- <a id="record-ikos-bmc-indirect-address-validation-2026-10-10"></a>**间接访问的严格参考地址约束**：[32.json](artifacts/bmc-indirect-address-2026-10-10/32.json)；[64.json](artifacts/bmc-indirect-address-2026-10-10/64.json)；[test_indirect_addresses.py](../scripts/dartagnan_strict/test_indirect_addresses.py)。
- <a id="record-ikos-bmc-heap-binding-2026-10-10"></a>**严格参考约束中的实际堆分配绑定**：[summary.json](artifacts/bmc-heap-binding-2026-10-10/summary.json)；[fixtures.json](artifacts/bmc-heap-binding-2026-10-10/fixtures.json)；[benchmark38.json](artifacts/bmc-heap-binding-2026-10-10/benchmark38.json)。
- <a id="record-ikos-bmc-typed-reference-2026-10-10"></a>**严格参考映射中的实际内存类型（2026-10-10）**：[calibration.json](artifacts/bmc-typed-reference-2026-10-10/calibration.json)；[heap-calibration.json](artifacts/bmc-typed-reference-2026-10-10/heap-calibration.json)；[37-32-partial.json](artifacts/bmc-typed-reference-2026-10-10/37-32-partial.json)；[37-64-partial.json](artifacts/bmc-typed-reference-2026-10-10/37-64-partial.json)。
- <a id="record-ikos-bmc-creation-sequence-validation-2026-10-10"></a>**跨分支的真实创建实例映射（2026-10-10）**：[summary.json](artifacts/bmc-creation-sequence-validation-2026-10-10/summary.json)；[old-incompatible-mapping.json](artifacts/bmc-creation-sequence-validation-2026-10-10/old-incompatible-mapping.json)；[selected-models.json](artifacts/bmc-creation-sequence-validation-2026-10-10/selected-models.json)；[compatibility.json](artifacts/bmc-creation-sequence-validation-2026-10-10/compatibility.json)；[conditional-replay.json](artifacts/bmc-creation-sequence-validation-2026-10-10/conditional-replay.json)。

### 逐任务历史台账

- <a id="record-ikos-bmc-current-ledger-2026-10-09"></a>**当前 UNKNOWN 台账与历史证据衔接**：[results.json](artifacts/bmc-current-ledger-2026-10-09/results.json)；[summary.json](artifacts/bmc-current-ledger-2026-10-09/summary.json)。
- <a id="record-ikos-bmc-post-atomic-ledger-2026-10-09"></a>**TP77基线后的完整UNKNOWN台账**：[results.json](artifacts/bmc-post-atomic-ledger-2026-10-09/results.json)；[summary.json](artifacts/bmc-post-atomic-ledger-2026-10-09/summary.json)。
- <a id="record-ikos-bmc-tp80-ledger-2026-10-10"></a>**TP80基线后的UNKNOWN台账更新**：[results.json](artifacts/bmc-tp80-ledger-2026-10-10/results.json)；[summary.json](artifacts/bmc-tp80-ledger-2026-10-10/summary.json)。
- <a id="record-ikos-bmc-tp82-ledger-2026-10-10"></a>**TP82基线后的UNKNOWN台账**：[results.json](artifacts/bmc-tp82-ledger-2026-10-10/results.json)；[summary.json](artifacts/bmc-tp82-ledger-2026-10-10/summary.json)。
- <a id="record-ikos-bmc-tp90-ledger-2026-10-10"></a>**TP90之后的逐任务台账（2026-10-10）**：[results.json](artifacts/bmc-tp90-ledger-2026-10-10/results.json)；[summary.json](artifacts/bmc-tp90-ledger-2026-10-10/summary.json)。
- <a id="record-ikos-bmc-tp98-ledger-2026-10-10"></a>**TP98之后的逐任务台账（2026-10-10）**：[results.json](artifacts/bmc-tp98-ledger-2026-10-10/results.json)；[summary.json](artifacts/bmc-tp98-ledger-2026-10-10/summary.json)。
- <a id="record-ikos-bmc-tp104-ledger-2026-10-10"></a>**TP104 后的逐任务台账（2026-10-10）**：[results.json](artifacts/bmc-tp104-ledger-2026-10-10/results.json)；[summary.json](artifacts/bmc-tp104-ledger-2026-10-10/summary.json)。

<a id="references"></a>
## 15. 文献、规范与固定源码

原始依据按具体设计决策阅读，不把论文性能数字当本项目测量；相关章节阅读也不等于移植完整算法。

| 依据 | 对IKOS的用途与阅读范围 |
| --- | --- |
| Biere et al., *Symbolic Model Checking without BDDs*, TACAS1999，[DOI](https://doi.org/10.1007/3-540-49059-0_14) | BMC转移展开和有界UNSAT范围；审查时未阅读全文 |
| Ponce de León et al., *BMC with Memory Models as Modules*, FMCAD2018，[论文](https://hernanponcedeleon.github.io/pdfs/fmcad2018.pdf) | 已读相关建模章节：控制、数据、内存模型、性质四部分；不能只迁移竞争公式 |
| Gavrilenko et al., *BMC for Weak Memory Models: Relation Analysis for Compact SMT Encodings*, CAV2019，[论文](https://hernanponcedeleon.github.io/pdfs/cav2019.pdf) | 已读may/active关系候选压缩；指导IKOS候选/依赖裁剪，未移植完整关系分析 |
| Haas et al., *CAAT: Consistency as a Theory*, OOPSLA2022，[论文](https://hernanponcedeleon.github.io/pdfs/oopsla2022.pdf) | 已读相关章节：惰性一致性与执行编码分离，后期研究方向，不立即搬入整个CAAT |
| *Static Analysis of Memory Models for SMT Encodings*, OOPSLA2023 | 条件化may/must与外部filters；版本/URL/hash/实际阅读范围见精度恢复证据目录references/manifest.json |
| Alglave et al., *Partial Orders for Efficient Bounded Model Checking of Concurrent Software*, CAV2013，[DOI](https://doi.org/10.1007/978-3-642-39799-8_9) | 部分序调度与执行证据；题录核对，审查时未阅读全文 |
| Ponce de León et al., *Dartagnan: SMT-based Violation Witness Validation*, TACAS2022，[论文](https://hernanponcedeleon.github.io/pdfs/svcomp2022.pdf) | witness表达执行与独立验证；题录核对，审查时未阅读全文 |
| Miné, ESOP2012 / VMCAI2014并发抽象解释论文 | 干扰覆盖与关系线程模块化，SAFE及AI/BMC契约的后续阅读，不由BMC论文代替 |
| Vojdani et al., *Static Race Detection for Device Drivers: The Goblint Approach*, ASE2016 | 私有化、发布、锁保护；题录/算法阅读建议，完整PBR未移植 |

规范：
[SV-COMP2027规则](https://sv-comp.sosy-lab.org/2027/rules.php)、
[POSIX mutex_init](https://pubs.opengroup.org/onlinepubs/9799919799/functions/pthread_mutex_init.html)、
[mutex lock/unlock](https://pubs.opengroup.org/onlinepubs/9799919799/functions/pthread_mutex_lock.html)、
[mutexattr_settype](https://pubs.opengroup.org/onlinepubs/9799919799/functions/pthread_mutexattr_settype.html)、
[cond_wait](https://pubs.opengroup.org/onlinepubs/9799919799/functions/pthread_cond_wait.html)。
平台私有字段依据本地 glibc 头文件/库 hash 与 native 断言，不冒称POSIX可移植布局。

源码参照：

- [固定Dat3M源码](https://github.com/hernanponcedeleon/Dat3M/tree/7caef5c850dc2dbf81c5554dcf41e402007f7a19)：
  ProgramEncoder、PropertyEncoder、ExpressionEncoder、WmmEncoder、LoopUnrolling、ThreadCreation、
  Inlining、Intrinsics、NaiveDevirtualisation、WitnessGraph、Alloc/Init及Acyclicity。
  实际launcher和library JAR都需核对内嵌git.properties/hash，不能只读旁边target/classes。
- Goblint本地 `/home/ruan/goblint-analyzer`，提交`bb55b70d91290992629ba3d073185c10cdf356e6`：
  interval/overflow和`basePriv.ml::ProtectionBasedPriv.read_global/unlock`。
  它的保护读端与证明依赖不能省略后宣称已经迁移完整算法。
- IKOS上游 `/home/ruan/ikos-original`：核对真实revision后对照前端、AR、域和执行机制，
  不修改上游来适配本fork的结论。

历史Dat3M提交线索包括join/detach `664d81f7f/0e1e73128/ab0610069`、跨线程SCCP
`55590d7c7`、rf类型转换`95224e40c`、指针修复`ee5a58ae9`、devirtualization
`24a65b237`、混合字长`e45ac070e`、spinloop/bound相关`e8622b751/d8e6066d6/7950dd782/81da44f69`、
witness `5fc2bcb78`。这些是源码研究入口，不是各机制的当前IKOS缺口证明或收益预测。
