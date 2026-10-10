# 保留暂缓候选的两轮搜索证据

两轮搜索原先只输出暂缓组第二轮的诊断，第一轮的值／同步、路径和SMT结果
被丢弃。qrcu全量与隔离报告因此可能只显示search_budget，掩盖此前已经执行
的查询；这些报告不能用来断言只要提高预算就能恢复。

本轮保留`first_pass_candidates`及两轮合并的aggregate counts，`candidates`
仍指最终轮，编号在各轮内部解释。诊断脚本统计首次阻塞时遍历两轮，不重复
累加aggregate。只有诊断开启时才保存第一轮JSON，没有改变候选、查询顺序、
预算、公式或verdict。被省略候选的aggregate仍保留，不能把有上限的例子看成
全部路径／全部证明。

新断言检查分支爆炸正例的第一轮证据确实存在，且没有省略候选时每轮计数
之和严格等于aggregate。4条双架构枚举断言、14条helper断言、20条P0观察
通过，P0与原版逐项一致。build/install/cmp成功。
1029项ILP32全量逐文件完全一致：TP80/FP0/TN697/FN0/UNKNOWN252/error0。
[汇总](artifacts/bmc-stage-diagnostics-2026-10-10/summary.json)、
[配对](artifacts/bmc-stage-diagnostics-2026-10-10/paired.json)。

隔离qrcu-1／2现在能看到各360次SMT UNSAT、2次展开work耗尽及第一轮其它
事实，而不是只看到第二轮的24条search_budget。它们仍UNKNOWN，不能把
有限查询UNSAT推成SAFE。07_rand隔离FALSE与全量UNKNOWN不同，依然只
记录配置／预算观察，不声称恢复TP；ticket的已确认结果保留。
完整独立反例验证没有增加。下一步应按恢复后的实际阻塞选择通用语义扩展。
