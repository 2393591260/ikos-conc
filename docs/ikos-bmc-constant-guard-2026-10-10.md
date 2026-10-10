# 确定读值传播与不可行守卫（2026-10-10）

与2f3cbf2冻结集1029项配对：TP86→89/FP0/TN697/FN0/UNKNOWN246→243/error0。
四个fib任务恢复FALSE：`fib_{safe,unsafe}-{5,6}-racy.i`。
`47_ticket_lock_hc_backoff_vs.i`在全量FALSE→UNKNOWN；隔离旧/新均FALSE，
约8.34/8.52秒，查询/诊断计数相同，没有把此资源回退静默忽略。
没有frontend修改或放宽预算。**标签TP仍未全部独立验证。**

同路径SC读源已证明唯一时，保留`read == source`约束，再以精确simplify
后的source替换该动态读的缓存表达式。后续守卫如果精确化简为false，直接
返回未确认而不调用SMT。这是等式替换和原公式UNSAT判定，不把抽象区间中的
may值当实际值，不改变跨线程rf或符号来源前提。可行路径仍通过完整算术、
内存和同步检查；未知来源不能成为新鲜自由变量。

具体`fib_unsafe-5-racy`旧版本在10秒预算耗尽时UNKNOWN；新版本ILP32
隔离约10.07秒FALSE，两条候选均SAT，368个不可行路径对提前排除。
LP64也FALSE。四个新增fib任务在固定参考bound16双架构原生SOURCE_RACE。
这里“safe”是文件名中的旧assertion性质，不代表no-data-race安全。

新增长循环最小例此前ILP32 UNKNOWN，新版双架构FALSE；atomic读取对照
双架构SAFE。20条读源/守卫断言、34条atomic及6条startup断言通过，P0的
20条观察相同，build/install/cmp与全量完成。五个最小竞争双架构原生参考
SOURCE_RACE、三组安全对照SOURCE_PASS；十二个所选模型部分约束SAT。

**验证未闭环**：fib5的ILP32两条、LP64一条所选模型各绑定70个内存事件，
部分约束查询均在60秒超时。修改实际store值的探针均UNSAT_AT_BOUND；
这只能证明该错误值被拒绝，不能把原模型超时改写成SAT或完整验证成功。
CFG/call、同步身份、局部值及总调度仍未全部绑定，完整验证数不增加。
下一步先让确定守卫更早阻止不可行前缀的额外编码，保持原可行路径检查。

[全量配对](artifacts/bmc-constant-guard-2026-10-10/paired.json)、
[统计与隔离A/B](artifacts/bmc-constant-guard-2026-10-10/summary.json)、
[原生参考](artifacts/bmc-constant-guard-2026-10-10/bench-native.json)、
[最小例部分验证](artifacts/bmc-constant-guard-2026-10-10/reference-partial.json)、
[fib5 ILP32](artifacts/bmc-constant-guard-2026-10-10/fib-32-partial.json)、
[fib5 LP64](artifacts/bmc-constant-guard-2026-10-10/fib-64-partial.json)。
