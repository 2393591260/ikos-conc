# 本地 GraphML 验证与下一步优化（2026-10-09）

## 1. 结论和证据边界

已接通 CPAchecker 4.2.2、Dartagnan 4.3.1 的本地 GraphML 数据竞争验证。
35 个当前 TP 的 44 组已保存访问对全部参与验证；输入源码 SHA256 全部未变，
build/install analyzer SHA256 与产生这些反例的精度基线一致。

**35 项都得到至少一个外部工具的竞争结论支持，但不能称为 35 项原始 SMT 模型均被独立证明。**
34 项有工具接受了 GraphML 源码投影；剩余函数指针任务得到 Dartagnan 独立分析的竞争结果。
这是有限实测、跨工具印证和见证适配结果，不能证明整个 checker 的 SAFE 方向 soundness。

两个旧 validator 都接受了故意错误的线程 ID；Dartagnan 还接受了不存在的目标行、
以及带有另一处真实竞争的程序中不冲突的目标位置。因此旧版 ACCEPT 不能充当严格线程实例/
候选对验证，更不能充当机器值、rf/co 和原始交错的执行重放。

本轮没有修改 analyzer、正式 YAML 2.2 导出的行为、BenchExec adapter 或已封装 ZIP。
没有重跑 1029 项全量；TP=35/FP=0/FN=0 等数值引用上一轮同一二进制的基线，
不是本轮重新测出的分数。

## 2. 实现与复现

- [本地适配脚本](../scripts/validate_bmc_witnesses.py)：读取已确认访问对的保存记录，
  检查源码 hash 和架构，导出 GraphML，调用两个本地 validator，保存命令、日志、退出码和分类。
- [自检和校准程序生成](../scripts/test_bmc_witnesses.py)：检查来源 hash、同入口实例、
  XML、AR 表达式源码投影和返回结果分类，并生成正反对照。
- 所有材料在 [证据目录](artifacts/bmc-witness-validation-2026-10-09/)，
  逐任务结果见 [task-results.tsv](artifacts/bmc-witness-validation-2026-10-09/task-results.tsv)。

脚本默认源路径/源行投影模式 `prefix`；同一路径连续的同源行 AR 步骤先合并，再按该组最后
一个 AR 时钟排序。否则跨线程交错会把一条 C 表达式强行变成两次源码执行要求。
这种合并是源码抽象，不是对 AR 调度的等价变换；列号、表达式内访问身份可能丢失。

`targets` 模式只声明创建和两处访问位置，用于判断前缀投影失败是否同时意味着目标不可验证。
不能把从 prefix 降级到 targets 的通过写成更严格的证据。
`--independent` 不给 validator 传见证，只做程序竞争结论的交叉验证。

两种 GraphML 模式均未绑定 SMT 的具体值与读来源，也未重放单独的 startup 前缀，
只声明保存记录中的创建点；未选中的线程由 validator 自己探索。
不能由此推导某个具体原模型被重放。
目前只支持保存记录足以区分的创建实例；嵌套创建、缺失/歧义实例回报导出错误，
不猜一个线程 ID。同入口的两个实例要求保存的创建列表中恰好有两个对应实例。

复现到**新的输出目录**：

```bash
python3 scripts/test_bmc_witnesses.py
python3 scripts/test_bmc_witnesses.py --controls /tmp/ikos-witness-controls-new
python3 scripts/validate_bmc_witnesses.py \
  --pairs /tmp/ikos-witness-controls-new/pairs.json --mode targets \
  --output /tmp/ikos-witness-controls-results-new --jobs 3 --timeout 30
python3 scripts/test_bmc_witnesses.py \
  --check-results /tmp/ikos-witness-controls-results-new/results.json

python3 scripts/validate_bmc_witnesses.py \
  --pairs docs/artifacts/bmc-precision-2026-10-09/confirmed-pairs.json \
  --output /tmp/ikos-witness-tp-results-new --jobs 3 --timeout 60
```

