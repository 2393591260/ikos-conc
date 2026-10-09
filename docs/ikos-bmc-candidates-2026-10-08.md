# BMC 阻塞诊断、多候选搜索与线程激活审查

本轮实现了结构化诊断、构造期间的展开预算、组内多候选搜索及真实确认对的报告/witness 透传。
审查还发现了冻结集之外的线程激活 FP，因而补充保守 gate。
最终结果是 **TP 53→27，TN=709、FP=0、FN=0、超时=1 不变，UNKNOWN 266→292**。
这次没有实现净得分提升：新增 `scull` 的确认，但撤回了 27 个缺少创建/实例存在性证据的旧确认。
撤回不表示这些标注为 racy 的任务没有竞争，也不表示它们全是假反例。

## 为什么必须检查线程激活

抽象解释的 may-multiplicity 可用于保守地产生候选，不能用于证明两个具体实例存在。
下面三个 race-free 对照在本轮开始时保存的地址 A1 二进制中，ILP32/LP64 都被误报 FALSE：

1. nondet `n` 被限制在 `[0,1]`，循环创建 `n` 个只写全局变量的线程。
2. if/else 的两个分支分别创建同一个入口函数，任何执行只有一个线程。
3. if/else 分别创建两个不同入口，两个入口都有无锁写，但没有执行会同时创建它们。

新版均返回 UNKNOWN。样例生成和断言在 `analyzer/test/regression/bmc-candidates/runtest`，
旧/新二进制的 12 条独立观察保存在配套 JSON 中。
这说明此前冻结集 FP=0 是有限观察，不能证明 BMC 已满足完整 soundness 契约。

目前激活前提的支持范围：

- 两个非 main 入口：相关显式创建站点必须位于 main；当前只接受无分支、无等待/未知调用的直线创建前缀。
  同入口需要两次显式创建；不同入口各需一次。为保持 witness 的站点身份，额外/其他创建上下文保守不确认。
  已识别 nondet intrinsic 和少量 pthread 初始化/detach 调用可出现在此前缀。
- 一端为 main：选中的 main 展开路径必须实际包含对方入口的 Create 事件。
- 创建循环、条件和一般 helper 上下文尚未建模。缺少前提一律保持 UNKNOWN，不能用 may-spawn 放行。

这是一项较强的暂时能力边界。消除精度损失需要建模创建前缀与动态实例；不能把此 gate 当成永久设计限制。

## 实现与预算

- `info.bmc`：由 `IKOS_BMC_DIAGNOSTICS=1` 开启，包含各候选的编号、原因计数、首次记录的阻塞、路径数量及 AR/源码示例。
  独立的 checker 前提同时记录；被前置 gate 阻断后，没有执行的下游阶段不推断原因。
  每候选至多 16 个语句示例，每组保留前 256 个候选详情，组原因计数覆盖所有候选。
  未命名 AR 变量的 dump 可能包含进程内地址，不适合作为跨运行身份。
- 每组最多尝试 32 对，全局最多 256 对、1024 次路径编码查询，搜索预算 10 秒；每次 SMT 检查最多 1000 ms。
  这些是内部搜索预算，不是整个分析器/前端的硬限时。
- 展开构造期间检查路径 256、事件 1024、DFS 步数 16384、栈深 256；CFG 头收集也检查预算并使用 visited。
  超限丢弃部分路径、保留 UNKNOWN。DFS 栈按路径复制，回边不修改调用方的栈。
- 每对重新检查同步、原子混合、别名、偏移、线程入口、隔代排序和激活前提；偏移预筛使用精确整数。
  正常 race 模式关闭恢复和诊断时，不执行新候选搜索。
- 确认后的报告位置、flat `access_a/access_b` 与 `confirmed_pair_id` 指向实际成功的一对。
  同入口的两个已证明实例在 witness 中使用两个不同 thread_id。

首轮发现某个大组耗尽全局候选预算、饿死后续组，所以加入每组配额。
独立回归使用大量不可展开的候选和另一对象上的直接竞争，检查后者仍能被确认。
预算仍可能损失 TP；诊断分别记录组预算和全局预算，不能将未搜索解释为 UNSAT。

文档所记 `workstealqueue_mutex-2` 历史 null-deref 本轮未复现。
正常新版及隔离 ASAN 构建（恢复旧的共享栈传参）均返回 UNKNOWN，无 ASAN 报错。
因此仅报告当前不崩溃及路径栈状态修正，不宣称已经证明历史崩溃的具体根因。

## 可复查的诊断

