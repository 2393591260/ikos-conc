# IKOS-ConC：阅读入口与当前状态

这里维护并发扩展的六份文档。原生 IKOS 的安装、数值域和模块说明仍在
[项目 README](../README.md)、[原生概览](../doc/OVERVIEW.md)及各模块 README。

## 从哪里开始

| 你要了解什么 | 阅读入口 |
| --- | --- |
| 项目怎样从 C 程序得到 SAFE / FALSE / UNKNOWN | [整体架构](concurrency-architecture.md#pipeline)，再看贯穿示例 |
| 抽象解释怎样覆盖线程干扰、证明安全 | [AI 手册](ikos-ai-complete.md#principles)，按需读线程、对象、锁与正确性合同 |
| BMC 怎样利用 IKOS 信息确认真实竞争 | [BMC 手册](ikos-bmc-complete.md#architecture)，再看值流与执行检查 |
| 修改代码、构建、调试和验证 | [开发速查](dev-gotchas.md#workflow) |
| 注册、包装、运行及 witness 外部验收 | [参赛指南](SVCOMP2027-参赛指南.md) |
| 已实现与待研究的双向反馈、论文证据缺口 | [AI/BMC 协同](ikos-ai-complete.md#feedback) |

理解代码先读整体架构；深入某个问题时只读 AI 或 BMC 的对应章节。
手册按“原理 → 实现位置 → 前提与失败回退 → 证据 → 下一步”组织，
无需先读完历史实验。

<a id="current"></a>
<a id="baseline"></a>
## 当前可复核基线

以下是 **2026-10-11 已保存的实验**，不是本次文档整理重新运行的结果。
分析器代码状态为 `a2623c3b1f0934a0c095e116b9d944288a4a470f`；
安装二进制 SHA256 为
`b59ea7fc87408a695d0ee712ad08878ada0ccd6b3c46768d8b265c9969553e02`。

任务集合为 `/home/ruan/sv-benchmarks-2026`，revision
`0a254fdddcfe24949be9edd47f8d609462eb52fb`，本地扫描 1029 项、全部 ILP32，
4 jobs、每项外层 30 秒。2027 官方目标 manifest 尚未固定；
定向 LP64 回归不能称为 LP64 全量。

| 比赛判定模式 | TP | TN | FP | FN | UNKNOWN | error/timeout |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| AI，demote，BMC 关闭 | 0 | 678 | 0 | 0 | 350 | 1 |
| AI + BMC，demote | 109 | 678 | 0 | 0 | 241 | 1 |

109 个标签 TP 对应重新导出的 137 条所选执行，全部独立 AR ACCEPT。
检查范围为导出的 AR、可信前端与明确库合同；尚非官方源程序/witness 验收，
不认证全部 SAFE 规则，也不穷尽所有 SAT 模型。
有限基准 FP/FN 为零不能作为全语言 soundness 定理。

证据：[汇总](artifacts/ai-precision-2026-10-11/diagnostic-types/summary.json)、
[逐任务配对](artifacts/ai-precision-2026-10-11/diagnostic-types/paired.json)、
[逐执行检查](artifacts/ai-precision-2026-10-11/diagnostic-types/current-tp-executions.json)。
唯一失败项是安全标签的 `nsc-ircc` 驱动外层超时，未计成 UNKNOWN。

## 下一步去哪里查

- 安全 UNKNOWN 115 项，24 项有正常 Goblint TRUE 参考，91 项参考未证实。
  [安全台账](artifacts/ai-precision-2026-10-11/unknown-ledger/ledger.json)保存状态及历史诊断指纹。
  TN 工作入口为 [AI 后续机制](ikos-ai-complete.md#roadmap)。
- 竞争 UNKNOWN 126 项，历史 Dartagnan 参考为 FAIL 95、TIMEOUT 15、ERROR 13、有界 PASS 3。
  [混合历史台账](artifacts/bmc-v1-final-2026-10-10/unknown-results.json)同时含安全和竞争任务，
  看 `current_bmc`/`current_ai` 字段及标签筛选，不能把全部 227 行称为竞争池或新参考运行。
  TP 工作入口为 [BMC 后续机制](ikos-bmc-complete.md#roadmap)。
- 已有 AI→BMC 的能力及局部查询减少证据；真正 BMC→AI 归纳精化、
  整体优于参考工具及大幅提速仍需验证。

## 文档与证据的维护边界

| 内容 | 唯一维护位置 |
| --- | --- |
| 最新实验基线和总数 | 本页 `current`；原始依据在对应 JSON |
| 全流程、模块职责、上游扩展位置 | 整体架构 |
| SAFE、干扰、线程摘要及反向反馈义务 | AI 手册 |
| FALSE、SMT、所选执行及 BMC 扩展 | BMC 手册 |
| 构建、调试、回归、提交纪律 | 开发速查 |
| 外部赛事规则和发布状态 | 参赛指南，注明实际核验日期 |

实现状态写成“已实现 / 保守封堵 / 待实现 / 待验证”；新修复更新机制所在章节，
不再新增日期化 Markdown 或在正文追加整轮流水账。
历史实验必须有版本、输入、配置与证据范围；撤回的机制和零收益实验也保留简短结论。
跨手册使用链接，避免反复复制“最新数字”。

`review-repros/`、小型 `artifacts/`、必要 CSV 和 UNKNOWN 台账继续保留；
原始大日志、数据库和二进制不作为阅读入口。旧报告正文可从 Git 历史取回：

```bash
git show a2623c3:docs/ikos-ai-optimization-plan-2026-10-10.md
git show a2623c3:docs/archive/ikos-5b-mutex-hb-soundness.md
```

这些原文是历史材料，其结论需要对应版本和实际语义依据。
