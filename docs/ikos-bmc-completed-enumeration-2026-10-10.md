# 枚举中断后的完整竞争前缀与分阶段搜索

条件变量摘要使 `thread-join-counter-outer-race-3.i` 的展开路径超过256条。
旧展开器在构造预算耗尽时清空此前已经发出的完整路径，导致该用例从FALSE
退回UNKNOWN。本轮固定Dartagnan原生运行重新确认该源文件竞争，并定位到
路径构造限额；不能把这项回退解释为随机波动。

保留已完成的执行前缀不要求穷举所有替代路径。`unroll_to` 只在真实目标已到达
且必要atomic区间闭合后发出路径；`unroll_completed` 只保留真正Return且包含
候选的路径。中断的DFS分支从未发出，不参与SMT。保留的每条路径仍须通过
实际守卫、值来源／定义性、地址宽度、锁与atomic历史、线程创建的全部检查。
这个改变只增加寻找FALSE的执行选择，不用枚举不完整或UNSAT证明SAFE。

直接把这些路径混入原搜索会挤占共享预算：第一版全量TP76，四对试探限制版
TP79，均存在旧TP退回UNKNOWN，已撤回。最终采用两轮：先保持原查询集合
和顺序，把有完整路径但枚举中断的组暂缓；再查询暂缓组，重置候选／query
计数但不重置原10秒文件deadline。第一轮已确认竞争则无需额外搜索，暂缓组
仍输出一次UNKNOWN报告。每组最终报告仅发出一次，选中的竞争对和报告锚点
仍保持一致。没有延长文件搜索时间或绕过任何语义gate。

最终build成功，install同步并cmp一致。四条新双架构断言：分支爆炸正例
UNKNOWN→FALSE，互斥值守卫对照不确认FALSE。参考双架构正例原生FAIL、
对照PASS；选中模型的内存约束双架构SAT，篡改写值后UNSAT_AT_BOUND。
28条atomic-target、20条condition回归通过；20条P0观察与前版逐项相同。
以上参考检查仍为部分固定内存模型，未绑定完整局部值、调用／CFG occurrence
和同步历史，不增加完整独立验证数量。

1029项冻结集ILP32逐文件配对：**TP79→80 / FP0 / TN697 / FN0 /
UNKNOWN253→252 / error0**。唯一转移为上述outer-race-3 UNKNOWN→FALSE，
没有旧TP／TN回退。lamport、ticket和三个计数器的定向检查均FALSE。
LP64仅做定向回归，未做全量；没有可比性能测量，不能据此声称速度提升。
TP仍是标签确认、未全部独立验证。

实验及最终可执行文件哈希、参考结果、回归数量见
[汇总](artifacts/bmc-completed-enumeration-2026-10-10/summary.json)，
全部1029项配对见[配对](artifacts/bmc-completed-enumeration-2026-10-10/paired.json)。
原始日志和数据库留在临时目录，不进入git。fixtures见
[回归入口](review-repros/completed-enumeration/run.py)。
