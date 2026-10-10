# 守卫先于未使用算术的完整检查（2026-10-10）

与3d7f063的1029项冻结集配对：TP89→90/FP0/TN697/FN0/UNKNOWN243→242/error0。
唯一变化是ticket-lock恢复FALSE；相对于本轮初始TP86，四个fib恢复、所有
旧TP/TN均保留。没有frontend修改，build/install/cmp完成。

先编码实际守卫，再编码未被守卫使用的算术结果。守卫需要的算术/读源仍
通过原精确编码；不可行守卫可提前结束。所有保留查询仍执行完整trap/poison
义务，包括未使用结果。任一guard无法编码就直接失败，避免后来复用失败
来源的缓存。这改变编码顺序而不放宽合法执行、内存/同步或未定义行为检查。

26条双架构读源/守卫断言和34条atomic断言通过，P0的20条观察相同。
新增强制signed overflow、除零的unused-store结果对照均不确认竞争，
合法unused算术正例双架构FALSE。四个最小正例双架构原生参考SOURCE_RACE，
十个所选模型部分内存约束SAT；这仍不是完整独立验证。

定向四个fib均FALSE，耗时约6.06/7.73/7.44/9.52秒；ticket-lock约9.27秒。
fib5此前约10.07秒，当前约7.44秒；只作为这两次隔离观察，不宣称全局提速。
上一阶段fib5三个所选模型的部分验证超时仍是未解决事项，完整验证计数
没有增加。下一步继续减轻确定不可行路径的公式构造开销、刷新UNKNOWN台账。

[全量配对](artifacts/bmc-guard-first-2026-10-10/paired.json)、
[统计与定向记录](artifacts/bmc-guard-first-2026-10-10/summary.json)、
[最小例参考](artifacts/bmc-guard-first-2026-10-10/reference-partial.json)。
