# TP104 后的逐任务台账（2026-10-10）

`48d36a2` 冻结集全量 TP104/FP0/TN696/FN0/UNKNOWN229/error0。
229 项剩余 UNKNOWN 已用最终安装二进制重新诊断，逐项仍 UNKNOWN，错误0。
301 条历史记录全部核对源码哈希；本台账含剩余229、恢复后69项标签TP和
3项抽象 SAFE，不是104项TP的完整验证清单。
[逐项记录](artifacts/bmc-tp104-ledger-2026-10-10/results.json)、
[统计](artifacts/bmc-tp104-ledger-2026-10-10/summary.json)。

剩余参考 FAIL101、TIMEOUT73、ERROR25、PASS29、UNKNOWN1。
除本轮两个恢复用例、malloc_struct 及共享回调双架构新跑外，沿用哈希一致的历史参考；不能称为
229次新的参考分析。恢复记录已去掉旧 blocker，附实际恢复证据，未独立
验证状态保留。历史 TP98/TP102 的实验记录不被改写。

诊断中的阶段计数可重叠，不是可恢复TP收益或不能实现的证明。仍须按
具体代码确认值/地址/动态对象/生命周期前提，不能为了提高得分封自由变量
后又任意放宽。结构体指针的纯 cast 来源已扩展，冻结集逐项不变，见
[指针槽报告](ikos-bmc-pointer-slot-casts-2026-10-10.md)。共享回调的新参考探针与实施范围见
[可行性分析](ikos-bmc-shared-callback-feasibility-2026-10-10.md)，尚未实现。后续还有
更一般分配/创建实例和SC供值，但先检查实际阻塞点与正反变体。

正确性证据仍有边界：完整独立 witness 验证、全部 SAFE 规则及完整C/LLVM
definedness 证明、稳定全局性能比较尚未完成。本轮新复现的 owner 字段
假反例已修复，不能从冻结集 FP/FN 零推导没有其他正确性问题。

需要更正继承的待办描述：`mark_internal_inline` 隔离 A/B 已在
[独立报告](ikos-inline-ab-2026-10-09.md)完成；同管线1029项逐项一致，
未启用 inline-all，已排除移除 NoInline 这一行导致历史11项TN下降。
这不证明其他历史改动的归因已完成，也不证明剩余TN回退已全部恢复。
部分宽度干扰 SAFE 变体的漏报也已在后续独立提交修复，见
[宽度审查](ikos-interference-width-audit-2026-10-10.md)。

最终原始诊断在 `/tmp/ikos-recovery-2026-10-10/pointer-slot-casts/unknown-diagnostics/`；
初轮诊断保留在 `/tmp/ikos-recovery-2026-10-10/tp104-unknown-diagnostics/`，
不进 Git；结论 JSON 各小于1MB。后续继续按独立行为阶段验证并提交。
