# IKOS-ConC 抽象解释：正确性修复与精度/性能优化计划

日期：2026-10-10。设计基线：`b3e6706`。**方案已批准，按独立机制实施；进度见 §9。**
事实依据及复现输入见[正确性审查](ikos-ai-soundness-audit-2026-10-10.md)。
审查启动时保存的冻结集基线是 `TP106/TN696/UNKNOWN227/FP0/FN0/error0`，
但集合外已发现六类 FN，必须先修 SAFE 的前提，再讨论 TN 增长与加速。

建议路线：**干扰/访问覆盖可靠 → 保护与对象摘要精化 → 选择性关系与收敛优化 → 有证据的 AI/BMC 双向反馈**。
不同阶段单独提交、单独 A/B。没有依据预先承诺能恢复多少 TN 或能达到多大加速比。

## 1. 总体架构约束

当前 IKOS 已有值域、内存域、锁域、跨线程黑板、线程模块化循环和 checker。
应在这些所有权层修正事实来源，不在 checker 堆叠 benchmark 补丁。

| 信息 | 应归属的层 | 必须保持的契约 |
| --- | --- | --- |
| may 值/地址/逃逸/创建目标 | execution engine + abstract domains | 覆盖所有相关具体状态和环境效果 |
| MUST 持锁/完成/私有 | lock/thread/escape abstraction | 每条相关具体执行都成立；未知时撤销证明 |
| 跨线程效果 | concurrent environment | 覆盖所有线程/上下文/路径，任何语义扩张使依赖失效 |
| SAFE 充分条件 | property checker | 使用前提明确的证书；不能根据摘要缺失推出安全 |
| 有限真实竞争 | BMC + 独立执行检查 | 可执行前缀；仍以[complete](ikos-bmc-complete.md)契约为准 |

优先完善现有接口；只有确有多个使用者时才抽公共组件。
不要先建设未使用的通用并发框架，但线程身份/对象可达性也不应硬编码成 race 文件名规则。
未来 deadlock/reachability 可以复用线程和内存语义，性质判定仍需各自契约。

## 2. P0：先消除已经复现的六类漏报

### 2.1 候选/线程证明的直接收紧：F1、F3、F5

**共同锁（F1）**：删除无法证明完整 mutex 相等的 self-consistency shortcut，
复用已有真实共同锁判定。flat key 至少包含对象身份、字段 offset、实例与锁模式；
symbolic tuple 不得只比较 index 或哈希。
若希望保留“同数据 ⇒ 同保护锁”的推理，必须明确数据到锁的唯一映射，不能从同对象推出。

**未知线程入口（F3）**：有限 may targets 全部入 worklist；覆盖不了时保留 property-analysis-incomplete 状态。
报告层必须将此状态与“无候选”区分，最终 UNKNOWN。
尝试解析更多 function pointer 是精度工作；先有保守 incomplete fallback 才能正确扩展。
未知线程可能再创建线程或影响同步，单次数值 havoc 不足。

**join 身份（F5）**：短期撤销区间上界生成 MUST joined 的规则；
单个确定目标也须满足动态实例及成功 join 前提。
随后引入有限带来源 HandleSet：创建实例、pthread_self 来源、失效/未知状态分开。
多个可能目标只产生共同保证的 MUST 完成信息；循环实例合并时更保守。
有初始化 0 的 conditional create 可精化，但要有独立 provenance，不能凭区间形状恢复旧捷径。

每项保留主反例、有效/无效身份对照、字段重排/改名/helper 抽取变体。
预期效果是反例不再 SAFE；若 BMC 暂不支持，可以 UNKNOWN，不能为了立即得到 FALSE 放松证据。

### 2.2 干扰正确性：F6 与 F4 分清两种问题

**写效果（F6）**：无锁黑板发布必须 join 所有可能写。
批量发布可减少迭代开销，但批量容器中不能因为“最后访问”丢掉别的线程/路径。

**共享读（F4）**：每个相关读点都要考虑允许干扰，不能只在入口加入一次。
第一步实现覆盖 local read 与 interference 的保守读语义；
没有可靠 byte/type/alias 证据时，可以 havoc 相关结果或内存 cell。
不要重新引入旧的 stale `int_set` 覆盖，也不要把非共享局部变量全部 havoc。

关系域尤其要注意：若外部可改写 x，原来 `x=y` 的关系通常不能保留并与新 x 区间取交。
可行的保守起点是忘掉被环境改写变量的相关关系，再加入覆盖全部允许值的约束；
更精确的环境转移/关系摘要留到后续阶段。

两个机制要分别消融：

- F6 的 reader 无本地写，检查多线程写 join 是否生效；函数改名不能导致漏报。
- F4 的 reader 有本地写，检查后续读是否仍覆盖新干扰。
- 保留旧 fib 循环退出等反例，确保没有恢复原来的错误收窄。

实现应允许 **局部一条执行路径的强更新**，同时对 **多条路径/多个线程效果做 join**。
不能用“全部写永远 join”取代所有局部语义，也不能用“最后值”取代全局行为集合。

### 2.3 逃逸闭包：F2

建立保守 may 可达关系，至少覆盖：全局→heap、heap 字段→heap/stack、线程参数→子图、
TLS/stack 地址发布、共享对象中新插入指针、未知调用的可达写与发布。
现有 heap pointer board 可用于导航，但其 allocation-site/offset key 不是完整动态堆证明。

发布 root 时撤销其 may-reachable 节点的 MUST fresh；
若边为 TOP 或不能枚举，不能把空列表解释成没有子节点，应退到较大共享集合。
循环 malloc/多实例抽象地址的 private 证明需单独规定。

保守短期版本可以停用前提不可证的 fresh 过滤，随后用图闭包恢复。
加入深度 2/3、环、先连边后发布/先发布后连边、指针经 helper/phi/数组转移等变体。
其 soundness 前提是 **may 可达图足够完整**，不是“遍历现有已知边就自然正确”。

### 2.4 耦合不动点契约：已发现但尚未复现的风险

这是 P0 后半的审计任务，不能根据有限 FN 归零就跳过。

1. 所有语义摘要都有明确偏序和版本。值/地址范围扩张、points-to 转 TOP、offset 变化、
   创建目标增加、MUST 信息撤销都使依赖重算；不能只观察集合 size。
2. 全局/heap pointer 的 init、局部可见写和干扰来源完整，load restore 不错误覆盖来源。
3. 检查阶段消费稳定快照，或者在检查产生摘要变化时重新收敛、废弃旧访问记录。
4. 验证 coupled postsolution：全部线程转移与环境汇总应用后，没有新增 may 行为或未经成立的 MUST 事实。
5. 确认未知 extern/API 失败/配置不支持会退 UNKNOWN，不被空报告吞掉。

这不要求一开始引入复杂缓存系统。先修 dirty 的语义完整性及最终状态检测，
稳定后才做依赖工作队列与增量摘要。

**P0 阶段出口**：六类反例和各变体不再误报 SAFE；历史 FN 回归保持；
新转移/过滤有前提、失败回退和近似方向；全量变化逐项说明。
这是可审查的工程出口，仍不能替代完整语言和全项目 soundness 证明。

## 3. P1：用实际保护集实现精确 privatization

### 3.1 论文到源码的依据

Miné LMCS 2012 §4.4.1 的 unlock 终值属于 well-synchronized communication；
Goblint SAS 2021 §4.1–4.4 则区分 PBR、lock-centered、write-centered 及其组合，
这些方法在精度上并非普遍支配关系。
不能挑出各方法“更精确”的几行操作就继承正确性定理。

本地 Goblint：

- `mutexAnalysis.ml`：变量实际 protecting sets；`MustProtectingLocks`、`MayBePublicWithout` 等查询。
- `basePriv.ml:948`：读依据局部私有状态与 protected/unprotected 摘要选择来源。
- `basePriv.ml:978`：unlock 检查 `is_protected_by`、`precise_side` 与移除该锁后是否仍受保护。
- 第 989–1002 行：内层解锁后尚有其他保护锁时，不必马上发布/删除局部私有状态。

IKOS 目前“held 非空就给每个锁记一份日志”缺少等价证明。
建议明确选择一个已知框架作为基本语义，再以保守抽象实现它。

### 3.2 IKOS 的最小可用设计

给共享 byte cell/保守字段组记录：may writes、实际 MUST protection、局部 dirty/private 状态、
可见外部摘要。保护集信息自身必须随外层分析收敛。

- 任何无保护访问存在或保护事实未知时，无保护干扰通道不能省略。
- 同锁临界区内可使用局部强更新；unlock 从局部状态抽取效果，然后跨执行 join。
- 有多个嵌套锁时，只有证明剩余锁仍覆盖该变量全部相关外部写，才保持私有。
- 不同路径的保护不一致时合流撤销 MUST，保留 may effects。
- 保护集精化需与先前更保守事实兼容；不得形成“假设无竞争所以删除造成竞争的干扰”的循环论证。

