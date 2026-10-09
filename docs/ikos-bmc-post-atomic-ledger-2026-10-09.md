# TP77基线后的完整UNKNOWN台账

`25e3a1e`冻结集基线：TP77/TN697/UNKNOWN255、FP/FN/error0。
全部255项UNKNOWN用同一install二进制重新诊断，仍为255项UNKNOWN，无错误；
当前观察与原300条历史任务记录衔接，保留源哈希、最新候选/阻塞/AR例子和历史义务。
逐条记录：[results.json](artifacts/bmc-post-atomic-ledger-2026-10-09/results.json)，
版本及汇总：[summary.json](artifacts/bmc-post-atomic-ledger-2026-10-09/summary.json)。

剩余255项：参考竞争117、超时82、错误25、PASS26、标签分歧4、UNKNOWN1。
参考结果没有全部重跑；只在源哈希一致时继承，fresh字段区分本次检查与历史证据。
恢复项包括本轮16个标签TP；airline-25的参考运行及多次附加约束检查仍超时，
不能把其标签TP写成参考确认。airline-20固定参考访问后SAT证明原参考查询存在竞争，
没有证明完整IKOS调度；其他未映射/部分检查也没有升格为完整验证。

所有UNKNOWN都尝试了本轮通用机制，但不代表逐个实现了它们缺少的全部语义。
剩余记录明确未满足的前提，而非把“当前未实现”写成“IKOS不可能实现”。
堆对象/多points-to、调用与地址来源、实际线程激活、完成路径及搜索预算仍有重叠。
仍需推进严格动态创建/调用映射和全模型验证，再根据实际候选做对象/依赖闭包、
join身份与公平搜索。SAFE规则证明也未完成，尚未达到全部精度手段耗尽的结束节点。

原诊断在 `/tmp/ikos-post-atomic-target-2026-10-09/`；此前TP67的265项诊断在
`/tmp/ikos-post-atomic-2026-10-09/`。小证据进git，原日志和DB没有提交。
