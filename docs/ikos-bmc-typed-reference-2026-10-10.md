# 严格参考映射中的实际内存类型（2026-10-10）

分析器与8baf02b二进制相同，冻结集仍为TP92/FP0/TN697/FN0/UNKNOWN240/error0。
本阶段仅修改研究验证适配器，没有新增标签TP，不重复分析器build/全量。
不修改固定参考安装，Java hook仍在临时目录编译。

`04-mutex_37-indirect_rc`此前UNMAPPED：`(*g1)++`同一源码行包含pointer
load与整数load，按行/访问种类不能证明是哪一次静态访问。现在descriptor
增加参考MemoryCoreEvent实际IntegerType位宽，映射时结合AR整数类型及实际
访问字节宽度；AR同源码位置的静态站点也按类型区分。循环clone检查仍要求
同类型同源码位置只有一个UnrollingId，未知/多个同类型来源仍拒绝。
这是类型/来源核实，不选择任意一个“看起来像”的load。

Dat3M pointer存储在ILP32下仍与IKOS ABI不同，32位pointer访问明确遗漏；
不能把4字节槽绑定成参考8字节访问。LP64的global pointer访问仍可映射，
heap的pointer存储/值绑定继续明确遗漏。对整数类型与访问宽度不一致的
输入提前拒绝；同类型重复站点、未知clone来源、错误位宽等负向测试通过。

当前二进制重新导出的37模型，ILP32绑定3个、LP64绑定7个内存事件，
双架构部分约束均SAT；修改实际store值均UNSAT_AT_BOUND。旧88条参考校准、
26条heap断言、16条新类型映射断言与8条原映射断言全部通过。旧UNKNOWN/
UNMAPPED证据保留，没有用新结果覆盖历史。

**仍非完整验证**：CFG/call、同步历史、局部值和总调度等未全部绑定。
32/64不同的映射覆盖是明确的ABI边界，不能宣称已经实现pointer rf、完整
C生命周期验证或正式witness 2.2验证。下一步继续按缺口逐项补齐。

[校准](artifacts/bmc-typed-reference-2026-10-10/calibration.json)、
[heap校准](artifacts/bmc-typed-reference-2026-10-10/heap-calibration.json)、
[37 ILP32](artifacts/bmc-typed-reference-2026-10-10/37-32-partial.json)、
[37 LP64](artifacts/bmc-typed-reference-2026-10-10/37-64-partial.json)。
