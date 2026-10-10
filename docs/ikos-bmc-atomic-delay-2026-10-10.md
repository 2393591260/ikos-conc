# 延后不可行路径的atomic约束构造（2026-10-10）

与79eec9e的1029项冻结集逐项相同：TP90/FP0/TN697/FN0/UNKNOWN242/error0。
本阶段没有新增TP；保留本轮恢复的四个fib与所有旧TP/TN。

atomic区间与对侧每个事件之间的排斥约束，移动到实际守卫和完整算术检查
成功之后再添加。内容、循环范围和查询条件完全相同；只让已经拒绝的路径
不再构造这些约束，不能让可行路径失去atomic不可分割性。
没有frontend修改、增加超时或改变IKOS的SAFE判定规则。

build、install同步/cmp完成，26条双架构读源/守卫、34条atomic断言通过，
P0的20条观察相同。定向四个fib依次从约6.06/7.73/7.44/9.52秒降至
3.81/4.97/3.67/6.16秒，均FALSE。这是单次隔离A/B观察，不是稳定全局提速。

本阶段`fib_unsafe-5-racy`的两个ILP32所选模型，在固定Dartagnan bound13
部分内存约束检查中SAT（约19.1/23.8秒），各绑定70个内存事件；改变实际
store值的探针均UNSAT_AT_BOUND。所需第12次循环访问全部映射，不以不存在
的事件补齐模型。此前bound16的三个模型超时仍保留在上一阶段证据中。
LP64新模型的同界限检查仍在运行。减少参考界限只减少额外搜索；此处SAT
仍只是所绑定条件在参考模型中可实现，不是整个IKOS反例已独立验证。
未绑定CFG/call、局部值及同步历史等缺口继续保留，完整验证数不增加。

[全量配对](artifacts/bmc-atomic-delay-2026-10-10/paired.json)、
[统计与定向A/B](artifacts/bmc-atomic-delay-2026-10-10/summary.json)、
[fib5 ILP32部分验证](artifacts/bmc-atomic-delay-2026-10-10/fib-32-partial.json)。