CPAchecker 必须同时加载 `config/specification/datarace.spc` 和 GraphML 自动机。
只用 `--spec witness.graphml` 会覆盖配置里的数据竞争 specification；
DataRaceState 只提供 `checkProperty("data-race")` 查询，不直接实现 target state。
调试时这种错误命令甚至拒绝 CPAchecker 自己生成的正见证，不能解释成 IKOS 的错误。
正式脚本已保留两者，并用正负例校准。CPP/CFLAGS 按 ILP32/LP64 传入 `-m32`/`-m64`。

运行将 `ACCEPT / REJECT / UNKNOWN / TIMEOUT / ERROR` 分开保存。
REJECT 表示所给见证/配置未验证成功，不自动归为 IKOS 的 FP；
UNKNOWN 日志里的不支持、资源不足也不能当作 SAFE。
validator 内部生成的大量 CFA/ARG/HTML 已完整归档到
`validator-internals.tar.gz` 和 `final-validator-internals.tar.gz`，
原命令、GraphML、结果及日志仍可直接阅读。

## 3. 正反校准（ILP32、LP64）

以下每格在两个架构上结果相同，共 24 条工具观测：

| 校准情形 | CPAchecker | Dartagnan | 含义 |
|---|---|---|---|
| 两个线程无锁写同一全局 | ACCEPT | ACCEPT | 正例管线可工作 |
| 两个线程使用同一 mutex | REJECT | REJECT | 不把受保护访问确认成竞争 |
| 历史 exclusive-flag 不可同时满足的守卫 | REJECT | REJECT | 值条件正反对照通过 |
| 真竞争程序，但两处目标行改成 999999 | REJECT | ACCEPT | Dartagnan 不强制目标存在 |
| 真竞争程序，但交换目标线程 ID | ACCEPT | ACCEPT | 两个旧版本均不能严格核验实例绑定 |
| 程序在 x 上竞争，但目标写的是不同对象 y/z | REJECT | ACCEPT | Dartagnan 的通过未绑定所给竞争对 |

另跑了 IKOS 的 race/locked/exclusive 三个程序，各两个架构，共 6 次。
只在 race 生成 confirmed race；locked/exclusive 未确认。
负例见证是校准时故意构造的错误声明，不是 IKOS 真的报告这些程序为竞争。
最终脚本版本再次运行这三种对照的两个架构，共 12 条 validator 观测：
每个工具 2 ACCEPT、4 REJECT，与预期一致。

实现依据：固定 Dartagnan 提交
`7caef5c850dc2dbf81c5554dcf41e402007f7a19` 的
`WitnessGraph.getEventsFromEdge/encode` 按**行号**收集所有线程的内存事件，
没有按 threadId 匹配，找不到行对应事件时不加入相应顺序约束。
在 CPAchecker 4.2.2 的 `AutomatonGraphmlParser` 中，threadId 被保存在 witness 自动机
变量/作用域信息中；本轮反例明确显示它没有强制目标步属于声明的实际线程实例。
源码副本和二进制 hashes 位于证据目录 `references/` 和 `manifest.json`。
没有修改外部 validator 来制造通过。

## 4. 当前 TP 的验证结果

44 组访问对，全部 ILP32；每组各运行两个 validator，超时设置 60 秒，外层进程限额额外留 10 秒。

| 最终源码前缀投影 | ACCEPT | REJECT | UNKNOWN | TIMEOUT | ERROR |
|---|---:|---:|---:|---:|---:|
| CPAchecker 4.2.2 | 20 | 6 | 17 | 1 | 0 |
| Dartagnan 4.3.1 | 41 | 1 | 0 | 2 | 0 |

初版投影也保存了 44 组结果，修正 AR 表达式合并后复测；总计数未改善。
不能把投影修正写成 TP 或验证通过率收益。

对 7 项争议任务做 targets 回退：CPAchecker 5 ACCEPT、1 UNKNOWN、1 REJECT；
Dartagnan 6 ACCEPT、1 REJECT。CPAchecker 共在 19 项任务上接受了至少一种源码投影，
Dartagnan 共在 32 项上接受投影；两者联合覆盖 34 项。
这两组 task 数不能相加，也不能换算成严格模型验证数。

