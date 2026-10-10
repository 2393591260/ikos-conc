# 提前判定atomic区间中间目标的查询UNSAT

1029项ILP32配对为**TP85→86 / FP0 / TN697 / FN0 / UNKNOWN247→246 /
error0**，唯一转移为`23_lu-fig2.fixed` UNKNOWN→FALSE；旧TP/TN全保留。
相比静态condition阶段TP84，两个小界限恢复及lu现在均保留。

优化的是现有IKOS查询，不是删除抽象候选或证明程序SAFE。具体前提：一个
目标位于实际闭合的verifier atomic区间，另一目标非atomic；该区间内目标
之前、之后各有一个被现有相邻查询视作排序障碍的实际事件。局部值/assume
及pseudo atomic标记不是这种障碍；普通内存和真实同步是。

证明直接来自现有公式：po给出`begin < before < target < after < end`，
atomic互斥要求另一个目标R在区间之外。如果`R < begin`，before就在R和
target之间；如果`end < R`，after就在target和R之间。两种情况均违反现有
查询的“目标间无内存/真实同步事件”约束，两方向都UNSAT。提前返回未确认
保持原公式的SAT集合；它不能证明源码没有竞争，也不能解决该查询的精度边界。
障碍判定函数与真正查询共享，避免两份定义漂移。没有改变SMT值/地址/读源。

lu的mThread候选位于atomic读计数与更新计数之间，旧版272个路径对产生544次
SMT UNSAT。新版用272次结构判定替代，随后仍确认start_main的实际竞争对。
隔离耗时约7.7→2.6秒；这是单文件观察，无稳定全局提速的结论。
同一实际选中start_main模型的参考部分约束SAT；同步、局部值、CFG/call
尚未完全绑定，完整独立验证计数不增加。

build、install同步/cmp、全量已完成。34条双架构atomic断言通过：区间中间
目标仍UNKNOWN，首个/最后一个内存目标及纯局部算术围绕目标均FALSE，安全
对照不确认；新增三个正例双架构固定参考均SOURCE_RACE。8条枚举断言通过，
P0的20条观察逐项相同。没有frontend改动。后续继续实际终止前缀、对象/
值流及未完成依赖闭包工作，不把这一阶段当作精度提升已经耗尽。

[全量配对](artifacts/bmc-atomic-interior-2026-10-10/paired.json)、
[统计与参考](artifacts/bmc-atomic-interior-2026-10-10/summary.json)、
[lu模型](artifacts/bmc-atomic-interior-2026-10-10/lu-partial.json)。
