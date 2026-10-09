# 当前 UNKNOWN 台账与历史证据衔接

在 `3806ea4` 基线上，冻结集 TP61/TN697/UNKNOWN271，FP/FN/error0。
此前296条台账已经过期：若直接拿历史 blocker 排序，会把已经恢复的创建/数组问题继续
当成当前问题，并漏掉stdio修复新增的四项UNKNOWN。本次按任务YAML和输入哈希重新对齐。

[逐任务记录](artifacts/bmc-current-ledger-2026-10-09/results.json)共300条：全部当前271项
UNKNOWN，另保留历史恢复项。对271项全部重新运行当前诊断，结果仍全部UNKNOWN，
每项均有最新候选数量、观察到的阻塞原因及最多四条AR例子；阻塞存在重叠和遮挡，
不是可恢复TP数量。历史义务显式标为历史，不能当作新诊断。

继承的参考结果只在源文件哈希一致时复用，标记未重跑。stdio新增四项重新跑固定参考
launcher，均为ERROR：内部日志有FAIL，但最终 Result: ERROR / UnsupportedOperationException /
Violation found for unsupported property。它们不能归为参考UNKNOWN或已确认竞争；
特别是预期SAFE任务不能借内部FAIL导出任何新TP结论。

该台账区分参考FAIL、PASS、UNKNOWN、TIMEOUT、ERROR及标签分歧。仍保留可继续推进的
参考竞争任务；没有把某个blocker或一次有界失败解释为“IKOS不能实现”。完整反例验证
未完成的标注也继续保留。[汇总](artifacts/bmc-current-ledger-2026-10-09/summary.json)固定
版本、二进制、逐任务范围和复用限制。原日志位于 `/tmp/ikos-ledger-current-2026-10-09/`。

下一阶段先关闭已复现的同步家族库输出遗漏，再按最新台账选择完整atomic目标闭包、
具体对象/指针来源、线程激活或其他共享依赖的通用增量。更新台账不能替代实现这些语义，
当前也未达到“所有可行恢复都已用尽”的结束节点。
