# 静态条件变量初始化的ABI证据

本阶段冻结集1029项ILP32配对为**TP82→84 / FP0 / TN697 / FN0 /
UNKNOWN250→248 / error0**。唯一恢复`thread-join-counter-inner-race-5`与
`thread-join-counter-outer-race-2`，所有旧TP/TN逐项保留，没有frontend改动。

这两个任务使用静态条件变量，没有显式cond_init。main观察存活计数不能
保证worker已结束最后一次data访问；有限虚假唤醒也不能替代共享值谓词。
固定Dartagnan本轮双架构重新确认两项原生竞争。迁移的wait语义仍为实际
持锁→释放→有限虚假唤醒→重新持锁，不从signal创造同步边。
旧IKOS在检查condition对象时因没有显式初始化而拒绝这些前缀。

新增证据只识别**glibc x86 Linux/GNU目标的默认静态表示**：LLVM底层global
须为非TLS、完整`union.pthread_cond_t`直接对象，48字节，单个控制结构，七个
字段的偏移/大小分别匹配0/8/16/24/32/36/40与8/8/8/8/4/4/8。LLVM实际初值
须全零，AR按真实类型取两个64位及八个32位初值也须全零。依据是当地
pthread.h的PTHREAD_COND_INITIALIZER及thread-shared-types.h、atomic_wide_counter.h，
对应源码哈希保存在证据中。不把任意零数组、其他ABI、未知/非零初值或
存储子范围当作合法默认condition。前提不可证仍UNKNOWN。

静态证明加入现有对象生命周期/对齐/大小检查，原有显式初始化的执行前提、
wait所有权、同condition的mutex绑定及普通访问覆盖控制存储的拒绝均保留。
destroy、非默认属性、局部/堆/嵌入式condition仍有能力边界。平台ABI需要
独立核实后扩展，不能把这一布局推成普遍POSIX零初始化规则。
该ABI上未写显式C initializer的静态condition也会产生同一全零表示，旧
missing-init-unsupported例因此恢复，改名static-zero-race；没有按文件名放行。

验证完成build、install同步/cmp，16条新双架构断言覆盖改名、helper、signal、
虚假唤醒、公共锁对照、错误存储类型、非零初值及存储覆盖。20条已有condition
断言及26条heap断言通过，P0的20条观察与此前逐项相同。
三个正例及一个公共锁对照双架构参考分别SOURCE_RACE/PASS。
额外检查inner-race-2/3也有本轮参考竞争，但IKOS仍UNKNOWN，当前搜索未找到；
inner-race-4在参考bound2为UNKNOWN，未借历史竞争结论声称本轮已确认。

两项新benchmark选中访问、整数值、地址、创建实例和已映射顺序在参考中
部分约束SAT；共39/44条源步骤仍有未绑定项，包括完整同步和CFG/call对应。
完整独立反例验证计数不增加，TP继续标记标签确认、未全部独立验证。
没有测得稳定提速；下一步研究小界限搜索以避免直接扩到16造成枚举饱和。

[全量配对](artifacts/bmc-static-condition-2026-10-10/paired.json)、
[汇总/参考](artifacts/bmc-static-condition-2026-10-10/summary.json)、
[最小例A/B](artifacts/bmc-static-condition-2026-10-10/fixtures.json)、
[选中模型部分检查](artifacts/bmc-static-condition-2026-10-10/partial-models.json)。
原日志/数据库均在`/tmp/ikos-recovery-2026-10-10/static-condition-*`。
