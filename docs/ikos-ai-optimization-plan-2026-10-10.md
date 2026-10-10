# IKOS-ConC 抽象解释：正确性修复与精度/性能优化计划

日期：2026-10-10。适用代码：`b3e6706`。**以下均为待批准方案，本轮没有实现。**
事实依据及复现输入见[正确性审查](ikos-ai-soundness-audit-2026-10-10.md)。
当前保存的冻结集基线是 `TP106/TN696/UNKNOWN227/FP0/FN0/error0`，
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
