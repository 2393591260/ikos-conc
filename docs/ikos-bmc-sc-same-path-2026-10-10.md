# 同路径SC读源公式简化（2026-10-10）

与161d593配对，冻结集1029项逐项相同：TP86/FP0/TN697/FN0/UNKNOWN246/error0。
本阶段是公式优化，没有声称恢复新的TP或获得稳定全局提速。

对于常量偏移的global整型读，先运行原有两路径写源收集，核实所有可能
重叠写的宽度、整型类型和值来源。只有它们全部是本路径此前对精确同一区间
的写，才把一般SC“读最新写”的时钟析取简化成读值等于最后一写值；若两
前缀没有任何相关写，则使用已经证明的typed初值/startup值。保留转换规则。
po已要求此前的写严格按本路径顺序位于读前，故其最新写唯一，简化保留
当前公式的SAT集合。**这不是对象在整个程序中private的证明**，外围线程
闭包及初始化前提仍由已有检查负责。

对侧写、变量偏移继续使用一般rf；混合宽度、未知来源继续失败，不会只取
最后一写而跳过更早来源检查。失败向上传播，不能fallback复用失败读缓存。
heap/stack未扩展，没有frontend修改。参考SC定义来自固定Dartagnan的
`cat/sc.cat`：`acyclic po | (rf | fr | co)`；本阶段优化的是IKOS已存在的
有限SC读源公式，没有声称复现Dartagnan全部内存模型与生命周期语义。

build、install同步/cmp完成。16条双架构新断言、34条atomic及6条startup
断言通过，P0的20条观察相同。新断言还检查精确读实际走简化分支、对侧
写实际走一般rf。四个竞争变体双架构固定参考SOURCE_RACE，两个安全对照
双架构SOURCE_PASS；八个实际选中模型的部分内存检查SAT，完整验证数不增加。

`fib_unsafe-5-racy`定向仍UNKNOWN：同一10秒预算内旧观察59 UNSAT与1 SMT
UNKNOWN，新观察324 UNSAT且没有SMT UNKNOWN，7827次读使用简化公式。
这些计数只能说明该次运行的吞吐改善，不能换算为可恢复TP或全局速度收益。
后续继续核对实际候选路径、让已证明的确定值尽早参与守卫筛选。

[全量配对](artifacts/bmc-sc-same-path-2026-10-10/paired.json)、
[统计](artifacts/bmc-sc-same-path-2026-10-10/summary.json)、
[固定参考与部分模型](artifacts/bmc-sc-same-path-2026-10-10/reference-partial.json)。
