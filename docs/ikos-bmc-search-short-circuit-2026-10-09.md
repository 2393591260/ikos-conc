# 竞争方向 SAT 后停止重复求解

`check_race` 查询两种相邻方向的析取；旧代码第一方向SAT并保存模型后，仍求解第二方向。
现在保存首个模型、恢复solver栈后立即返回true。第一方向UNSAT/UNKNOWN仍继续第二方向；
不改变执行、值、地址、同步、候选或展开约束，也不改变首次所选证据。

构建ikos-analyzer、同步install并cmp通过。全量配对1029 verdict全部相同：
TP58→58、TN698→698、FP/FN0→0、UNKNOWN273→273、error0→0。
双架构P0共20观察逐项检查。新正反例以真实main nondet初始化只读selector：
互斥guard保持UNKNOWN、两方向UNSAT查询次数不变；相同guard竞争保持FALSE，
SAT查询2→1，首份结构化traces/targets保持完全相同。Dartagnan双架构分别PASS/FAIL。

[精简证据](artifacts/bmc-search-short-circuit-2026-10-09/summary.json) 记录二进制指纹及断言。
原日志在 `/tmp/ikos-search-short-circuit-2026-10-09/`。源码仅两行，不涉及前端。
这是减少一次已无必要的SMT查询，不宣称整个管线提速一倍；尚无全管线性能倍数实验。
58个标签TP仍未全部严格独立验证，部分约束结果不能代替完整见证验证。