以历史 `28-race_reach_60-invariant_racefree` 和 11 项 TN 回退台账为目标用例，
先从一个文件画出每次 lock/store/unlock/read 的理论摘要，再核对 Goblint 对应状态和 IKOS 探针。
[complete §9.3](ikos-bmc-complete.md#correctness) 已说明 frontend 那一行独立 A/B 并不是 11 项下降原因；
不要再次混合前端改动。不能先声称可恢复全部 11 项。

阶段出口：至少一个真实 TN 的一般语义恢复，嵌套锁/不同字段锁/锁外写/分支对照不过度过滤；
保护摘要前提与理论模型一一对应，A/B 同时报 TN、TP、UNKNOWN、内存/时间。

## 4. P2：对象、字段和关系信息精化

### 4.1 字节/字段敏感干扰

当前 scalar board 按 global 地址聚合，aggregate 入口处理偏粗；
这会使无关字段互相污染，或令本来可证明的 guard 变成 UNKNOWN。

建议摘要 key 采用对象抽象身份 + 字节覆盖范围 + 合法值类型，并保留 imprecise 全对象 bucket。
重叠访问必须同时读取/更新相交摘要；memcpy/memset、union、混合宽度、指针转换及非单例 offset 不能只按字段名归类。
当前 `AccessRecord.width` 与 DataLayout 可以复用，heap 分配点仍需多实例解释。

先实现确切 offset、明确 width 的整数/指针字段，再增加有证明的分区；
未知边界回到 whole-object/TOP。不要以“不相同字段名”或“不相同起始地址”直接认定不冲突。

### 4.2 选择性关系域与变量 packs

Astrée ESOP 2005 §6 用小变量 packs 控制关系分析成本，§4 指出精度提升有时反而减少迭代时间。
IKOS 已有 `VarPackDBM`、`VarPackDBMCongruence` 和可选 Apron 域，见
[option.hpp](../analyzer/include/ikos/analyzer/analysis/option.hpp)、
[var_packing_domain.hpp](../core/include/ikos/core/domain/numeric/var_packing_domain.hpp)。
先核实当前构建可用域和现有自动 packing 行为，不默认要新写一个数值域。

候选相关 guard、地址下标、写值关系可驱动小 packs；
跨线程关系还需要 relation-preserving interference summary，不能因为局部开启 DBM 就恢复丢失的全局关系。

Goblint ESOP 2023 §8 的 cluster 大小 ≤2 无精度损失结论有 2-decomposable domain 及其框架前提；
不能据此要求 IKOS 任意域、heap、机器算术都固定用两个变量。
关系域与机器整数溢出/位宽适配也应核对，避免把数学整数等式带入允许 wrap 的路径。

阶段出口：在具体 UNKNOWN/TN 回退文件上证实是哪条关系缺失，
局部 packing 与干扰摘要的消融各自显示收益；无收益就停止扩大关系域。

### 4.3 动态身份与 MHP

借鉴 Goblint ESOP 2023 §6–7 的创建历史/重复创建合并与 MHP，而不是复制一个更强的静态名字过滤。
IKOS 当前有 site_id、uniqueness、spawn/join 集合及 creator 关系，可先增加带来源句柄，
再选择性细分创建上下文。

同一入口多实例、循环创建、helper 创建、重复句柄覆盖、不同线程执行相同 create 都要留对照。
不能把“有一个创建点”当成唯一实例；当前已有执行次数检查应保留并补前提审计。

## 5. P3：在覆盖可靠之后优化时间与内存

### 5.1 全局摘要 widening 与 narrowing

当前黑板 `kWideningDelay=2` 后对增长区间强制 TOP，可能比原生 IKOS 的阈值 widening 更粗。
本地函数 fixpoint 已有 delay/period/threshold/narrowing 机制；先检查能否复用策略，而非重复实现全部收敛器。

候选方案：按摘要 cell 或依赖 SCC 控制 widening，提取类型/guard 的有效阈值；
在完整 coupled postsolution 上尝试 narrowing，并再次验证局部和环境闭包。
不能只收窄一个线程的局部区间而继续消费旧环境摘要。
先实测 TOP 的来源和占比，不预设强制 TOP 是当前主要 UNKNOWN 原因。

### 5.2 依赖 worklist 与稀疏候选枚举

当前 dirty 可令全部线程重新分析。建立读/写摘要依赖后，仅重算受变化影响的分析节点。
缺依赖、未知调用、TOP 地址时回退全相关集合；不能为加速忽略影响。
先处理 offset/identity/保护等摘要失效问题，再引入缓存。

race 对枚举可借鉴 Goblint `raceAnalysis.ml` 的 root/offset trie 与 `access.ml` 分组，
按可能对象和重叠字节生成稀疏 pairs。TOP bucket 必须与所有可能对象配对，
重复线程实例与未知 width 保留，不能成为隐性剪枝。
该优化应做到判定逐项不变，重点衡量访问数、pair 数、峰值内存和 checker 时间。

### 5.3 与 BMC 的预算协同

用稳定 AI facts 安排最可能成功且代价小的候选、发现 guard/地址依赖；
bounded BMC 结果可帮助选择下一次 AI 分区/变量 packs。
涉及 SAFE 剪枝时改用覆盖全部状态的独立查询，详见[研究方案](ikos-ai-bmc-feedback-research-2026-10-10.md)。
不给已有受限 `check_race` 添加“UNSAT 就 SAFE”的快捷出口。

## 6. 每一轮的执行流程和提交纪律

1. 从一个具体源码及当前诊断开始；确定 YAML/手工性质、ABI、API 合同与相关执行前缀。
2. 写出理论上每个关键读/写、锁、创建/join 后应包含的抽象值和身份。
3. 阅读对应 Miné/Goblint 原文和固定源码，确认哪些前提适用于 IKOS；参考工具报错/Unsound 则记录边界。
4. 使用现有诊断或临时 env-gated 探针核对实际状态，找到第一次丢行为或过度合流的位置。
5. 先有最小正例、无竞争/不同保护对照及改名、控制流、helper、位宽变体，再改所属语义层。
6. build、install 同步、定向回归及全量配对；性能优化同时保存耗时/内存。
7. 立即提交这一机制，不攒跨阶段 diff；commit message 写清配对数字、一般机制和残留边界。
8. 保存 <1MB 的结果/转移/证据摘要；原始 DB、大日志和临时探针不进 Git。

后续代码阶段的标准验证：

```bash
cmake --build build --target ikos-analyzer -j"$(nproc)"
cp build/analyzer/ikos-analyzer install/bin/ikos-analyzer
python3 docs/review-repros/bmc-2026-10-08/run.py --bits 32
python3 docs/review-repros/bmc-2026-10-08/run.py --bits 64
# 新增本审查六类正例及其对照/变体；代码改动时才需要维护回归 runner
IKOS_BMC_RECOVER=1 python3 svcomp_race_test.py
```

运行前核实 runner 当前是否已经带 demote、冻结目录、30 秒预算与并行数，
配对运行使用一致配置；BMC off 的 AI SAFE 变化单独统计。
若改 `frontend/` 或 Python wrapper，必须独立提交、独立 A/B，并同步对应安装组件。
本轮只有文档/证据变动，因此没有以重复 build/全量冒充新行为验证。

指标至少包含：TP/FP/TN/FN/UNKNOWN/error 的逐项转移、UNKNOWN 原因、已验证执行比例、
AI 迭代/线程重分析次数、TOP 摘要比例、访问/pair 数、SMT 查询数、CPU/墙钟、峰值内存。
正确性收紧导致 TN/TP 下降时明确归因，不能把旧漏报或假模型重新恢复以追求数字。
仍有已知未修 FN 的阶段不得标成“零 FN 认证通过”，应逐条记录遗留风险。

## 7. 可预估和不可预估的部分

可以预估机制范围：F1/F3/F5 的保守收紧通常比完整 PBR、逃逸图和关系干扰小；
F4/F6 要改读/写环境契约，PBR 与 coupled convergence 必须一起核查。
可以逐阶段验收，但此刻不应承诺开发时长、最终 TN 或全部 UNKNOWN 恢复数量。

每个精度阶段的停止条件：有一般语义前提、对照不退化、全量收益/成本可解释；
连续候选都受同一未解决大模型边界阻塞时，记录案例并停止堆局部规则。
第一轮实施建议只做 P0 并单项提交；完成后重新建立可信工程基线，再挑 P1 的一个嵌套保护案例。
论文研究则在正确基线和真实消融结果上判断是否继续，不能提前包装成已经实现的贡献。

<a id="goblint-tn"></a>
## 8. 补充：以 Goblint digest-driven 分析为参照，逐项恢复 TN

补充日期：2026-10-10。用户提到的论文已核实为 Michael Schwarz、Julian Erhard，
[Data Race Detection by Digest-Driven Abstract Interpretation](https://doi.org/10.1007/978-3-032-15700-3_15)，
**VMCAI 2026**，正式页面标注 first online 2026-01-13。
正式全文入口此次返回订阅预览，因此实际阅读全文相关部分采用作者提供的
[扩展版 arXiv:2511.11055v2](https://arxiv.org/abs/2511.11055)，2025-11-25，29 页。
不能把扩展版发布日期说成正式会议年份，也不能假称读到了付费正式版全部正文。

另核对背景论文
[The digest framework: concurrency-sensitivity for abstract interpretation](https://doi.org/10.1007/s10009-024-00773-y)，
STTT 2024；其 §3 用 admissibility 条件保证 digest 不会排除真正兼容的线程历史。
来源和指纹见[研究元数据](artifacts/ai-goblint-reference-2026-10-10/sources.json)。
本地 Goblint 仍固定 `bb55b70d91290992629ba3d073185c10cdf356e6`。
论文 [artifact](https://doi.org/10.5281/zenodo.17128591) 的公开元数据已取得，
但本轮没有下载 8.9GB 档案或复现其论文实验，不把本地 checkout 自动称作原版 artifact。

### 8.1 为什么没完整采用这个框架，也可能得到相近 TN

这里的 digest 是**计算历史的摘要**：持锁、创建历史、唯一线程、已经完成的线程、对象未逃逸等。
它不是哈希校验码，也不等同于函数输入/输出摘要或单纯把所有共享写值取 join。

VMCAI 2026 扩展版 §7 明确说：Goblint 长期已支持竞争检测，
该框架为既有机制提供 theoretical underpinning，并统一了 older ad-hoc mechanisms。
所以缺少一份统一框架，并不意味着必然缺少全部对应的 operational 能力。
IKOS 已有 AccessRecord 和析构后 pair 检查，结构上也接近论文 §6 的两阶段检测；
相近结构仍不表示已经满足其 admissibility、access stability 和访问覆盖要求。

| 论文中的 digest | IKOS 当前已有相似信息 | 需要核查/补齐的差异 |
| --- | --- | --- |
| L：locksets / RW 模式 | MUST locks/read_locks，共同锁过滤 | 完整锁身份、成功持锁；F1 的 self shortcut 有错 |
| TF：单/多线程阶段 | 创建前后 spawned 信息及 creator 顺序 | 是否涵盖全部入口/创建；F3 未解析入口缺访问 |
| TID：创建历史线程身份 | site_id、入口/uniqueness、实例集合 | 不等于完整历史型 thread ID；重复/嵌套创建要证明 |
| J：MUST joined | joined digest 与实例覆盖 | F5 的数值区间上界不是句柄身份 |
| FR：fresh / 未逃逸对象 | fresh_heap_nodes 与 escape 标记 | F2 缺 transitive publication |
| SL：符号元素锁 | symbolic index、对象/instance 对应 | 模式覆盖及数据→锁映射前提；不能仅凭相同 index |
| R：regions | region_collect 与 taint gate | 语法 gate 不是完整 region/alias 证明 |

其中 SL、R 在论文 §7 明确带星号，含时间/空间共同推理，超出基本 framework 的展示范围。
不能认为包装成一个统一 `Digest` 就自动继承它们的正确性。

论文 Fig. 8 在 **SV-COMP 2025 的 794 个 race-free 任务**上报告：
`L+TID+J` 证明 681 项，全部组合证明 712 项；实验预算 5 分钟/5GB。
这支持“锁、线程身份和 join 的组合很重要”，
**不支持**“在 IKOS 上引入一个 digest 类就会得到五倍 TN”或“必定再恢复若干项”。
两工具需固定同一源码/YAML/ABI、时限、内存和 API 假设后逐项配对，不能比较两个总数就认定能力等价。
已有六类漏报也说明当前较高 TN 中可能有未被冻结集暴露的错误规则。

### 8.2 两个不同的收益方向

**方向 A：候选访问的时间排除。**
把现有 L/TF/TID/J 信息整理成独立、可核查的 no-parallel 判定。
每个模块只在证明不并行时返回排除；否则返回 MAY，保留候选。
独立模块的保守谓词可以组合，但跨模块推断更强结论时须补专门证明。
这是扩展版 §5.2–5.3 的区分，而不是“所有条件凑齐就大概安全”。

论文也明确：两个静态访问 tuple 可以完全相同，仍可能属于不同动态线程而竞争。
IKOS 的非唯一实例复制/同语句处理不能因新接口重构而丢失。
此外，公式中的 `m_g` 是 local-trace 语义的理论 atomicity mutex，
**不能在 IKOS checker 新增成真实共同锁来隐藏数据竞争**。

**方向 B：值分析的干扰精化。**
STTT 2024 §3 让摘要/分析节点带 digest，筛选实际兼容的观察来源；
本地 `basePriv.ml:DigestWrapper` 为共享贡献保留来源，再在读时 join 未被排除的贡献。
`commonPriv.ml` 的 `ThreadDigest` / `ThreadNotStartedDigest` 明确区分：
“同唯一线程或已 joined 的贡献由本地状态涵盖”与“尚未启动线程的贡献不可能被观察”。
只有第一种所需局部状态真正涵盖效果时，才能省去对应摘要。

IKOS 当前 flat board 与入口合并会丢失来源，单加 postfilter 不能恢复 guard 所需关系。
可按需保留 `(cell, 抽象写者/创建历史, 必要保护状态)` 的效果分区，
读取时 join 全部可能来源；只有可证不兼容或已被本地状态涵盖的贡献才能排除。
未知/重复实例并入保守 bucket，分区数超预算做 join，不能舍弃。
这是机制设计建议，**本轮尚未实现，也没有测出 TN 增益**。

优先研究 A 还是 B，取决于具体 UNKNOWN 是“时间关系缺失”还是“guard/地址的值关系丢失”，
不是先为整个项目引入 digest 的全笛卡尔积。

### 8.3 可直接启动的逐文件工作流程

当前已保存的 227 项 UNKNOWN 中，**98 项 YAML 标为 Safe、129 项标为 Race Found**。
本轮重新核对 98 项输入 SHA256、活动 no-data-race YAML 性质与 ILP32 配置，
保存[初始台账](artifacts/ai-goblint-reference-2026-10-10/safe-unknown-seed.json)。
这些是未来恢复 TN 的候选池，不是承诺恢复 98 项；也不是 98 次新 Goblint 分析。
P0 修复可能改变池子，届时重新生成基线。

建议完全沿用此前 BMC 的具体文件驱动方法，但把证据方向改为安全证明：

1. **先修 P0**，重建 IKOS AI-only/BMC 基线，当前 696 个 TN 也保留审查与反例变体。
2. **同 ABI 跑 Goblint**，固定版本和完整有效配置。当前本地 ILP32 机器定义缺失会发 Unsound，
   指原安装二进制；现已按下述隔离构建校准，后续用新参考二进制。不能用 LP64 TRUE 代替 ILP32 结果。
3. 对 YAML-safe / IKOS-UNKNOWN 逐项记录：Goblint TRUE、UNKNOWN、TIMEOUT、ERROR、UNSOUND/配置不支持或结论冲突。
   Goblint TRUE + Unsound 单独归类，不作为参考安全证明。
4. 优先选 Goblint 正常 TRUE 的一个文件，写出为什么全部相关访问被排除：对象/width，
   L/TF/TID/J/FR/SL/R 各贡献了什么，哪个 guard 不可达。
5. 读取具体源码和对应 Goblint 转移/摘要，先预测各关键状态，再用已有诊断或临时探针核对。
   对某个 digest 的排除谓词做消融时，保持其他分析设置相同；
   关闭整个子分析还会影响 value analysis，不能把这种结果当成纯 pair-filter 消融。
6. 对照 IKOS，定位第一处信息丢失或不合法过滤。落地一个一般机制，而非复制最终 TRUE。
7. 为该机制做最小 race-free 正例、应发现竞争的反例和改名/字段/位宽/控制流/helper 变体。
   尤其用“移除共同锁、改变 join 目标、提前发布”检查是否只是恢复一个错误 SAFE。
8. build/install、旧正确性/BMC 执行回归、全量逐项 A/B；独立提交这一机制并更新台账。

Goblint UNKNOWN 也不等于 IKOS 必然无法证明安全。可以记录为参考边界，
有独立一般证明时仍研究 IKOS 的优势；但不把未证结论直接恢复成 TN。

### 8.4 记录、优先级和停止节点

每条记录保留：源码/YAML hash、ABI/配置、参考原生结果及 Unsound 警告、
语义解释、Goblint 源码/论文位置、IKOS 缺口、尝试与结果、正反变体、配对代价。

建议分桶顺序：

1. P0 之后仍存在的历史 TN 回退，尤其嵌套锁/PBR。
2. 正确句柄与创建历史能够排除的访问对。
3. 分字段/来源干扰能恢复 guard 关系的例子。
4. 完整 escape 下的 fresh/regions 与符号元素锁。
5. 选择性关系域与更复杂 API；成本过高时记录能力边界。

退出时要求候选池都有状态：已一般实现并恢复 TN、参考未证/失败、
参考已证但 IKOS 需尚未实现大机制、或存在规范/源码结论冲突待审查。
不是逐项试到“任何办法都不可能”，也不能把“尚未实现”写成“IKOS 理论上不能实现”。
时间/内存收益分别测量，未完成处理的输入有清楚阻塞记录；
全部可做的小机制处理完，再决定是否为剩余大机制单独立项。

前期完成了注释清理与该路线的文献/源码核实；digest 改造与 TN 恢复仍未实施。

**2026-10-10 参考环境校准**：系统实际支持 GCC13 `-m32` 编译及执行，无需修改系统库。
在临时目录复制原 opam 固定 CIL 源码（pin `2e076120a039b52f174fa799e65773f3a78d383c`），
重新生成 gcc32/gcc64 机器定义、安装到独立 prefix，再用同一 Goblint commit
`bb55b70d91290992629ba3d073185c10cdf356e6` 的 detached worktree 构建。
原源码、二进制、安装依赖及用户文件保留。生成 gcc32=Some，实际五类 sizeof 检查在每个 ABI
均通过：int/long/pointer/pthread_t/mutex，ILP32 为4/4/4/4/24，LP64 为4/8/8/8/40。
共同锁 SAFE 与三类竞争 smoke 共8次，没有 Unsound；竞争时 Goblint 原生输出 UNKNOWN，
不能称其已生成竞争 witness。完整有效配置已保存，privatization 实际为 `protection-read`。
该校准尚不是98项/更新候选池的全量配对，也没有复现论文的大型 artifact。
构建与哈希、机器定义和结果见[校准证据](artifacts/ai-goblint-reference-2026-10-10/calibration/setup.json)。

## 9. 实施进度与配对证据

### 9.1 P0-F1：撤销 self-consistency 共同锁捷径

删除 `self_locked(a) && self_locked(b)`，保留已有完整具体锁 key 的交集与 RW 模式检查。
两个访问分别与自己的对象/region 匹配，不证明两侧持同一 mutex；符号锁暂不参与这个排除。
这一步扩大候选集合，不改变值转移、前端或 BMC 公式。

build 成功，install 与构建产物逐字节相同。40 个字段锁/字段重排/RW 模式案例覆盖双位宽与
AI-only/BMC，24 个竞争配置均不再 SAFE；共同锁及读写互斥对照保持 SAFE。
旧三类 BMC 正反回归双位宽共 20 项保持边界，锁分配身份回归通过，独立执行/损坏检查 242 条通过。

| 1029 ILP32，jobs4/30 秒，demote | TP | FP | TN | FN | UNKNOWN | error |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 修复前 AI-only | 0 | 0 | 696 | 0 | 333 | 0 |
| 修复后 AI-only | 0 | 0 | 680 | 0 | 349 | 0 |
| 修复前 BMC | 106 | 0 | 696 | 0 | 227 | 0 |
| 修复后 BMC，首轮 | 102 | 0 | 680 | 0 | 247 | 0 |

16 个 TN 回退全部来自已撤销的符号/region 捷径，逐项保留，后续只能用完整锁地址相关性恢复。
两个 funloop 的可靠 TP 因新增候选耗尽搜索预算退 UNKNOWN；另两个 nondet-array 首轮退 UNKNOWN，
但独立选中任务重跑仍确认；未并行编译的全量复跑恢复这两项，
得到 TP104/TN680/UNKNOWN245、FP/FN/error0，逐项对照见下节证据。
该观测符合时间预算波动，不证明编译是唯一原因；两个 funloop 的候选膨胀仍待优化。
重导出旧 106 项时，104 项确认、130 组执行获独立 AR ACCEPT，两个未确认项不算验证成功。
这些不是官方 C/witness 验证，也不证明其他五类 FN 已解决。

证据：[阶段汇总](artifacts/ai-p0-2026-10-10/f1/summary.json)、
[1029 配对](artifacts/ai-p0-2026-10-10/f1/paired.json)、
[变化清单](artifacts/ai-p0-2026-10-10/f1/transitions.json)、
[双位宽用例](artifacts/ai-p0-2026-10-10/f1/fixtures-after.json)、
[130 组执行](artifacts/ai-p0-2026-10-10/f1/execution-results.json)。

### 9.2 P0-F3：未知线程体的覆盖回退

实际执行 `pthread_create` 时，只有解析到有函数体的入口才算已覆盖。
未解析入口/只有声明的入口记录到 per-analysis environment；DataRaceChecker 即使没有访问对，
也发出性质 Warning，不能返回 SAFE。记录在迭代间保留、分析环境清空时一并清除。
告警使用已有 unknown-function-pointer 类型和 `unknown_reason`，没有修改 Python 前端或 BMC 编码。
有限间接入口集合的解析是后续精度任务；这里没有伪称已分析那些线程体。

32 项双位宽/双模式覆盖 volatile 入口、缺少函数体、有限选择、helper、不可达创建及 join 对照。
未覆盖入口全部 UNKNOWN；字面量入口竞争在 BMC 中确认并获 AR ACCEPT；字面量先 join 与不可达创建保持 SAFE。
新增六类审查 runner：默认要求全部修复，分阶段 `--fixed F1 F3` 会明确列出仍待修的 16 次漏报，
不把它们伪装成已通过正确性验收。19 个输入的 76 次运行及旧三类 BMC 的 20 次双位宽回归通过当前阶段约束。

完整 1029 配对：AI-only TP0/TN680/UNKNOWN349 不变；BMC TP104/TN680/UNKNOWN245 不变，
两侧 FP/FN/error0，逐任务无变化。F2/F4/F5/F6 仍待修，P0 整体尚未通过。

证据：[汇总](artifacts/ai-p0-2026-10-10/f3/summary.json)、
[配对](artifacts/ai-p0-2026-10-10/f3/paired.json)、
[新用例](artifacts/ai-p0-2026-10-10/f3/fixtures-after.json)、
[原审查输入](artifacts/ai-p0-2026-10-10/f3/audit.json)。

### 9.3 P0-F5：join 只从确定句柄获得 MUST 完成信息

移除 `[0,SID]` 和 `[0,main]` 的区间上界捷径，保留现有 singleton 身份及实例/HB 过滤。
这不是完整 HandleSet，也没有扩大“join 总成功”的 API 合同；模糊身份不再生成 MUST 完成事实。
24 个双位宽/双模式用例加入只选择有效 a/b 句柄的变体、直接 join writer、join 两线程和
`{0,b}` 对照，竞争变体不再 SAFE，确定 writer/全部 join 对照保持 SAFE，不能用 `join(0)` 造 FALSE。
原审查 76 次及旧 BMC 20 次回归通过阶段约束；F2/F4/F6 尚有 12 次已知漏报。

1029 全量：AI-only TP0/TN680→677/UNKNOWN349→352；BMC TP104→104、TN680→677、
UNKNOWN245→248，FP/FN/error0。三个回退均为 `race-1_1/1_2/1_3-join`，已登记为
需要句柄来源/条件创建证明的精度任务，不能凭一个区间重新启用旧规则。

证据：[汇总](artifacts/ai-p0-2026-10-10/f5/summary.json)、
[变化](artifacts/ai-p0-2026-10-10/f5/transitions.json)、
[双位宽用例](artifacts/ai-p0-2026-10-10/f5/fixtures-after.json)、
[审查回归](artifacts/ai-p0-2026-10-10/f5/audit.json)。

### 9.4 P0-F6：共享写队列按可能效果取并集

共享队列混合线程与路径，分析器最后访问的 store 不能作为程序最后执行的 store。
flat/sentinel 与锁分区 flush 都按地址 join 所有待发布值；本地内存强更新保持原有行为。
混合宽度先确定共同类型，再逐个转换原值，避免渐进 signed cast 丢掉窄 unsigned 值。
这不是完整 PBR；在没有路径/写者归属证据的队列中，不能以终态强写覆盖其他贡献。

build/install 一致。独立摘要测试有 59 条断言，覆盖双宽度、双分区、写序颠倒、多地址、
空 flush/dirty 和混合 signedness。最后一项是在旧版采样后补充，只在修复版执行，
不伪称做过旧版混合类型 A/B。16 个双位宽/双模式程序运行中，竞争变体从 SAFE 或 UNKNOWN
统一回退 UNKNOWN，共同锁对照保持 SAFE。C11 原子操作仍不受 BMC 支持，不把回退称作 TP。
76 次原审查与旧 BMC 20 次回归通过阶段约束；F2/F4 尚有 8 次已知漏报。

1029 配对：AI-only TN677→676/UNKNOWN352→353；BMC TP104→104、TN677→676、
UNKNOWN248→249，FP/FN/error0。唯一回退是 `13-privatized_03-priv_inv.c`，已登记为
需要本地路径终态及真实保护集合证明的恢复目标；不能恢复全局队列 last-write 捷径。

证据：[汇总](artifacts/ai-p0-2026-10-10/f6/summary.json)、
[变化](artifacts/ai-p0-2026-10-10/f6/transitions.json)、
[新用例](artifacts/ai-p0-2026-10-10/f6/fixtures-after.json)、
[原审查](artifacts/ai-p0-2026-10-10/f6/audit.json)。

### 9.5 P0-F4：整数共享读覆盖本地值与可能干扰

整数 load 前，通过独立内存状态写入 external interval，再与原本地内存 join，最后执行原有
mem_read。这样包含 local ∪ interference，并让内存/数值关系一同失效，不用旧 int_set
覆盖读取结果。复用 load 的结果变量作临时值，写入后投影，不逐读创建永久 shadow。
单个全宽、offset0 的整数 global 才消费该标量摘要；读取时包括 flat、所有锁分区及未 flush
的队列，不能用 reader 持锁选择性丢弃其他写。其他共享布局或摘要缺失，对读取范围 havoc。
未逃逸 TLS 和私有局部地址保留原转移；其逃逸前提仍受 F2 与后续覆盖审查约束。

build/install 一致；32 个双位宽/双模式测试覆盖本地 store 后的新干扰、改名、struct/heap
与私有 stack/TLS、无其他 flag 写、共同锁对照。12 条摘要查找断言验证 pending/flat/全部分区。
另 12 个 DBM/var-pack-DBM 配置检查关系域，竞争 guard 不再消失、flag 仅写0对照仍 SAFE。
旧 BMC 20 次、执行及损坏检查 242 条通过；原审查 76 次中剩 F2 的 4 次已知漏报。
这些不能证明任意 API/指针/摘要收敛已完整；C11 用例回退 UNKNOWN，未伪称 BMC 支持原子值流。

1029 全量 AI-only 保持 TN676/UNKNOWN353。首轮同时跑 AI/BMC 时 BMC 为
TP104/TN676/UNKNOWN248/error1，唯一超时为 nsc-ircc 驱动；单独运行在26.3秒结束为 UNKNOWN。
随后完整 BMC 复跑得到 TP104/TN676/UNKNOWN249/error0，FP/FN0，逐任务与 F6 基线完全一致。
保留首轮与复跑证据，不把局部重跑拼成一轮全量。

证据：[汇总](artifacts/ai-p0-2026-10-10/f4/summary.json)、
[双位宽](artifacts/ai-p0-2026-10-10/f4/fixtures-after.json)、
[关系域](artifacts/ai-p0-2026-10-10/f4/relational.json)、
[首轮预算变化](artifacts/ai-p0-2026-10-10/f4/first-scan.json)、
[1029 配对](artifacts/ai-p0-2026-10-10/f4/paired.json)。

### 9.6 P0-F2：无逃逸闭包证据时不排除堆对象

checker 不再用 MUST fresh 删除堆访问。另将每个显式 pointer store 的 may-target 保守记为
可能逃逸，不依赖当前容器是否已发布；TOP 目标撤销所有地址的该私有排除，集合扩张通知
外层重算。这覆盖“先连边、后发布”的 heap/stack/TLS 末端，也覆盖 stack 根的显式连边。
它是保守后备，未实现完整可达图；copy/未知调用、spawn TOP、指针值恢复仍需效果覆盖审查。
恢复 private 精度必须用完备 may 图及未知边回退，不能重新使用直接 fresh 的捷径。

build/install 一致；56 个双位宽/双模式用例覆盖深度2/3、环、helper、数组转移、64位字段、
先发布再连边、stack/TLS 末端和 stack 根。真正独立 heap 与 join 后写对照保持 SAFE，
竞争变体全部不再 SAFE；7条逃逸/dirty/TOP/clear 断言通过。
单元测试后续更正：原版使用伪对象地址，但 points-to 集合会读取 stable_id，存在 UB，
原版7条断言不能作为证据。现改为真实 AbsoluteZero/Argv MemoryLocation，链接生产构造实现，
对已提交 F2 header 重新执行7条断言，并复跑56次程序；更正证据单独提交，分析器未改变。
先前五项机制144次定向运行、旧 BMC20次保持；原审查76次现在无 KNOWN_FN_PENDING。
这不是整个项目不存在 FN 的证明。

1029 完整顺序复跑：AI TN676→667/UNKNOWN353→362；BMC TP104→104、TN676→667、
UNKNOWN249→258，FP/FN/error0。9个 TN 回退为 list/region 私有证明案例，逐项登记。
首轮并行 AI/BMC 两侧 nsc-ircc 都超30秒；保留该轮预算变化，不用局部结果替换全量。

继续审查时新增具体 FN：`pick = nondet()? &left : &right; target = *pick`，
两个已写 pointer global 的 board 恢复按源逐次强绑定，最后一个源覆盖前一个源。
两个选择分支都可竞争，但双 ABI 下只有 y 被保留，主线程写 x 被误判 SAFE。
它不依赖 fresh 排除，必须作为下一项独立修复；当前不能宣称 P0 全部结束。
[最小输入](artifacts/ai-p0-2026-10-10/f2/pending-pointer-choice.c)、
[双位宽观测](artifacts/ai-p0-2026-10-10/f2/pending-pointer-choice.json)。

阶段证据：[汇总](artifacts/ai-p0-2026-10-10/f2/summary.json)、
[9项变化](artifacts/ai-p0-2026-10-10/f2/transitions.json)、
[56次用例](artifacts/ai-p0-2026-10-10/f2/fixtures-after.json)、
[原审查](artifacts/ai-p0-2026-10-10/f2/audit.json)。


### 9.7 P0：多源指针读保留本地与全部干扰值

旧 pointer restore 在 may-source 集合上逐项强绑定 lhs，最后一个来源覆盖其他来源。
最小例 `pick = nondet()? &left : &right; target = *pick; *target=2` 中，
主线程并发写 x 被误判 SAFE，而写 y 得到 UNKNOWN；Dartagnan 原生双 ABI 均 FAIL。
修复将 pointer board 改用现有 PointerAbsValue，保留初始化、nullity、points-to 和 offset；
load 前把每个来源写入独立内存状态并与本地 join，再执行原 mem_read。
NULL/TOP 也参与摘要，offset 等任何值分量扩张都触发 dirty；缺共享摘要时 havoc 读取范围。
已被本地值包含的摘要跳过重复合并，避免无意义的状态复制。

build/install 一致；40 次双 ABI/双模式程序运行、48 条真实 MemoryLocation 摘要断言、
原审查76次及旧BMC20次通过。直接不相交与join对照保持SAFE。
历史指针值和私有原子指针对照只要求不报确定FALSE，不能称作保持SAFE；
原子指针用例修复前已UNKNOWN，不能把它们算成新修复FN。
数组容器用例是global数组，heap摘要有单元覆盖及既有F2堆回归，不能混称malloc用例。

1029 完整顺序扫描：AI TN667→662、UNKNOWN362→366、error0→1；
BMC TP104→104、TN667→662、UNKNOWN258→262、error0→1，FP/FN0。
五个TN回退为 list2、hclhlock 和三个weaver程序，逐项登记。
安全标签的 nsc-ircc 原为UNKNOWN，现在两侧均超过30秒；初版及优化版完整扫描结果一致。
这是性能退化，不把局部重跑拼成error0的全量；P2需单独优化。
原六类及本次具体FN已修，但耦合不动点和效果覆盖仍待验收，尚不能宣称P0全部完成。

证据：[汇总](artifacts/ai-p0-2026-10-10/pointers/summary.json)、
[变化](artifacts/ai-p0-2026-10-10/pointers/transitions.json)、
[修复前](artifacts/ai-p0-2026-10-10/pointers/fixtures-before.json)、
[修复后](artifacts/ai-p0-2026-10-10/pointers/fixtures-after.json)、
[Dartagnan](artifacts/ai-p0-2026-10-10/pointers/reference.json)。


### 9.8 P0：耦合摘要参与收敛，最终检查变更回退

join exit摘要任何改变（含MUST移除）、spawn实参points-to/offset扩大、逃逸信息变化、
新增create/once实例均通知外层重算。TOP实参不再丢弃，同时撤销不可靠的私有地址排除；
offset使用同一外层轮数的延迟widening。create点继承的MUST集合对历次观察取交，
跨轮保存；round reset仅清临时状态，完整clear仍清摘要。
所有锁分区及无锁队列均在判收敛前flush，不能依赖后续unlock才能通知重算。
最终run_checks及全局析构后再次flush；若摘要变化，插入race Warning和明确unknown_reason，
阻止旧快照证明SAFE。本阶段未改成自动丢弃旧访问记录后的重收敛，这是保守能力边界。

build/install一致；真实AR函数/call和真实MemoryLocation的40条断言覆盖变更、幂等、
TOP、offset、跨轮交集、queue和完整clear；旧header在第一条join dirty断言上失败。
40次双ABI/双模式程序测试及原审查76次、旧BMC20次通过。
析构中新建线程用例从SAFE回退UNKNOWN并有边界诊断；这是效果覆盖边界对照，本例本身安全，
不能算作修复FN。嵌套helper的transitive-join安全对照仍UNKNOWN，直接join对照保持SAFE。

1029完整顺序AI/BMC扫描与pointer阶段逐任务一致：AI TN662/UNKNOWN366/error1；
BMC TP104/TN662/UNKNOWN262/error1，FP/FN0。原安全标签nsc-ircc超时仍在，未拼接局部重跑。

效果审查继续发现两个具体漏报，双ABI都SAFE：
(1)同一struct的两个pthread_once控制字段被当作同一对象，两个回调写同一数据被排除；
(2)共享memcpy写未发布，reader的flag守卫被删，payload竞争消失。
Dartagnan对原once输入给PASS但明确不支持pthread_once，对动态长度copy给ERROR；
这些输出不能背书。Goblint校准配置对once双ABI给竞争警告（最终UNKNOWN，无Unsound）。
once首次调用的显式展开与n==sizeof(flag)路径的常量copy特化在Dartagnan双ABI均FAIL；
这种依据是源码/规范推理加等价特化的佐证，不冒充原输入原生确认，也不是IKOS witness验证。
两项独立修复待续，不能宣称P0已经全结束。

证据：[汇总](artifacts/ai-p0-2026-10-10/coupling/summary.json)、
[新回归](artifacts/ai-p0-2026-10-10/coupling/fixtures-after.json)、
[1029配对](artifacts/ai-p0-2026-10-10/coupling/paired.json)、
[新漏报](artifacts/ai-p0-2026-10-10/coupling/pending-coverage.json)、
[原生once限制](artifacts/ai-p0-2026-10-10/coupling/pending-once-native-reference.json)、
[特化参考](artifacts/ai-p0-2026-10-10/coupling/pending-specialized-reference.json)。


### 9.9 P0：pthread_once 控制对象与回调覆盖

控制对象身份改为带独立known/ambiguous命名空间的(base,精确byte offset)结构键，
不能忽略同一struct/array的字段偏移或用会碰撞的整数打包。
只对范围内、非TLS、已定义global的静态常量地址建立MUST once完成事实；
变量下标、加载地址、TLS/汇总对象和未知/声明回调均留下明确UNKNOWN边界。
同一control可能传不同callback时，每个函数都必须记录该实例；唯一函数映射不成立则置未知，
不能把某一回调漏出multiplicity统计。新增通用unmodeled-effect元数据供checker产生Warning，
也通知外层重算，未引入文件名、比赛标签或参考结果分支。

build/install一致；30条真实AR对象断言、48次双ABI/双模式程序运行、原审查76次、旧BMC20次通过。
struct两个字段、array两个元素、TLS每线程对象和声明回调的竞争变体旧版SAFE，新版UNKNOWN；
同一control的直接struct/array及等价地址对照仍SAFE。
不同control但顺序调用，以及同一control选择不同callback的安全对照只保证不报确定FALSE，
仍有实例/MHP精度空间。额外callback注册遗漏有单元反例，复杂程序旧版已UNKNOWN，
不能把该程序当作新修复的FN。未扩展BMC的once语义，未把Goblint may-warning当witness验证。

1029完整顺序AI/BMC扫描逐任务与coupling阶段一致：AI TN662/UNKNOWN366/error1；
BMC TP104/TN662/UNKNOWN262/error1，FP/FN0。共享byte effects与未知外部调用覆盖仍待续。
证据：[汇总](artifacts/ai-p0-2026-10-10/once/summary.json)、
[双ABI修复前](artifacts/ai-p0-2026-10-10/once/fixtures-before.json)、
[修复后](artifacts/ai-p0-2026-10-10/once/fixtures-after.json)、
[1029配对](artifacts/ai-p0-2026-10-10/once/paired.json)。


### 9.10 P0：字节内存操作发布与覆盖边界

memcpy/memmove/memset原先绕过Store的跨线程写摘要，线程在mutex下复制flag=1，
另一个线程先写flag=0再读取，真实可能读到1的路径被删，payload竞争误判SAFE。
完整、offset0、精确长度、单一global整数/指针标量现在通过复用typed Store发布结果；
共享copy源在读取前havoc，避免用过期本地值排除可能执行。
其他非零目标布局留下明确byte-effects UNKNOWN边界，copy还保守记录可能逃逸；
零长度保持no-op。没有前端改动，也未修改BMC竞争公式。

build/install一致；96次双ABI/双模式/宽度/改名配置、pointer程序40次、
原审查76次、旧BMC20次通过。copy/move/set及pointer发布主反例不再SAFE，
复制零值、同锁保护payload和零长度对照仍SAFE；private copy对照只保证不报确定FALSE。
部分写和共享array源明确回退，未生成新的BMC FALSE。
原生Dartagnan动态长度输入ERROR；之前的常量长度路径特化FAIL只是佐证，不能当原输入确认。

1029完整顺序AI/BMC扫描逐任务与once阶段一致：AI TN662/UNKNOWN366/error1；
BMC TP104/TN662/UNKNOWN262/error1，FP/FN0。nsc-ircc超时保留，未用局部重跑替换。
P0尚未结束：另已复现heap通配字段写被精确字段读遗漏、LP64 offset=2^32的锁键碰撞。
未知extern、aggregate和数值地址覆盖继续审查，假设与已复现问题分开记录。
证据：[汇总](artifacts/ai-p0-2026-10-10/bytes/summary.json)、
[修复前](artifacts/ai-p0-2026-10-10/bytes/fixtures-before.json)、
[修复后](artifacts/ai-p0-2026-10-10/bytes/fixtures-after.json)、
[1029配对](artifacts/ai-p0-2026-10-10/bytes/paired.json)。


### 9.11 P0：堆指针通配字段与精确字段重叠

已复现的独立FN：reader先在mutex内把slot0重设为x，publisher在同mutex内向
slot[nondet()?0:1]写z，reader随后读取slot0并在锁外写*p，与main写z竞争。
旧精确字段getter只取slot0摘要，遗漏通配写，因此双ABI/双模式均SAFE；
原生Dartagnan对保存的原输入双ABI均FAIL，无unsupported警告，仍不等于IKOS witness验证。
修复把摘要键改为完整(allocation-site,exact-offset?,ZNumber offset)结构：
精确读合并对应字段和通配写，未知读合并该base全部字段。
无摘要时继续已有havoc回退；offset或base高32位不再损坏身份。
allocation-site仍是汇总对象，不能把结构键当动态堆唯一性证明。

64次双ABI/双模式配置、92条真实MemoryLocation摘要断言、byte96次、
原审查76次、旧BMC20次通过。slot0/slot1竞争变体SAFE→UNKNOWN，
未知下标读原来已UNKNOWN，不能算成新修复FN；不相交目标、同锁、join对照仍SAFE。
单元覆盖通配与精确摘要并存、未知读、缺精确字段但有通配写、不同base，
以及超32位base/offset。没有新BMC FALSE或前端修改。

1029完整顺序AI/BMC扫描逐任务与bytes阶段一致：AI TN662/UNKNOWN366/error1；
BMC TP104/TN662/UNKNOWN262/error1，FP/FN0。nsc-ircc超时仍在。
大偏移锁键碰撞和未知extern覆盖待续，未宣称P0全部通过。
证据：[汇总](artifacts/ai-p0-2026-10-10/heap/summary.json)、
[修复前](artifacts/ai-p0-2026-10-10/heap/fixtures-before.json)、
[修复后](artifacts/ai-p0-2026-10-10/heap/fixtures-after.json)、
[原生参考](artifacts/ai-p0-2026-10-10/heap/reference.json)、
[配对](artifacts/ai-p0-2026-10-10/heap/paired.json)。


### 9.12 P0：锁偏移打包只对可表示身份建立MUST

LP64单一malloc对象内，mutex offset0与offset2^32的打包键旧版相同，
两个线程分别持不同mutex写同一data，误判SAFE。偏移16/2^32+16变体亦复现。
现在仅允许非零base≤2^32-1、非负offset≤2^32-1；其他返回0（无确定身份），
所有直接调用点已核对。获取不得加入key0，解锁不可表示地址按unknown release
移除可能持锁并flush所有写分区，cond wait不得恢复key0为MUST。
这是保守表示边界，没有把地址低32位相同解释为mutex相同；symbolic锁仍不参与SAFE共同锁排除。

build/install一致；20次双ABI/双模式程序、12条打包边界断言、pointer64次与摘要92条、
原审查76次、旧BMC20次通过。两个LP64竞争变体SAFE→UNKNOWN，
ILP32采用256字节距离、原本已UNKNOWN，不能算作新增修复；共同锁、外层共同锁和join仍SAFE。
原生Dartagnan因BigInteger范围报ERROR，校准Goblint因CIL typeOffset报错，均不作为竞争确认。
另一本机LP64无竞争探针实际分配对象，持有第一个mutex时trylock第二个成功，
确认4GB距离对应不同对象。它只核实身份前提，不是IKOS witness验证。

1029完整顺序AI/BMC扫描逐任务与heap阶段一致：AI TN662/UNKNOWN366/error1；
BMC TP104/TN662/UNKNOWN262/error1，FP/FN0。未知调用和剩余效果覆盖继续审查，
仍不能据此宣称全项目SAFE已证明。nsc-ircc超时保留。
证据：[汇总](artifacts/ai-p0-2026-10-10/locks/summary.json)、
[前后输入结果](artifacts/ai-p0-2026-10-10/locks/fixtures-after.json)、
[参考限制与本机检查](artifacts/ai-p0-2026-10-10/locks/reference.json)、
[1029配对](artifacts/ai-p0-2026-10-10/locks/paired.json)。


### 9.13 P0：未知调用体的访问覆盖与经过核实的声明合同

未知extern及递归/未展开internal调用即使havoc数值，也不能覆盖被省略的访问事件，
现记录显式unmodeled-effect Warning。已复现递归update最终写data与worker竞争，
旧版双ABI均SAFE，新版UNKNOWN；同锁递归对照亦UNKNOWN，属于能力边界。
声明extern用例只证明缺失代码覆盖边界，不把它当作已独立确认的具体FN。
已定义的写函数及官方nondet守卫竞争对照由原生Dartagnan双ABIFAIL佐证。

保留的声明合同有：SV-COMP2026规则列出的精确零参无副作用nondet API（不含lookalike、
nondet_memory、错误返回类别）；有效opaque pthread对象的NULL属性init/destroy；
glibc2.39 assert.h中三个精确ABI的终止hook，复用abort，checker仍记录buffer访问。
NULL可经helper/分支nullity证明；非默认属性仍回退。LLVM不保留C unsigned拼写，
assert行号合同检查实际32bit ABI而非源语言signedness。没有frontend行为改动。

build/install一致；108次双ABI/双模式/改名/helper配置、原审查76次、旧BMC20次通过。
1029完整顺序AI/BMC扫描：AI TN662→647、UNKNOWN366→381；
BMC TP104→104、TN662→647、UNKNOWN262→277，FP/FN0、error1（nsc-ircc超时）不变。
15个SAFE→UNKNOWN逐个按调用点归因：sleep4项、mutexattr/非默认init2项、
pthread TLS key3项（其中析构callback确需覆盖）、pthread_exit6项。
这些是待补合同/效果模型的精度损失，不宣称标签安全输入是实际漏报。
第一轮未经assert ABI校正的临时试验曾TN28，未提交，最终647来自重新完整扫描，未拼接结果。

未证明所有SAFE规则。另已发现LP64外部libatomic store值未发布导致守卫路径消失，
ILP32的__int128原输入编译失败，必须另建有效双ABI版本才作双架构结论；下一阶段独立处理。
证据：[汇总](artifacts/ai-p0-2026-10-10/calls/summary.json)、
[修复前](artifacts/ai-p0-2026-10-10/calls/fixtures-before.json)、
[修复后](artifacts/ai-p0-2026-10-10/calls/fixtures-after.json)、
[官方/库合同](artifacts/ai-p0-2026-10-10/calls/contracts.json)、
[参考](artifacts/ai-p0-2026-10-10/calls/reference.json)、
[15项归因](artifacts/ai-p0-2026-10-10/calls/regression-causes.json)、
[全量配对](artifacts/ai-p0-2026-10-10/calls/paired.json)。


### 9.14 P0：外部原子库效果覆盖与函数名前缀

GCC generic libatomic store的运行时ABI为(size_t,void*,void*,int)。新8byte版本在
ILP32/LP64均可编译，mutex下writer写flag=1而reader先重设0再读取，锁外payload
访问真实无序；旧版双模式SAFE。它经过SingleCall路由只havoc本地内存，未发布值摘要。
原16byte版本只在LP64可用，结论不外推到ILP32。
另外普通定义的ELF符号带__atomic_/__sync_前缀时旧版注入共同伪锁，
两个线程调用同一写函数也SAFE。输入用普通C标识符加asm符号指定，避免仅凭保留C名字论证。

移除GCC名字家族的伪锁分类；已定义函数正常分析，未覆盖声明走已有extern边界。
SV-COMP规定的verifier atomic定义仍按真实函数体分析；仅有声明的single-call atomic
另记录atomic-call-effects UNKNOWN，原子性不能代替被省略函数体的值/内存效果。
GCC实际降为AR atomic Load/Store的路径不变。此阶段没有实现新的libatomic值模型，
BMC对GCC-looking helper仍保守拒绝展开，不因此承诺这些新例变成TP。

build/install一致；46次双ABI/双模式/改名/typed对照、原审查76次及旧BMC20次通过。
竞争的generic store、普通前缀定义与未知声明均不再SAFE；安全的atomic写0/同锁payload
对照也UNKNOWN，明确是能力边界。原生Dartagnan对typed对照及前缀定义双ABI均FAIL，
对generic store却警告unknown intrinsic后PASS，后者无效，不能作为安全依据。
GCC官方说明核实ABI，本机32/64bit运行核实8/16byte写入值，只证明库效果前提，不是witness验收。

1029完整顺序AI/BMC扫描逐任务与calls阶段一致：AI TN647/UNKNOWN381/error1；
BMC TP104/TN647/UNKNOWN277/error1，FP/FN0。nsc-ircc超时保留。
效果旁路审查另复现read、printf的%n和strncpy的共享值未发布，以及read(NULL,0)
错误终止有效后续路径，双ABI/双模式均SAFE；这些作为下一项独立机制处理。
原生Dartagnan在库操作上未正确覆盖，校准Goblint也遗漏%n，参考PASS不当作真值。
证据：[汇总](artifacts/ai-p0-2026-10-10/atomics/summary.json)、
[前](artifacts/ai-p0-2026-10-10/atomics/fixtures-before.json)、
[后](artifacts/ai-p0-2026-10-10/atomics/fixtures-after.json)、
[合同](artifacts/ai-p0-2026-10-10/atomics/contracts.json)、
[原生参考](artifacts/ai-p0-2026-10-10/atomics/reference.json)、
[本机ABI](artifacts/ai-p0-2026-10-10/atomics/native.json)、
[全量配对](artifacts/ai-p0-2026-10-10/atomics/paired.json)。


### 9.15 P0：库buffer效果与零字节read续行

三个独立共享值旁路已复现：read向int flag写1、printf("x%n",&flag)写1、
strncpy向byte flag写1；reader先在mutex下重设0后再读，缺少writer摘要时守卫死掉，
旧版双ABI/双模式SAFE。另有read(NULL,0)被prepare_mem_access错误bottom，
其后main与worker的两个data写被漏掉。局部count被printf/sprintf/snprintf的%n修改也被漏掉，
四种局部守卫例（含snprintf(NULL,0,...)）旧版均SAFE。

新增intrinsic库效果检查复用现有points-to/escape共享判定：可能共享的输出buffer
留下明确library-memory-effects边界，未引入库内值流的假精度。printf/scanf家族的
variadic pointer无格式证明时都按可能输出处理；局部输出也havoc，防止保留过期guard值。
零长度read不解引用buffer，返回值保持保守，合法NULL续行保留；snprintf的size0
仅免目标buffer检查，%n仍有其自身写效果。strncat即n0也可能写terminator，未作为no-op。
这些是通用调用/布局规则，无filename/标签分支；未修改frontend或BMC公式。

build/install一致；100次双ABI/双模式/改名/共享及私有/共同锁/typed对照通过，
原审查76次、atomic46次、旧BMC20次通过。八类竞争例不再SAFE；私有buffer和typed同锁对照SAFE，
库模型缺失的shared同锁安全对照UNKNOWN。raw sscanf符号版本也加覆盖边界，
glibc __isoc99_sscanf版本本来已由unknown-extern gate挡住，不混称同一个新FN。
POSIX2024 read/printf合同及本机两ABI核实read1、%n、strncpy和零字节效果，只证明库效果，
不是竞争witness验收。Dartagnan对read/strncpy/sscanf有unknown-intrinsic却PASS，
对%n不正确覆盖且PASS；Goblint对read/strncpy给may-race UNKNOWN，但%n给TRUE。
这些参考结果不能推翻规范及实际效果；asm别名sscanf在Goblint又有Unsound，明确排除。

1029完整顺序AI/BMC扫描：AI TN647→645、UNKNOWN381→383；
BMC TP104→104、TN647→645、UNKNOWN277→279，FP/FN0、error1不变。
仅bigshot_s/s2的strcpy SAFE→UNKNOWN：缺少足够逃逸/时序证明时保守覆盖heap写，
不把安全标签回退算作具体FN。nsc-ircc超时未用局部重跑替换。

重新建立当前safe-label UNKNOWN池148项，校准Goblint32bit参考：TRUE26、UNKNOWN59、
不支持/Unsound60、ERROR2、TIMEOUT1。TRUE也不是自动真值；需源码证明和首次精度损失诊断。
此前98项seed台账已过时。原始日志留/tmp，保存逐文件命令、输入/二进制hash和摘要。
P0仍有句柄表示及未完成的SAFE前提审查，不能宣称普遍零FN；precision恢复计划不取消。
证据：[汇总](artifacts/ai-p0-2026-10-10/library/summary.json)、
[前](artifacts/ai-p0-2026-10-10/library/fixtures-before.json)、
[后](artifacts/ai-p0-2026-10-10/library/fixtures-after.json)、
[合同](artifacts/ai-p0-2026-10-10/library/contracts.json)、
[本机效果](artifacts/ai-p0-2026-10-10/library/native.json)、
[参考局限](artifacts/ai-p0-2026-10-10/library/reference.json)、
[Goblint](artifacts/ai-p0-2026-10-10/library/goblint-reference.json)、
[2项归因](artifacts/ai-p0-2026-10-10/library/regression-causes.json)、
[全量](artifacts/ai-p0-2026-10-10/library/paired.json)、
[当前参考池](artifacts/ai-p0-2026-10-10/library/goblint-safe-unknown-pool.json)。


### 9.16 P0：合成线程句柄表示与 join 来源

IKOS 把创建点 SID 写入 scalar pthread_t，将 main 的 pthread_self 写成 0xA7E0AD1D；
这是内部身份令牌，不是目标 libc 的可观察数值。create 后比较 t!=1、或比较
pthread_self()!=main-token 的分支，旧版双 ABI/双模式可错误消失并 SAFE。
本机 glibc 两 ABI 探针核实实际值不同，只验证表示前提，不当作竞争 witness。
POSIX 2024 pthread_equal 的 rationale 允许结构类型身份，pthread_join 要求有效 joinable 目标。
Dartagnan 对 t!=1 为 PASS、t==1 为 FAIL，采用不同句柄合同；不能因此笼统指控其不 sound。

全局摘要新增 handle-slot→SID 集合，变更触发耦合重算；轮次重置保留、完整 clear 清除。
读取令牌只允许直接 declared join 与唯一同宽 assignment/bitcast 链；数值/字节观察、
重写、存储或其他消费者留下 representation 边界。join 的 singleton SID 还必须可追溯
到标记槽的 offset0 load 或 declared self，不能把 literal/nondet 凑出的 SID 当真实目标。
私有未逃逸槽在相关 SID 尚不在 MAY-spawn 时的 seed-store 豁免，避免 ikos-pp 的
nondet 初始化被误判成句柄重写；其后所有未知身份/逃逸/别名均保持保守。
BMC 暂时以该边界作 program-wide gate，避免内部数值再用于分支/值 witness。
尚未实现完整 HandleSet/provenance；TOP points-to 和整个对象标记会损失精度。

build/install 一致，56 次句柄双 ABI/双模式/改名对照、64 次 pointer 配置及实际对象
unit 断言、库100次、原审查76次和旧BMC20次通过。plain join、同宽复制和pre-create
初始化仍SAFE；数值观察、自身数值比较、字节copy/rewrite及伪造SID均UNKNOWN。
literal/nondet-forged join 两例属于不支持来源边界，不冒称已证实的具体FN。

1029完整顺序扫描：AI TN645→643、UNKNOWN383→385；
BMC TP104→97、TN645→643、UNKNOWN279→288，FP/FN0、error1。
2个TN是ptra_nr/arraylist的TOP别名及对象粒度边界，非已确认真实竞争。
7个TP都是thread-join-counter inner/outer 的pthread_detach令牌消费：当前仅允许join，
于是程序级gate撤回旧确认，不等于旧witness已被证伪。下一轮核实detach的纯身份消费，
单独恢复；不撤销数值观察gate。nsc-ircc超时仍计error，未替换为局部结果。

证据：[汇总](artifacts/ai-p0-2026-10-10/handles/summary.json)、
[前](artifacts/ai-p0-2026-10-10/handles/fixtures-before.json)、
[后](artifacts/ai-p0-2026-10-10/handles/fixtures-after.json)、
[合同](artifacts/ai-p0-2026-10-10/handles/contracts.json)、
[本机ABI](artifacts/ai-p0-2026-10-10/handles/native.json)、
[参考差异](artifacts/ai-p0-2026-10-10/handles/reference.json)、
[9项归因](artifacts/ai-p0-2026-10-10/handles/regression-causes.json)、
[全量配对](artifacts/ai-p0-2026-10-10/handles/paired.json)。


### 9.17 精度：detach 的不透明身份消费

POSIX 2024 pthread_detach 只使目标由 joinable 变 detached，不使其立即终止，也不建立
join/HB。Goblint libraryFunctions.ml 的 detach 摘要丢弃 thread 值且不读写应用内存。
直接 declared、非variadic、单参数、32bit int 返回的 detach 可消费内部身份令牌，
不需要假定令牌就是 libc 的数值表示。仅扩展 identity-only-use 白名单；数值/字节
观察仍 gate，join 来源证明不放宽，detach 的未知extern效果边界仍保留，未捏造同步边。

新 plain detach 竞争例双 ABI 都由 BMC 确认，选中执行重放ACCEPT；detach 后数值比较
仍UNKNOWN。64次句柄对照、原审查76次、旧BMC20次通过，build/install一致。
7个thread-join-counter旧确认逐文件再次导出选中执行，全部ACCEPT；这是具体AR/API
执行检查，不等同官方C/witness验收。

1029完整顺序AI/BMC配对仅恢复这7项：AI TN643/UNKNOWN385/error1不变；
BMC TP97→104、TN643不变、UNKNOWN288→281，FP/FN0、error1。
两个TOP别名TN仍UNKNOWN，句柄通用值流尚未实现。没有frontend改动。
证据：[汇总](artifacts/ai-p0-2026-10-10/detach/summary.json)、
[合同](artifacts/ai-p0-2026-10-10/detach/contracts.json)、
[对照](artifacts/ai-p0-2026-10-10/detach/fixtures-after.json)、
[7个选中执行](artifacts/ai-p0-2026-10-10/detach/executions.json)、
[全量配对](artifacts/ai-p0-2026-10-10/detach/paired.json)。


### 9.18 精度：经过核实的 POSIX sleep 声明

由4个真实safe-label UNKNOWN开始核对源码：priv_multi的A/B各受mutex_A/mutex_B保护，
cond_wait三个变体对g的访问均持mut；值断言的真假不属于本轮no-data-race属性。
共同精度损失是sleep被unknown-extern效果gate挡住。校准Goblint四项TRUE、无Unsound；
libraryFunctions.ml:394的sleep模型不访问应用内存，返回未知seconds。
POSIX2024明确线程暂停，multi-threaded sleep不借用SIGALRM；不释放用户mutex，
也不建立HB。信号/取消注册仍走各自现有未覆盖调用边界，不能由纯sleep隐藏回调。

仅准确匹配declared sleep、非variadic、一个32bit整数参数和32bit整数返回的AR合同，
复用无内存副作用的unknown-return transfer。返回值保持不确定；不改锁集，不加同步边，
不把sleep(N)当“另一个线程必已结束”。自定义body继续分析，错原型/相似名字保守处理。
没有把sleep加入BMC leaf/value编码，经过sleep的反例前缀仍须独立补足调用语义。

build/install一致；48次双ABI/双模式/改名/helper/锁/返回守卫/信号/错原型对照通过，
原审查76次、句柄64次与旧BMC20次通过。未持共同锁的例保留UNKNOWN；
可疑返回守卫没有被常量0强制删掉。新Goblint十项对照无Unsound，安全3类TRUE，
两类may-race UNKNOWN；不会把参考UNKNOWN误称TP。

1029完整顺序配对仅恢复上述4项：AI TN643→647、UNKNOWN385→381；
BMC TP104不变、TN643→647、UNKNOWN281→277，FP/FN0、error1。
此收益是准确调用效果覆盖，不冒称实现了Goblint完整PBR或digest框架。
证据：[汇总](artifacts/ai-p0-2026-10-10/sleep/summary.json)、
[合同](artifacts/ai-p0-2026-10-10/sleep/contracts.json)、
[对照](artifacts/ai-p0-2026-10-10/sleep/fixtures-after.json)、
[校准参考](artifacts/ai-p0-2026-10-10/sleep/goblint.json)、
[四个参考结果](artifacts/ai-p0-2026-10-10/sleep/corpus-reference.json)、
[源码充分前提](artifacts/ai-p0-2026-10-10/sleep/corpus-proof.json)、
[全量](artifacts/ai-p0-2026-10-10/sleep/paired.json)。


### 9.19 精度：NULL状态的当前线程退出

6个triangular safe-label UNKNOWN的唯一覆盖边界为pthread_exit(NULL)。全部共享i/j
访问位于verifier atomic区域；no-data-race不要求数值断言成立。校准Goblint六项TRUE、
无Unsound，libraryFunctions.ml:470建模ThreadExit并将返回状态传给join，不解引用状态。
POSIX2024明确退出当前线程、执行cleanup/TLS析构、不释放mutex。故不能简单
假设无回调，也不能把它变成进程abort或隐式unlock。

仅支持准确declared、非variadic、void返回、单pointer参数且状态可证NULL的合同；
复用参数检查后结束当前normal continuation，保留退出前记录/摘要，不修改锁集或
其他线程状态。非NULL状态仍走原覆盖边界；cleanup/TLS/进程清理注册API保留其
未知效果警告，当前没有实现析构调用或完整退出状态值流，也没有增加BMC exit leaf。
这依赖已覆盖的注册API边界，后续扩展注册模型时必须同时补齐退出回调，不能直接删gate。

build/install一致；52次双ABI/双模式/helper/改名/退出前竞争/当前main退出/不释放锁/
cleanup/TLS/错误声明/非NULL对照通过；旧审查76、sleep48、句柄64与旧BMC20通过。
无noreturn声明的变体AR仍有exit后的data=1，transfer确实阻止续行；普通同名defined
body正常分析，其真实写竞争双ABI确认并重放ACCEPT。NULL能力不会盖掉回调警告。
两ABI原生glibc探针确认NULL状态、无后续写、mutex仍EBUSY；仅证明库效果，不是witness。
Goblint20项参考中，cleanup/TLS与asm-alias无noreturn例有Unsound，明确排除其背书；
非NULL安全例Goblint为TRUE而IKOS仍UNKNOWN，是保留的能力边界。

1029完整顺序配对仅恢复6个triangular：AI TN647→653、UNKNOWN381→375；
BMC TP104不变、TN647→653、UNKNOWN277→271，FP/FN0、error1。未修改frontend。
证据：[汇总](artifacts/ai-p0-2026-10-10/exit/summary.json)、
[合同](artifacts/ai-p0-2026-10-10/exit/contracts.json)、
[对照](artifacts/ai-p0-2026-10-10/exit/fixtures-after.json)、
[原生效果](artifacts/ai-p0-2026-10-10/exit/native.json)、
[AR续行](artifacts/ai-p0-2026-10-10/exit/ar-continuation.json)、
[参考边界](artifacts/ai-p0-2026-10-10/exit/goblint.json)、
[六项源码前提](artifacts/ai-p0-2026-10-10/exit/corpus-proof.json)、
[全量配对](artifacts/ai-p0-2026-10-10/exit/paired.json)。


### 9.20 正确性：指针对象表示与重叠写入

`ai-pointer-bytes/bytes.c`的scalar global字节复制及NULL表示复制，在此前已提交
`f4b4f92`上两ABI、AI/BMC均SAFE。writer完整逐字节复制合法指针值后被main join；
reader启动后读取的新指针使data=1可达，与main的data=2无同步。对象表示本身没有
并发读写。C11 N1570 §6.2.6.1p4、§6.5p7支持该字符访问；不是任意字节都可解释成
合法指针的假设。两ABI原生顺序化探针确认guard可达，但不把执行UB的数据竞争当
原生证明。Dartagnan 4.3.1、bound12在两ABI分别FAIL，原值对照PASS，无未知intrinsic
警告；Goblint有data竞争告警并返回UNKNOWN，对照字节复制也UNKNOWN。参考工具
结果仅辅助核对，不替代源码和标准前提。

根因是非pointer store只影响整数board，指针board仍保留旧有限may-value。
新机制在ConcurrentGlobalEnv独立记录global/heap的写入范围及完整pointer值分类，
不依赖“该指针摘要是否已被发现”。按实际read offset及pointer width检查相交：
相交的raw写、部分/异位pointer写、不能证明位置的写均使该read取TOP；
只有精确同slot、完整类型指针写可以继续用joined may-value。邻接字节字段不相交，
可保留摘要。未知offset或超过64个footprint回退whole-object；端点算术用ZNumber。
footprint增加通知dirty，跨普通分析轮次保留，完整clear同时清除；不按锁或线程名
跳过原始写，也不把锁外写归入无效摘要。

直接摘要断言覆盖写入先于/后于摘要发现、dirty/reset/clear、邻接字段、完整同位、
错位typed pointer、未知offset及32/64bit。新增72次双ABI/双模式/正反例/改名/helper
回归覆盖scalar、global struct、heap struct、邻接字段、typed赋值、memcpy、NULL、
GNU packed overlap；后者仅为本地Linux/GNU布局变体，旧版本就UNKNOWN，未声称
又发现独立FN。原指针64次、正确性76次、句柄64次、sleep48次、exit52次及旧BMC20次
通过。旧指针control case11（不确定pointer-array slot）由SAFE回退UNKNOWN：当前
没有表达“只可能两个对齐slot”的证书，不能用完整写类型替代字节重叠证明；这是
明确保留的精度边界。

1029完整顺序AI/BMC配对逐任务均无变化：AI TN653/UNKNOWN375；BMC TP104/TN653/
UNKNOWN271，FP/FN0、error1（同一nsc-ircc timeout）。集合外两个已复现FN由SAFE回退
UNKNOWN，未伪造竞争确认；有限集合零FN不是全部SAFE的证明。build/install一致，
源码/安装/fixture指纹已保存；没有frontend改动，不宣称本轮测得加速。
证据：[汇总](artifacts/ai-p0-2026-10-10/ptrbytes/summary.json)、
[标准合同](artifacts/ai-p0-2026-10-10/ptrbytes/contracts.json)、
[修复前](artifacts/ai-p0-2026-10-10/ptrbytes/fixtures-before.json)、
[修复后](artifacts/ai-p0-2026-10-10/ptrbytes/fixtures-after.json)、
[原生guard](artifacts/ai-p0-2026-10-10/ptrbytes/native.json)、
[Dartagnan](artifacts/ai-p0-2026-10-10/ptrbytes/dartagnan.json)、
[Goblint](artifacts/ai-p0-2026-10-10/ptrbytes/goblint.json)、
[全量配对](artifacts/ai-p0-2026-10-10/ptrbytes/paired.json)。


### 9.21 简化配置 Goblint 对照池（历史，已由§9.23更新）

`e440a19`基线实际有140个safe-label UNKNOWN，已全部重新运行固定Goblint校准二进制，
没有复用旧结果。ILP32、每项30秒、4并发：TRUE18、UNKNOWN59、UNSUPPORTED60、ERROR2、
TIMEOUT1；有Unsound诊断的结果明确不能作为SAFE证据。相对旧148项池，sleep/exit
覆盖移除10项，句柄TOP别名新增2项；新两项均Goblint TRUE。source/config/binary
指纹及每任务命令见[新池](artifacts/ai-p0-2026-10-10/ptrbytes/goblint-current-safe-unknown.json)。

18个TRUE只表示后续核对入口，不保证移植后全部能成为TN。逐源码比较发现官方
SV-COMP2026规则规定函数名以`__VERIFIER_atomic_`开始时整个body为原子执行，
当前AI对acquire/release的额外排除与该合同不一致；BMC的named_verifier_atomic
反而按通用前缀实现。`29_conditionals_vs`只有共享m位于这两个函数体内，
与`40_barrier_vf-b`在函数体外仍有COND冲突不同；不能借后者的race标签排除前缀
合同。此项还在独立核实/实施，不能据TRUE数量推断算法收益。


### 9.22 精度：统一 verifier atomic 函数体合同（2026-10-11）

官方 [2027 rules](https://sv-comp.sosy-lab.org/2027/rules.php) 明确所有以
`__VERIFIER_atomic_` 开头的函数体不可交错，没有 acquire/release 的例外。
AI 分类器移除这两个名字的额外排除，并纠正将它们说成 caller section markers 的注释；
只有 begin/end 延续调用者的临界段，acquire/release 仅包裹本次函数体。
BMC 已按通用前缀实现，本轮没有更改 BMC 或 frontend。

充分条件仍是两条访问都有共同 MUST 伪锁；函数体外的普通访问不获得该锁。
缺失函数体继续使用已有未覆盖效果 gate，不从 atomicity 推出访问/值覆盖。
`29_conditionals_vs.i` 的全部共享 m 访问位于 acquire/release 内，因而由 UNKNOWN 恢复 SAFE；
`40_barrier_vf-b.i` 的 COND 竞争位于函数体外，全量保持原判定，不能根据其标签排除 API 合同。

build/install 一致；40 次 ILP32/LP64、AI/BMC、改名运行覆盖 static/external 前缀、
acquire/release、函数体外竞争、普通函数和缺失函数体。定向确定 FALSE 的执行均通过
独立 AR 检查；旧正确性76次、旧BMC20次通过。原六个参考用例已双ABI比较 Goblint/Dartagnan，
新增 acquire/release 外部竞争对照也得到 Goblint UNKNOWN 与 Dartagnan FAIL；Dartagnan
直接引擎的 exit13 是本次 FAIL 的退出码，不把它误记为运行错误。

完整顺序1029扫描：AI TN653→654 / UNKNOWN375→374；BMC TP104→104、TN653→654、
UNKNOWN271→270，FP/FN0、error1（同一 nsc-ircc timeout）。唯一逐任务变化是上述
conditionals 的 UNKNOWN→SAFE，未静默替换其他成功任务。有限零FN不能证明所有SAFE过滤；
本轮也没有重新导出全部104项执行或测量速度提升。

证据：[汇总](artifacts/ai-precision-2026-10-11/atomic-prefix/summary.json)、
[逐任务配对](artifacts/ai-precision-2026-10-11/atomic-prefix/paired.json)、
[40次用例](artifacts/ai-precision-2026-10-11/atomic-prefix/fixtures-after.json)、
[API合同](artifacts/ai-precision-2026-10-11/atomic-prefix/contracts.json)、
[参考输出](artifacts/ai-precision-2026-10-11/atomic-prefix/reference.json)。


### 9.23 官方 Goblint portfolio 校准与后续入口（2026-10-11）

核实父目录原 binary、隔离校准 binary 和官方
[2027训练表](https://sv-comp.sosy-lab.org/2027/results/results-verified/C.no-data-race.Concurrency.table.html)
均为 `tags/svcomp26-0-gbb55b70d9`（源码commit `bb55b70d91290992629ba3d073185c10cdf356e6`）。
官方结果XML记录选项 `--portfolio-conf conf/svcomp26/seq.txt`、900秒、3000MB、1核，
1030任务的TRUE/TN为714、FALSE/TP为0；不是官网已经切换了一个新的 svcomp27 算法。
官网比本地1029清单多一个 `safestack_relacy_racy.i`，它为UNKNOWN；公共任务名/标签一致。
未逐一比较官网下载输入内容，也未在本地按官方资源全量复现714。

此前§9.21的18个TRUE只覆盖IKOS的140个safe-label UNKNOWN，且使用 `ldv-races.json`，
不是Goblint全量TN，也不是其官方配置的能力上限。本轮重新运行全部140项，source hash
与旧池逐项一致，并增加YAML hash、模型与原生判定记录。使用同一校准binary和上游runner，
仅将六阶段配置文件路径改成绝对路径，隔离任务工作目录；每项60秒、4并发。
结果为正常TRUE46、UNKNOWN25、UNSUPPORTED33、TIMEOUT34、ERROR2。
原生TRUE共63，但其中17项含Unsound，不能直接作为参考安全证明。超时的历史unknown输出
也不能当成该次最终完成的UNKNOWN；未发现runner停止None却沿用上一阶段判定的误统计。
这是本地参考池校准，时限和资源仍与官方不同。

§9.22已经恢复其中一个atomic用例，因此当前139个safe-label UNKNOWN的参考分布为：
**正常TRUE45 / UNKNOWN25 / UNSUPPORTED33 / TIMEOUT34 / ERROR2**。
45个TRUE仅表示可以继续核对源码的入口，没有承诺45个都能移植成TN。
台账的research_area是阅读路线假设，不是已经核实的根因或“IKOS不能实现”结论：
条件数据/锁对应7、escape/region16、protected值不变量2、条件锁2、句柄/实例join4、
container指针/锁对应7、condition/helper控制流1、库buffer/发布2、transitive join2、
atomic/thread-local覆盖2。

本轮开始具体研究全局句柄：最小程序 `create(&handle); join(handle); data=2` 在两个ABI
均为UNKNOWN，Goblint官方portfolio均正常TRUE，Dartagnan固定引擎均PASS（有界PASS
只是对照，不当作无界证明）。调试输出显示静态初始化0发布到整数干扰board；create的
site token只写本地memory，后续global load合并0而丢失单例，报告中joined_threads为空。
LDV join文件还涉及条件创建、helper返回、retval等，不能把此最小观察声称为所有文件的唯一根因。
后续须区分初始化和真正并发写，覆盖所有handle写来源（包括join输出别名、字节/未知写），
保留本地alternative，尊重宽度/offset/实例唯一性；不能重新用区间端点生成MUST join。
此诊断尚未实现恢复规则。

证据：[参考池汇总](artifacts/ai-precision-2026-10-11/goblint-portfolio/summary.json)、
[全部140项](artifacts/ai-precision-2026-10-11/goblint-portfolio/results.json)、
[句柄最小输入](artifacts/ai-precision-2026-10-11/goblint-portfolio/pending-global-handle.c)、
[实际观测与义务](artifacts/ai-precision-2026-10-11/goblint-portfolio/pending-global-handle.json)、
[双ABI参考](artifacts/ai-precision-2026-10-11/goblint-portfolio/pending-global-handle-reference.json)。
原始日志/DB留在临时目录，不纳入Git。
