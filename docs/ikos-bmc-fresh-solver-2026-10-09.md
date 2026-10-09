# 独立竞争方向查询：保留非增量预处理

仿射守卫之后，unsigned-wrap 最小竞争仍 UNKNOWN。源码中 uchar start=255，
end=start+2 窄化为1，因此循环零次，两个成功创建的 worker 都执行 data=1。
AR 中 nondet 的返回临时为 si8=-1，随后 bitcast 为 ui8=255；这个位模式合法，
不是无来源自由 load。此前两个方向查询使用 push/pop，首个混合 Int/BV 公式超时。

先试验把算术/转换改成整数模运算，全量1029结果不变，正例仍 UNKNOWN，因此撤回。
隔离导出的试验公式后，同一公式独立求解为 SAT，加 push 后在1秒内 UNKNOWN/canceled。
这指向求解方式，而不是缺少可行执行。该观察限于本地 Z3 配置，不是所有版本的定理。

最终只修改 query 生命周期：每个竞争方向新建 solver，复制全部基础约束，加该方向的
相邻访问约束，维持每查询1秒预算，从实际 SAT query 取模型。不改算术、值来源、
地址、实例或同步语义，不把超时变成SAT。两个新公式分别与原对应公式完全相同，
区别是无需进入增量模式，保留非增量预处理机会；临时导出探针已删除。

最终 build/install/cmp、双架构 P0 20观察断言、仿射八例断言、新回绕八例断言通过。
冻结集1029项 ILP32 逐项相同：**TP61→61（标签统计、未全部独立验证）、TN697、
UNKNOWN271、FP/FN/error0**。这是能力/定向性能改善，不宣称冻结集TP增长。
uchar 循环回绕和 uint 最大值+1竞争双架构均 FALSE；两种不可达写安全对照均 SAFE。
固定 Dartagnan bound=2 对 uint 正例 FAIL、两个安全对照 PASS，均覆盖双架构；
uchar正例已有 bound=5 双架构 FAIL 证据。

ILP32 uchar 正例选中的两个 data 写，在严格参考端绑定指定实例/地址/宽度/写值后 SAT，
改写值后 UNSAT_AT_BOUND；局部 nondet/CFG/调用/同步/总调度未完整绑定，仍为部分内存
模型检查，完整反例验证计数不增加。实际执行说明如第一段，不能借SAT推导所有反例可靠。

无其他扫集并行时对同一 uchar ILP32 正例串行交替三轮：原 wrapper 总时间中位数
11.23秒、现在0.187秒；原 verdict UNKNOWN、现在 FALSE。这个约60倍的定向观察
涉及从超时搜索到首查询SAT，不能作为全项目或与Dartagnan相比的速度比例。
[精简证据](artifacts/bmc-fresh-solver-2026-10-09/summary.json)保留具体测量和验证范围。

```bash
python3 -B docs/review-repros/bmc-fresh-solver/run.py --output /tmp/ikos-fresh-solver-NEW
```

原日志在 `/tmp/ikos-fresh-solver-2026-10-09/`，不提交。剩余 UNKNOWN 仍需按最新阻塞
继续处理，当前数据不支持“已达到可恢复上限”，也未证明所有 SAFE 过滤规则。