```bash
python3 -B scripts/bmc_diagnostics.py --output /tmp/ikos-bmc-diagnostics-new
# 单任务，使用 YAML 的预期结论和 data_model：
python3 -B scripts/bmc_diagnostics.py \
  --input /home/ruan/sv-benchmarks-2026/c/pthread-atomic/scull.i \
  --output /tmp/ikos-bmc-scull-new --jobs 1
```

输出目录必须是新目录；保留每任务的完整日志、JSON 报告和命令，汇总写入 `summary.json`、`results.json`。
超时会杀掉该任务的进程组。任务人口复用 `svcomp_race_test.py` 的既有扫描，不宣称已经审计官方类别的全部包含/排除规则。

最终 UNKNOWN 中观察到的部分原因如下，列间、行间可重叠：

| 观察到的原因 | 预期 racy 任务 | 预期 safe 任务 |
|---|---:|---:|
| 线程激活/实例前提不足 | 128 | 61 |
| 循环或相关同步未完整展开 | 54 | 11 |
| 非全局对象 | 45 | 28 |
| 未建模同步 | 45 | 33 |
| assume 值不支持 | 16 | 0 |
| points-to 非 singleton | 13 | 9 |
| 实际遇到的 load 值不支持 | 8 | 0 |

这些不是互斥分桶，不是可恢复 TP 数。激活 gate 会遮住后续值流问题，不能据此说 rf/co 只影响 8 个任务。
下轮应优先补创建前缀、动态实例和 witness 身份，再重新测量剩余值、地址及循环阻塞。

## 验证与实际覆盖

数据集 `/home/ruan/sv-benchmarks-2026`，版本 `0a254fdddcfe24949be9edd47f8d609462eb52fb`。
源码起点 `51b795c4b09f36170a69be4e5ce0a10345db2de6`，加此前地址 A1 和本轮未提交修改。
保存旧二进制，再构建并原子同步新二进制到 install；二者 SHA256 记录在配套 JSON。

```bash
cmake --build build --target ikos-analyzer -j4
IKOS_BMC_RECOVER=1 python3 -B svcomp_race_test.py --demote
python3 -B analyzer/test/regression/bmc-candidates/runtest
python3 -B analyzer/test/regression/bmc-address/runtest
python3 -B docs/review-repros/bmc-2026-10-08/run.py --bits 32
python3 -B docs/review-repros/bmc-2026-10-08/run.py --bits 64
```

- 正常模式 A/B：同一 1029 任务、30 秒 wall timeout、8 jobs；FP/FN 均为 0。
  唯一新增确认是 `scull`；27 个旧 FALSE→UNKNOWN。全部 28 个变化及全部配对任务已留档。
- 最终独立诊断扫描与正常输出模式的 1029 个 verdict 全部一致：TP=27、TN=709、UNKNOWN=292、超时=1。
  既有超时仍是 `linux-3.14--drivers--net--irda--nsc-ircc.ko.cil.i`。
- 新候选/激活/预算回归 30 项、地址回归 28 项，均覆盖 32/64 位并断言通过；P0 runner 的 20 条观察另行核对。
  覆盖真实确认对/witness 目标一致性、两个实例的 witness ID、三种创建安全对照、串行 join、路径/事件/工作预算及组公平性。
- 全量既有扫描全部为 ILP32；LP64 仅覆盖上述定向回归。
- 新增 `scull` 经实际 `benchexec/tools/ikos-conc.py` adapter 返回 FALSE(no-data-race) 并生成 witness；这不等于独立验证成功。
- 启动基线时曾发生 argv 转发错误，该轮作废；并发扫描造成额外超时的一轮也作废。
  正文统计来自修正参数后、最终 A/B 各自运行的结果。

本地 CPAchecker 4.2.2 独立验证尝试失败：配置补全后，其 `AutomatonWitnessV2d0Parser` 对当前
YAML 2.2 `violation_sequence` 抛出 `NoSuchElementException`，没有得出验证结论。
不能把生成 witness 或本地目标一致性断言当成独立验证成功；打包 BenchExec 全流程也未验证。

配套材料：

- [聚合指标、逐任务变化和激活反例](artifacts/bmc-candidates-2026-10-08.json)
- [全部 1029 任务配对及观察到的原因](artifacts/bmc-candidates-2026-10-08.tsv)
- 本地完整日志、报告和隔离 ASAN 构建：`/tmp/ikos-bmc-pairs-k_deuhyo/`；该临时目录不随仓库提交。

这是对当前支持范围的改进与收紧，不是整个分析器的 soundness 证明。没有加入 benchmark 文件名或预期标签相关的语义规则。
