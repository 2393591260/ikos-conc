# 提前删除当前 Join 查询必然 UNSAT 的前缀（2026-10-10）

冻结集1029项ILP32逐项不变：TP98/FP0/TN696/FN0/UNKNOWN235/error0。
这项改变只省去当前查询不可能确认的构造工作，不修改SAFE/抽象解释规则。

当前check_race对A的每个Join加 B.last < Join；没有动态句柄匹配，
是已知欠精度边界。若Join在A.target之前，po给出
B.target <= B.last < Join < A.target。普通目标的整数clock相邻要求不能
成立；atomic目标查询也禁止Join这个真实barrier夹在目标之间。
另一方向与该已有顺序直接矛盾。因而该查询必UNSAT，无需构造后续路径
及读源/守卫公式。展开到目标尚未出现时遇Join即可停止该DFS分支。

只有confirm_race的unroll_to启用该参数，默认展开器及启动/其它用途关闭。
目标早于Join的atomic闭合尾部不会被此门切掉。没有声称join别的线程会
同步当前目标，也没有将某条路径未找到竞争转成SAFE。
未来check_race按动态join句柄/多线程闭包建边时，必须同步收窄此剪枝到
实际被join的实例；不能把目前二线程过约束冻结为C/POSIX语义定律。

6条双架构断言：目标在join前竞争FALSE保留；真实join后对照SAFE；
join另一个线程后仍有真实竞争的三线程例保持UNKNOWN，体现当前Join
能力边界，不能因为数据集标签为race而任意补执行。
46条SC、34条atomic断言通过，P0双架构20观察与前阶段逐项相同。
build/install/cmp一致；没有独立验证计数或全局性能收益声称。

[配对](artifacts/bmc-join-query-prune-2026-10-10/paired.json)、
[统计与hash](artifacts/bmc-join-query-prune-2026-10-10/summary.json)、
[最小回归](artifacts/bmc-join-query-prune-2026-10-10/fixtures.json)。
原始日志在 `/tmp/ikos-recovery-2026-10-10/join-query-prune/`。
