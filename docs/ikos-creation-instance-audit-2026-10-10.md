# 创建过滤必须区分父线程的全部动态实例

SAFE审查发现新的FN，位于 `DataRaceChecker` 的 `creates_before` 规则。
它根据父线程入口名称和当前访问的may-spawned摘要，认为“父访问尚未创建子线程”
就先于该子线程的所有访问。但另一个父实例可能已经创建了仍在运行的子线程。

最小执行：main创建两个parent；parent各自持同一锁递增data，然后创建child；
child不持锁读取data。先执行parent A的写和创建，再让child A的读与parent B
创建前的写竞争。父写之间受锁保护，子读之间不竞争；旧规则正好删除真正的跨实例对。
双架构下旧分析器均报SAFE，固定版本Dartagnan均FAIL/exit13。

“父线程同时最多一个实例”仍不足：main创建parent A并join后再创建parent B，
parent A退出并不等于它创建的child A退出。顺序父实例变体仍竞争，第一版仅检查
`is_unique_thread` 时仍报SAFE，因而未提交；对应未完成全量已中止。

最终补丁复用现有实例注册表，预先收集“创建点均至多执行一次、实例集合大小为1”的
父入口，并保留main。只对这个集合应用原创建过滤，其他原有前提仍须成立。
调用/循环重复性来自 `register_thread_func` 和 `thread_instances_are_static`，
没有改动前端、BMC取值或同步编码，也没有按文件名分支。
这证明的是该条规则新增的实例前提；不是整个SAFE分析的正确性证明。

## 验证

七个fixture：直接双父、循环创建、helper与改名、传递创建、顺序父实例；
单父和子访问共同持锁两个安全对照。双架构/default及BMC共28条回归通过。
五个竞争用例由SAFE转为default警告/BMC UNKNOWN，不再漏报；两个对照仍SAFE。
参考14条查询逐项核对FAIL/exit13或PASS/exit0；不是把接口OTHER当作SAFE。
原20条双架构P0观察逐项相同，既有single-execution回归通过。

build成功、install同步/cmp一致。冻结集ILP32全量1029项逐文件配对：
**TP77→78 / FP0→0 / TN697→697 / FN0→0 / UNKNOWN255→254 / error0→0**。
唯一全量转移为 `23_lu-fig2.fixed.i` UNKNOWN→FALSE；隔离复测旧/新二进制均FALSE，
诊断计数相同，因此不把这个+1归因于创建过滤修复，也不声称稳定精度提升。模型尚待独立检查，
这不是全部反例已经验证的声明，也未完成LP64全量或稳定性能归因。
[证据](artifacts/creation-instance-2026-10-10/summary.json)、
[逐文件配对](artifacts/creation-instance-2026-10-10/paired.json)。

SAFE其他规则仍需审查。新竞争fixture在BMC中仍UNKNOWN，原因涉及实际嵌套创建
及多线程依赖；正确性修复没有强行把它们变成确认竞争。
