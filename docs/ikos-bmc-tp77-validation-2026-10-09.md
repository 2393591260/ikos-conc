# 当前77项标签TP的验证覆盖

以 `79e4ee8` 对应的既有分析器重新诊断77项标签TP，全部仍FALSE，保存95个候选。
二进制hash及每个输入/report的hash见
[导出清单](artifacts/bmc-tp77-validation-2026-10-09/export-summary.json)。
本轮只改研究验证工具，未改前端或分析器；没有把局部复测当成1029项新全量结果。

## 固定界限2的全部候选检查

77任务/95候选全部检查：SAT68、UNMAPPED20、UNSUPPORTED2、
UNSAT_AT_BOUND2、TIMEOUT3。详情见
[逐任务记录](artifacts/bmc-tp77-validation-2026-10-09/initial-fixed-memory.json)。
这些是PARTIAL_FIXED_MEMORY_MODEL结果，不是完整反例验证统计。
动态事件合并、迭代界限、未知静态起点以及混合对象宽度都没有强行选取对应事件。

两个不满足候选分别是fmaxsymopt和thread-join-counter-outer-race。
前者IKOS路径执行六轮初始化，界限2不足以覆盖；后者保存的threads_alive=15
来自十五次创建，界限2同样不足。扩大界限是检查覆盖，不是修改标签。
fmaxsymopt界限6得到SAT；后者界限15参考运行60秒超时，没有导出IR，保持未映射。
所有首次不满足和重试观察均保留，不能从有界不满足推出IKOS反例错误。
[重试证据](artifacts/bmc-tp77-validation-2026-10-09/selected-retries.json)。
原始fmaxsymopt源码查询日志实际为UNKNOWN、exit1（Not fully unrolled loops），
首轮接口误记ERROR。固定版本ExitCode.java明确BOUNDED_RESULT=1、UNKNOWN_ERROR=30；
后续分类已修复并通过11项协议断言及该真实日志复查。旧统计保留为历史观察，
后续不得把界限不足当成工具错误；内部FAIL后launcher报错仍不得算参考竞争。

## 重复内存访问不能塌缩为一个事件

映射增加memory_occurrence：只有同源码位置的AR静态指令唯一、参考展开前身份唯一，
才绑定动态访问次数。参考端检查前向CFG、同读写类别和同静态来源，并要求全部前序
克隆执行。因此遇到跳过迭代时，不会把第二个展开克隆当成第二次实际访问。
静态来源歧义或参考界限不足继续拒绝/显式遗漏；仍不声称绑定了任意完整CFG。

每次AR结果重新定义时先清掉旧的模型值，避免当前未导出值沿用前一次helper/循环结果。
双架构60项参考校准全部通过，另有19项映射断言。新增控制覆盖合法重复访问、错误次数、
错误值、被跳过的访问以及不同静态来源。见
[校准记录](artifacts/bmc-tp77-validation-2026-10-09/memory-calibration.json)。

fmaxsymopt现在绑定15个全局访问，包括六次初始化写和两侧重复数组读，界限6查询SAT；
修改一个固定值后UNSAT_AT_BOUND。此前只绑定5个访问时，改值仍可能SAT，说明
遗漏初始化会允许参考自行补选数据；不能把这种检查宣称为完整验证。

## 仍未闭环的证据

局部nondet取值、全部CFG/调用实例、同步历史和IKOS总调度/rf仍未全部绑定。
独立验证数量没有升格。接下来补这些证据、扩大无法覆盖的参考界限并检查剩余映射失败，
随后继续UNKNOWN的对象/指针、线程依赖闭包、join和公平搜索；本次提交不是结束节点。
原始report、DB删除后的日志、编译类和全部运行记录在
`/tmp/ikos-tp77-validation-2026-10-09-a/`，git中仅保留小证据。
