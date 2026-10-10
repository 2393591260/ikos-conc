# 完整创建前缀与分阶段查询（2026-10-10）

冻结集1029项逐文件配对：TP98→101、FP0、TN696、FN0、UNKNOWN235→232、error0。
旧TP/TN全部保留，新增两个nondet-array及48_ticket_lock_low_contention_vs-b。
TP仍为标签确认，未全部独立验证；不是完整soundness证明。

## 具体执行与通用改动

两个nondet-array的main以实际nondet选择5至10个线程，同一create站点循环。
worker读共享x、写a[x]、更新x。main完成前两个create后可以暂停，两个孩子
都读0并写a[0]，剩余孩子不必创建。实际n>=5守卫和两个创建实例必须编码，
不能把循环上限改成2或把抽象may多实例当作创建证据。

原创建枚举因work budget中断而整批丢弃结果，尽管其中已有到实际目标的
完整前缀。现在保留已发出的前缀，检查目标标记、实际spawn数量、锁释放、
地址范围、定义性及SMT守卫；未完成DFS分支仍不作为反例。
只取有限可行执行是欠近似搜索，找不到反例始终不用于证明SAFE。

创建搜索先用bound2，找不到结构可用前缀再用原bound16。
在原10秒文件deadline内分三轮查询：先原候选；再已完成枚举及短创建前缀，
同组内原候选优先；短前缀仍未确认的组清除创建cache并以bound16重试。
搜索调整不改变实际守卫和确认义务，报告保留原候选序号。
第一次create前有6轮初始化的变体覆盖较大界限回退。

与当前Join公式等价的提前UNSAT剪枝是独立前置提交fab428e，
见[Join报告](ikos-bmc-join-query-prune-2026-10-10.md)。它不是通用Join身份语义。
没有前端或SAFE规则改动。

## 回退实验与最终证据

早期版本全量TP100，但逐项为三个新增和47_ticket_lock回退；
[早期配对](artifacts/bmc-activation-prefix-2026-10-10/early-paired.json)保留。
隔离旧/新二进制表明新worker查询挤占旧main/worker确认的deadline，
不能归咎于资源波动。分轮调度和独立Join剪枝后，最终旧TP全部保留。
fk2012早期的瞬时恢复未保留：最终32/64定向仍UNKNOWN，不算收益。

两个数组×双架构固定Dartagnan bound12新跑均SOURCE_RACE；最终四个
所选模型（每输入/架构一项）部分约束SAT。旧版本八模型记录不冒充最终模型。
ticket48固定参考bound4新跑SOURCE_RACE，当前ILP32所选模型部分约束SAT，
隔离诊断也恢复FALSE。参考的另一条竞争不能替代这些所选模型的绑定。
本阶段未完成CFG/call occurrence、全部局部值及同步历史的独立绑定。

10条激活断言、46条SC、34条atomic、双架构20项P0观察通过；P0逐项断言
返回码及判定，而非仅检查runner退出。build/install哈希一致。
未提交的AR重放脚本只作历史只读试验：其main(argc,argv)零实参初始化失败
尚未执行到事件，不证明模型错误；脚本未改/未提交。

[最终配对](artifacts/bmc-activation-prefix-2026-10-10/paired.json)、
[汇总](artifacts/bmc-activation-prefix-2026-10-10/summary.json)、
[数组参考](artifacts/bmc-activation-prefix-2026-10-10/reference.json)、
[数组模型检查](artifacts/bmc-activation-prefix-2026-10-10/partial-validation.json)、
[ticket48检查](artifacts/bmc-activation-prefix-2026-10-10/ticket48-validation.json)、
[选中模型](artifacts/bmc-activation-prefix-2026-10-10/confirmed-pairs.json)、
[定向结果](artifacts/bmc-activation-prefix-2026-10-10/targeted.json)、
[最小回归](artifacts/bmc-activation-prefix-2026-10-10/fixtures.json)。
定向时间不是公平全局加速证据。LP64定向，未全量。
原始日志/数据库在`/tmp/ikos-recovery-2026-10-10/activation-audit/`。
