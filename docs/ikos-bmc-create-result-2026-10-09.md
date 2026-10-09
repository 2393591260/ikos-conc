# 成功 Create 的返回值与守卫

从具体差异 `pthread-demo-datarace-2.i` 出发：main 在pthread_create返回非0时退出，
成功后main和worker循环修改myglobal。既有BMC已经验证默认属性、有效输出槽并选择成功
创建事件，却把该事件的返回值当成未知extern来源，导致guard前的definedness失败。

先运行固定Dartagnan确认FAIL，再读Intrinsics.inlinePthreadCreate：它输出tid，设置status=0。
其TODO仍未完整建模失败值，不能把参考实现当作完整POSIX失败契约。
本地Linux man-pages同样规定成功返回0，失败返回错误码且输出handle未定义。

现只为已经分类为Create、默认属性、声明API且返回类型为32位signed int的事件编码0。
现有prefix_values_defined继续检查输出对象范围/参数；并不令未知调用返回0，也不自由化结果。
每路径动态定义缓存照常使用。选择成功执行是欠近似：可能漏掉涉及其他创建失败的真实竞争，
当前并不支持一般失败分支。未满足既有创建/前缀条件仍不确认。

最小success guard和helper变体在ILP32/LP64均UNKNOWN→FALSE；failure分支对照仍UNKNOWN，
不会把失败分支和活跃子线程拼接。Dartagnan分别FAIL/PASS。
四个正例的指定访问对及固定对象/偏移/宽度/写值在参考编码中SAT。
这些是部分约束验证，不宣称覆盖全部AR局部/CFG/同步/调度；真实C-DAC循环事件映射仍有歧义。

构建、同步install并cmp通过；双架构P0共20观察逐项检查。全量1029任务：
**TP58→59（标签统计、未全部独立验证）/ FP0→0 / TN698→698 / FN0→0 / UNKNOWN273→272 / error0→0**。
唯一转移为该C-DAC任务UNKNOWN→FALSE，其余1028项相同。
[精简证据](artifacts/bmc-create-result-2026-10-09/summary.json) 保存前后指纹、参考实验、
断言和逐任务转移。原日志位于 `/tmp/ikos-create-result-2026-10-09/`，不入git。

回归入口：

```bash
python3 -B docs/review-repros/bmc-create-result/run.py --output /tmp/ikos-create-result-NEW
```

后续需要一般创建失败/句柄模型及完整证据映射，本增量不替代它们。
