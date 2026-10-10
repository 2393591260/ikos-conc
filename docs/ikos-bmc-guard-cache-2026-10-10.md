# 固定候选组内复用确定false守卫（2026-10-10）

先在固定Dartagnan确认较大fib真实竞争：safe/unsafe10和11 bound26
双架构SOURCE_RACE；12在26返回SOURCE_UNKNOWN，bound28才双架构
SOURCE_RACE。IKOS旧诊断显示它们在10秒预算内反复编码同一提前退出
守卫。此次缓存只复用精确表达式simplify为false的证据，不能缓存一般
SMT UNSAT或unsupported，也不能把未确认变成SAFE。

## 复用前提

`check_race`可选返回GuardRejection（A/B路径及已消费的global读集合）。
只有非startup路径，编码开始时该路径值cache为空，guard精确false且
此前无unencodable，才提供证据。编码消费formal实参或非直接global的
load就禁止复用；拒绝点之前消费的递归读一并进入依赖集合。空cache
前提防止另一条路径先前预编码的值隐藏传递依赖；同一守卫阶段内
重复cache使用仍保留首次消费记录。固定startup可继续使用其原编码，
但不缓存startup拒绝。

`confirm_race`收集当前整个对侧路径集合的真实store union。
任何非直接global store使union不精确，禁用复用；被拒绝路径也必须
全部store直接global。读集合与对侧writer union不相交才缓存。
检查整个对侧集合而非第一次配对，防止“某分支不写，另一个分支写flag”
被错误删除；排除自己的间接store，防止根线程实际参数随main路径改变。
抽象初始化按原来源规则忽略，真实前缀的其他门保留。
缓存严格限于同一confirm_race的fixed facts/startup、展开路径组及界限，
不跨创建上下文、候选或递归界限。只跳过已证明在此组内保持false的
路径对；不消耗SMT调用次数，仍检查原deadline。指针集合只作membership，
查询顺序仍按稳定短路径排序，与ASLR无关。

这里的writer union是**两前缀编码内**的复用证明，不是全程序private
或一般K线程闭包证明。Dartagnan固定源
`program/processing/RemoveDeadCondJumps::eliminateDeadCondJumps`
同样只在常量传播后移除确定不可行控制；此次不是照搬其全程序处理，
而是在IKOS原实际值来源编码下附加跨配对不变性前提。

## 已有证据与下一瓶颈

22次循环最小例旧ILP32 UNKNOWN，新双架构FALSE；改名/helper旧变体同样
保持FALSE并实际命中缓存。该例ILP32所选模型绑定122内存事件，固定
参考bound23部分约束SAT，改store值UNSAT_AT_BOUND。
完整CFG/call/同步历史仍未绑定，完整独立验证计数不增加。

前一独立正确性提交修复了测试揭示的锁分区入口遗漏。其共享flag
writer union、间接writer/reader及helper/signed byte正例在本缓存版
仍双架构FALSE，安全对照SAFE。若对侧某分支能够写flag，就不能把
第一次不写flag配对中的恒假值用于其他分支。
34条SC/guard/search断言、14条锁干扰断言、34条atomic断言和P0
双架构20条观察通过。build/install/cmp完成；没有frontend改动。

较大fib成功减少重复false编码后，仍因实际可行查询SMT预算UNKNOWN，
不能宣称缓存本身恢复这些TP。后续研究po/spawn已证明的写源顺序，
减少general rf里的冗余选择；不单靠延长timeout，不放松值流或机器语义。

1029项ILP32全量逐项不变：TP92/FP0/TN696/FN0/UNKNOWN241/error0。
[配对](artifacts/bmc-guard-cache-2026-10-10/paired.json)及
[统计](artifacts/bmc-guard-cache-2026-10-10/summary.json)。测试与原始日志在临时目录，
只保留总结JSON。完整验证与SAFE过滤证明仍未闭环。