重点解释：

- `04-mutex_06-ps_rc`、`04-mutex_16-ps_add1_rc`、
  `05-lval_ls_03-fld_rc`、`05-lval_ls_11-fldsense_rc`、`qw2004-1`：
  CPAchecker 拒绝前缀投影，但接受目标位置回退。
  说明源码前缀表达仍有障碍，不据此判定 IKOS FP。
- `qw2004-2`：CPAchecker 两种投影均拒绝，但不带见证独立分析能找到竞争；
  Dartagnan 接受两种投影且独立分析找到竞争。原 SMT 路径仍未独立重放。
- `04-mutex_50-funptr_rc`：CPAchecker 不支持指针分析，Dartagnan 拒绝两种 GraphML
  投影，但不带见证时找到竞争。这解释了第 35 项的外部结论支持，不能解释为模型已验证。
- `fkp2013-1/2`：Dartagnan 在见证和独立分析两种方式都超时；CPAchecker 接受前缀投影，
  独立分析也找到竞争。不是 IKOS 错误或“不能验证”的数学结论。
- `scull`：CPAchecker 接受 4 对中的 3 对，另一对超时；Dartagnan 接受全部 4 对。
- CPAchecker 的 17 个 UNKNOWN 包含指针分析不支持、指针锁参数不支持、rwlock 不支持。
  日志有具体 `Error: ... unsupported ...`；不是同一类 IKOS 语义缺陷。

最后四项额外独立分析共 8 条工具观察：CPAchecker 3 个竞争、1 UNKNOWN；
Dartagnan 2 个竞争、2 TIMEOUT。

**目前没有新增被证明的 IKOS FP，但存在尚未独立验证的模型，不能据此宣称已排除所有正确性问题。**
源码投影拒绝、外部工具不支持、超时和原编码不正确必须保持不同的诊断类别。

## 5. 下一步方案：先证据，再扩展精度

### P0：统一结构化反例、严格验证与源程序契约

当前模型在 diagnostics 模式里保存，实际正式 2.2 exporter 主要消费创建点/访问位置。
应在 `encode.hpp` 的 SAT 出口建立共用反例记录，由 checker 与 GraphML/YAML exporter 消费，
不再把 diagnostics 的 AR 字符串当作长期交换接口。

至少需要动态线程/创建实例、调用/循环实例、执行步 ID、对象与偏移/宽度、读写值、
rf 来源、同步事件、守卫分支及完整创建依赖；源位置保留宏/预处理映射及同一表达式内访问身份。
不能只给函数名配 thread_id，也不能把 abstract memory location 当成具体堆对象。
单独 startup 的步骤当前只有 `startup_N` 标记；这些标记不是已经求出的全局时钟值。

实施顺序：先覆盖目前受支持的全局整数/直线创建/两线程 SC 子集，
为反例记录的分支、值、读来源和实例绑定建立一致性检查；
再用固定的支持并发 2.2 的现代 CPAchecker 做严格源码验证。
必须再次运行错误线程、错误来源/值、错误地址与不冲突目标校准，确认真的约束了这些内容。
现代源码 pin `bd91e2fd933ac2704dc240089774db4e5ec877da` 是已有实现参照，
并不代表本轮已构建运行它或它已成为正式比赛配置。

对源码层 validator 无法描述的细粒度 AR 交错，可增加独立的受限 AR 执行重放器，
按具体对象、机器整数、rf 和同步历史重算步骤，避免只再求解同一套 SMT 公式。
它只能验证 AR 子集，不能替代 LLVM/AR 与 C 源语义保持性的检查。
同时继续检查 poison/undef、内存有效性、整数 UB 和库契约；在首次性质违例之后的
行为不应被误当作之前可达性的证明。

