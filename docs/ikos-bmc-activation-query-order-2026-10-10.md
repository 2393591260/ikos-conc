# 新激活候选的查询顺序（2026-10-10）

冻结集逐文件TP101→102、FP0、TN696、FN0、UNKNOWN232→231、error0，
唯一变化是fk2012恢复FALSE，旧TP/TN全部保留。TP仍为标签确认，
未全部独立验证。每个阶段独立commit，不修改frontend、SAFE或SMT语义。

## 从具体竞争到通用优化

fk2012的producer先持有flag锁lock2/lock1，counter=0分支释放lock1后
直接写counter；consumer持lock1读counter。选择producer释放lock1、
consumer随后获取lock1，再把producer的counter=0和consumer的读相邻安排，
形成实际未共同受锁保护的访问对。无需放宽SC读值或声明flag锁不重要。

旧排序先试consumer/consumer等同入口多实例，耗尽10秒deadline。
沿用现有激活三轮搜索，只在第二轮排序新增短创建前缀候选：
旧/主线程查询仍最先，同组新候选中不同入口先于同入口；同优先级稳定排序。
序号/报告对应原候选顺序，所有创建、值、地址、definedness及同步义务保留。
这是启发式工作排序，不是不可行候选证明；同入口仍会在预算内被查询。
不同入口可能也更贵，不能从单例推导普遍收益或完备性。

FK32/64隔离约1.41/1.37秒均FALSE；历史旧查询耗满10秒仍UNKNOWN。
47/48-ticket及两个数组本阶段隔离均保留FALSE。时间不是公平全局加速实验。
6条新双架构断言含改名、不同root竞争、flag互斥对照，旧激活10、SC46、
atomic34、堆读30和P0明确20观察断言通过；build/install同步且hash一致。
全量配对只增FK，不把TN或其他旧TP预算损失藏在总数中。

## 模型冲突已经处理，但证据仍有边界

固定参考对FK双架构新跑SOURCE_RACE。初次所选模型部分检查均UNSAT，
已定位旧mapper错误地把不同分支首次创建绑定到同一循环的互斥克隆。
独立适配器提交及证明/破坏探针见
[创建序列验证](ikos-bmc-creation-sequence-validation-2026-10-10.md)。
修复后的两所选模型部分SAT；改值/创建数UNSAT、改creator UNSUPPORTED。
另对未提交AR脚本只读试验两架构条件ACCEPT，改值/creator四项REJECT。
它不是独立C源程序验证，未因此撤销文档中的完整验证待办。

新最小竞争与改名变体参考双架构均SOURCE_RACE；互斥对照是
SOURCE_UNKNOWN（循环界限），不能写成参考已证明SAFE。

[配对](artifacts/bmc-activation-query-order-2026-10-10/paired.json)、
[汇总](artifacts/bmc-activation-query-order-2026-10-10/summary.json)、
[定向](artifacts/bmc-activation-query-order-2026-10-10/targeted.json)、
[LP64](artifacts/bmc-activation-query-order-2026-10-10/lp64.json)、
[最小回归](artifacts/bmc-activation-query-order-2026-10-10/fixtures.json)、
[最小参考](artifacts/bmc-activation-query-order-2026-10-10/fixture-reference.json)。
原始材料在`/tmp/ikos-recovery-2026-10-10/fk-scheduling/`，未进Git。
