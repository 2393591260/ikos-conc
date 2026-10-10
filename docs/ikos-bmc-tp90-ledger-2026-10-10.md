# TP90之后的逐任务台账（2026-10-10）

`79d9476`的冻结集基线为TP90/FP0/TN697/FN0/UNKNOWN242/error0。
剩余242项已全部重新诊断，逐项仍UNKNOWN，没有错误。300条历史任务
保留在[逐项记录](artifacts/bmc-tp90-ledger-2026-10-10/results.json)，
当前二进制哈希与[汇总](artifacts/bmc-tp90-ledger-2026-10-10/summary.json)对应。
原始日志与数据库留在`/tmp/ikos-recovery-2026-10-10/tp90-unknown-diagnostics/`。

剩余参考分类为FAIL107/TIMEOUT80/ERROR25/PASS29/UNKNOWN1，历史参考只在
源码哈希相同时沿用；不是本阶段重新运行242次参考的结果。四个新增fib有
本轮双架构固定参考原生SOURCE_RACE证据。FAIL证明参考找到了源码竞争，
不证明IKOS所选模型完整可行；TIMEOUT/ERROR也不是参考工具无法实现的证明。

fib5的新ILP32两个、LP64两个选中模型在bound13部分约束检查中均SAT，
各绑定70个内存事件；破坏store值均UNSAT_AT_BOUND。此前bound16超时记录
保留，不以重跑覆盖。仍未绑定完整CFG/call、局部值、同步历史和总调度，
因此完整独立验证计数不增加。

本轮进展为受限worker终止端点、精确同路径SC读源、确定值传播、不可行
守卫和公式构造顺序优化。均有阶段提交及1029项配对证据；没有frontend改动。
所有原TP/TN保留，新增四个fib，不能据此声称全部soundness问题已清零。
下一步继续具体参考竞争中的调用/循环/动态对象与线程闭包；SAFE过滤全证明、
完整验证及稳定全局性能收益仍未闭环。计数只记录实际观察，不换算恢复收益。
