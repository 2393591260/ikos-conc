# TP77 已映射内存顺序的检查结果

暂停后运行环境的 `/tmp` 已被清空，旧进程和未提交的原始报告无法续跑。
按保存的77任务清单重建报告，输入hash逐项核对，二进制hash仍为
`ee73d3d65be88b75e53d0e045247761cabb32a47115324ffc093b020078ac696`
（分析器行为提交 `91e8024`），全部仍FALSE，95个候选。
固定参考源码及Java编译器在隔离目录恢复，源码锚点hash检查通过；
研究适配器使用 `99e8386`，没有改变前端或分析器。

界限2、60秒、两并发，全部77任务/95候选的部分固定模型检查结果：
**SAT83 / UNMAPPED7 / UNSUPPORTED2 / UNSAT_AT_BOUND1 / TIMEOUT2**。
不能把83个SAT视为83个完整反例，也不能把这些候选数当作任务TP数。
[逐候选记录](artifacts/bmc-schedule-validation-2026-10-10/fixed-memory.json)、
[重新导出指纹](artifacts/bmc-schedule-validation-2026-10-10/export-summary.json)。

- fmaxsymopt界限2不满足，界限6固定15个访问、局部值、rf/co及13条内存顺序后SAT；
  修改固定值得到UNSAT_AT_BOUND。这是界限覆盖问题的证据，不是用界限2否定模型。
  [重试](artifacts/bmc-schedule-validation-2026-10-10/fmax-bound6.json)。
- counter-inner/outer的新短前缀均SAT；旧15次创建的模型仍未验证。
- airline-15/20的固定查询超时，airline-25的源码查询超时、未导出IR，保持未映射。
  负载与完整关系编码均可能影响耗时，未建立稳定的性能归因。
- funloop-simple/hard1存在访问次数超过界限；另外四个chosen-target映射失败，
  原因包括间接地址和优化后的来源映射，不能强选事件。
- sound-base/funptr两项研究接口返回UNSUPPORTED，保留原原因及日志指针。

相比先前通信检查，SAT没有转为不满足；两项airline转超时，一项fkp转SAT，
两个counter换成实际更短模型后SAT。不能把模型替换与求解负载差异归因于
单一顺序接口增量，也不能声称整体提速。

CFG/调用实例、完整局部值、同步历史及完整调度仍未全部绑定，
`full_counterexample_validated:false` 保持不变。没有重复1029项行为测试，
TP77/FP0/TN697/FN0/UNKNOWN255/error0引用既有分析器全量基线。
本报告记录的是发现并修复新的创建过滤漏报之前的模型集合；
后续分析器修改的结果必须另行配对。
