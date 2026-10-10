# 跨分支的真实创建实例映射（2026-10-10）

本阶段只修改外部验证适配器，不修改IKOS判定。仍为部分源程序约束检查，
不是完整witness/源程序validator。对应排序恢复实验单独提交和回归。

## 从冲突到定位

fk2012本次选择consumer首次counter读与producer释放lock1后的counter=0写。
IKOS实际startup先执行consumer分支，再在下一循环执行producer分支。
旧mapper各按静态创建站点的第一次出现选clone1，把它们分别映射到
参考主线程第一循环的互斥分支，导致双架构UNSAT_AT_BOUND。
固定参考对源程序仍SOURCE_RACE。这种错误绑定的UNSAT不能据以判定
IKOS反例虚假，更不能因为文件标签命中就忽略冲突。

参考处理后的parent事件显示第一轮producer/consumer创建id16/22，
第二轮38/44。实际顺序是consumer22再producer38；不能选producer16。
源码只创建一次某种入口，并不意味着对应静态clone1一定执行。

## 通用修复及保守边界

mapper按实际父trace的全部创建先后匹配参考forward CFG中的源站点，
静态源位置/AR站点及pre-unroll身份须唯一；选择保持顺序的最早clone。
这可能仍因复杂CFG、额外跳过循环或调用映射而不完整，不能从UNSAT推导
IKOS模型错误。不是按文件名或指定线程名修补。

hook的creation_occurrence改为所选clone之前同站点已执行创建的次数，
加上当前已执行创建为实际第k次；允许更早clone因分支未执行。
creation_prefix_count同时约束父线程该点之前所有已执行创建总数，
防止参考私自增加前缀中的线程。每个事件的id、thread、creator及descriptor
仍严格绑定。Memory occurrence仍沿用原先全部早期clone执行的受限规则，
没有顺便放宽一般load循环映射。

这仅绑定创建序列和现有已映射内存/值/相对SC次序，仍未绑定全部CFG、
call occurrence、局部值、nondet及同步历史，不声称完整反例验证。

## 验证

固定参考新hook重导IR双架构原生SOURCE_RACE；FK两所选模型部分SAT。
各改坏一个写值、或创建前缀数得到UNSAT_AT_BOUND；改坏creator得到
UNSUPPORTED，不能接受损坏实例。20条双架构创建映射断言、17条通信/
顺序断言及11条结果分类断言通过。两数组双架构四个历史所选模型用新
hook/mapper重新检查全部SAT，原生也全部SOURCE_RACE。

初次复用旧classes的IR descriptor没有memory宽度字段，与新hook不一致，
返回UNSUPPORTED；已重新导出同一源码/版本的IR，未绕过hash检查。
未提交AR重放脚本只读使用：FK两架构均条件ACCEPT（各76事件）；
四次改坏read或creator均REJECT。它依赖同一IKOS程序快照，不能作为独立
C导入证据，脚本hash记录且未修改/未提交。

[汇总](artifacts/bmc-creation-sequence-validation-2026-10-10/summary.json)、
[旧冲突](artifacts/bmc-creation-sequence-validation-2026-10-10/old-incompatible-mapping.json)、
[当前模型与破坏探针](artifacts/bmc-creation-sequence-validation-2026-10-10/selected-models.json)、
[兼容复测](artifacts/bmc-creation-sequence-validation-2026-10-10/compatibility.json)、
[条件重放](artifacts/bmc-creation-sequence-validation-2026-10-10/conditional-replay.json)。
原始日志/数据库/classes在`/tmp/ikos-recovery-2026-10-10/fk-scheduling/`。
