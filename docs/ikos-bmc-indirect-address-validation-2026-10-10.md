# 间接访问的严格参考地址约束

固定 Dartagnan 在处理 `06-symbeq_10-equ_rc.c` 时把 main 的目标写打印为
`store(bv64 __memToReg#4, bv32(8))`，对象名 B 不在打印文本里。旧映射器因此
拒绝目标；不能靠检查打印文本获得实际地址。

现在仅当 AR 的源位置、操作类型能唯一确定静态访问语句时，允许间接打印
进入映射。动态线程／创建／循环 occurrence 检查保持原有约束。参考 SMT
继续固定访问地址为该静态对象的 base+offset，并检查访问宽度、对象大小及
事件描述符。对象名不是自由占位符，多源位置或未知克隆仍拒绝映射。

双架构各五条参考查询通过：实际访问对及地址 SAT；错误对象与错误 offset
UNSAT_AT_BOUND；不存在的对象及错误宽度 UNSUPPORTED。每架构另有两个
歧义拒绝检查；原八条映射断言通过。IKOS 固定写值改动也被参考拒绝。
源码及独立参考查询都存在竞争，仍没有固定完整局部值、锁历史和全部 CFG
occurrence，证据等级保持 PARTIAL_FIXED_MEMORY_MODEL，完整验证数量不增加。

本项只修改研究映射／校准工具，未修改 analyzer 行为，不为它重复全量测试。
[双架构证据](artifacts/bmc-indirect-address-2026-10-10/32.json)、
[64位证据](artifacts/bmc-indirect-address-2026-10-10/64.json)、
[可重跑校准](../scripts/dartagnan_strict/test_indirect_addresses.py)。
