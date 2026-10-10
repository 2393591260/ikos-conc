# 暂缓候选组的小界限补充搜索

第一轮查询顺序与集合保持原样。第二轮只对界限16枚举耗尽的候选组，先查询
界限2的实际完成前缀，再继续原来的完成路径集合；共用原10秒deadline与
查询额度，不增大总时限。界限2不表示两次源循环，实际仍以基本块访次数计。
保留真实守卫、值/读源、地址和atomic闭包；没有将截断分支作为反例。
有限UNSAT和搜索结束仍UNKNOWN，不能证明SAFE。

原来main的创建循环需要重访循环头才能退出。界限1只留下未创建worker的
main路径；直接跳到16时，后续多个共享条件循环在DFS中产生大量实际不可行
但还未求解的完成路径，并占满枚举额度。界限2补入较短的真实创建前缀。
同一种通用机制使nested-loops最小竞争例双架构从UNKNOWN变FALSE，安全对照
仍SAFE；不是针对计数器文件或标签挑选循环次数。

`thread-join-counter-inner-race-2/3`本轮固定参考双架构SOURCE_RACE；IKOS
双架构恢复FALSE，四个实际选中模型在参考中的部分约束均SAT。仍有CFG/call、
同步及局部值未绑定，完整独立验证计数不增加。定向两项由约10.2秒UNKNOWN
变约2.3秒FALSE，求解次数大幅下降；这只是本轮局部观察，不是全局提速证据。

1029项ILP32全量为**TP84→85 / FP0 / TN697 / FN0 / UNKNOWN248→247 /
error0**：两个新增，但旧`23_lu-fig2.fixed`回退UNKNOWN。隔离旧/新均FALSE，
查询计数完全一致（544次UNSAT、1次SAT），均未走小界限补充分支，约7.6/7.7秒。
因此本轮不能将lu回退归因于小界限语义，也不能宣称全量净增2或全部旧TP保留。
下一阶段处理其昂贵的无效atomic候选查询，继续保留这项回退的配对证据。

build、install同步/cmp完成，8条完成枚举和28条atomic双架构断言通过，P0
20条观察逐项不变。nested-loops正例双架构参考在界限2为SOURCE_RACE；安全
对照的首次界限2检查为UNKNOWN，补核界限4后双架构PASS，保留初次未定结果。
无frontend改动。该阶段立即独立提交，不混入后续atomic查询优化。

[配对](artifacts/bmc-small-bound-2026-10-10/paired.json)、
[统计/隔离证据](artifacts/bmc-small-bound-2026-10-10/summary.json)、
[四个模型](artifacts/bmc-small-bound-2026-10-10/partial-models.json)、
[最小例参考](artifacts/bmc-small-bound-2026-10-10/fixture-reference.json)。
