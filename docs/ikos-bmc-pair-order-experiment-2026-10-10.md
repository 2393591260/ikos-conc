# 总前缀长度排序实验：撤回

尝试用有界行堆合并两个已排序路径集合，按双方前缀长度之和查询所有路径对，
替代先固定 A、遍历所有 B 的次序。每个公式、gate、预算不变；未耗尽时仍覆盖
整个笛卡尔积，堆只保存每行下一项。该启发式不能保证有预算时优于原次序。

build、install 同步及双架构 P0／机器回绕／global-choice 回归通过。
固定 Dartagnan bound2 重新确认 ticket 源程序竞争；IKOS 仍 UNKNOWN。
定向 fmaxsymopt 因搜索预算耗尽变 UNKNOWN，lu 仍 FALSE。
冻结集1029项 ILP32：TP78/FP0/TN697/FN0/UNKNOWN254/error0，计数没变，
但 fmaxsymopt FALSE→UNKNOWN、lu UNKNOWN→FALSE。不能把替换计为精度增长，
也没有稳定提速证据。已撤回源码并重新构建原机制。

[实验小证据](artifacts/bmc-pair-order-experiment-2026-10-10/summary.json)。
后续需要在文件级预算内公平安排候选、展开界限及路径组合，并实测稳定性；
简单交换路径次序不足以完成此目标。未修改任何语义或扩大支持范围。