这是可实施的工程方案，不需要用户提供额外代码。
完整 C/LLVM/AR 语义保持与所有 SAFE 过滤规则的普遍证明，不能由本轮有限验证替代；
需要明确契约、规则证明及独立同行复核。

### P1：扩大真实创建实例及候选依赖闭包

2026 冻结基线 200 个 racy UNKNOWN 中，131 项观察到 `thread_activation_unproven`。
这不是“可以恢复 131 TP”的预测：另有对象、同步和取值门重叠。

可实施方案：沿实际 CFG 和调用栈构造有界创建前缀，给每次 create 分配动态实例，
把实参、创建返回值及 join 句柄绑到这些实例；循环按有界迭代状态展开。
IKOS 的线程/points-to/区间事实用于筛选潜在创建位置和依赖，
**may-create 不能替代实例存在性证明**。未覆盖的必要上下文继续 UNKNOWN。
首先选多候选和 helper/条件创建的最小正反例，随后固定 2026 集 A/B。

共享读需要第三线程、main 创建后的写或循环迭代时，
用可靠 may-alias 收集可能供值/同步的事件，再构造所需的 K 线程有界闭包。
所有 demanded read 在共同 SC 顺序中绑定合法写，不允许让被遗漏依赖变成自由值。
“两线程前缀够不够”必须有具体依赖证据，不能只凭目标对有两个线程。

### P1/P2：对象身份、宽度值流和同步摘要

- 45 项 racy UNKNOWN 观察到 `non_global_object`：先支持真实 main 前缀中的单次 allocation，
  建立动态对象 ID、发布、生命周期、参数/全局指针传递的确定来源；
  再扩大多次分配/循环，不能直接把同一个 heap abstract cell 绑定成同一对象。
- 变量地址和部分重叠供值：复用 IKOS DataLayout/宽度/offset 和 may-alias，
  明确字节序、初始化字节与范围重叠，以具体字节读写建模；
  正反例必须包含不同宽度、错位、别名、不重叠及越界。
- 63 项 racy UNKNOWN 观察到 `unmodeled_sync`：按 API 契约扩大 trylock、cond/barrier、join、
  原子操作等摘要。先验证成功/失败路径及具体对象身份；
  不能用忽略同步或“锁名相同”的办法恢复分数。
  删除整个程序的未建模同步门之前，必须证明被排除事件与候选依赖无关或给出合法摘要。
- C11 原子/弱内存扩展独立处理；目前受限 SC 实现不能直接继承 Dartagnan 的所有内存模型结论。

### P2：IKOS 与 BMC 的实际协同，而非复制 BMC

`CandidateFacts`、AR initializer 和 DataLayout 已经被消费；前次基线的
`ai_candidate_bounds` 为 49 项/256 次，但 no-wrap 证书全量消费为 0。
这些计数证明信息流存在，不证明净收益。

下一步把 may-alias/offset/线程上下文用于闭包范围与候选优先级，
把上下文明确的 must 同步/局部关系证书用于公式简化；
做同配置逐条启停事实的消融，测 TP、UNKNOWN、耗时及求解规模。
共享读数值区间先修复读时 interference 和跨线程不动点契约，
不能把曾观察到的 stale `[0,0]` 直接限定成 BMC 的具体读值。

架构收益需要来自“抽象解释缩小待编码集合/提供可检查证书”，
同时保持真正的具体值和执行来源，不是将 may 结论当成具体事实。

## 6. 需要用户提供什么

本地兼容验证、结构化反例和上述受限语义扩展可以继续自行推进，当前没有必须由用户
补充源码/安装工具才能解决的阻塞。

未来收到官方通知时，请提供 **2027 validator 的固定版本和实际参数/BenchExec XML**，
以及冻结任务集提交号/类别配置；这用于正式验收和固定目标集，不阻塞目前 2026 回归。
这两个信息来自组织者的选择，不能从旧预运行页面或 schema CI 通过推断。

如果需要对普遍 soundness 作学术声明，帮助应是并发语义/抽象解释方向的独立同行审阅
及明确的性质与内存模型契约，而不是继续补一批 TP 标签。
