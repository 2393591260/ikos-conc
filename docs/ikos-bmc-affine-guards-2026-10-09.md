# 动态仿射守卫：减少不可行循环路径

`11_fmaxsymopt-race.i` 的数组循环从符号 offset 开始，以 offset+固定长度结束。
原展开器只折叠常量比较，对同源符号表达式的必真/必假关系继续枚举，耗尽有限搜索预算。
固定 Dartagnan 的历史参考结果为 FAIL；其结果不是 IKOS 模型的独立验证。

现在 `unroll.hpp` 在已有常量守卫过滤旁规范化单变量仿射表达式。
叶子身份是变量和当前路径的实际定义事件下标；只穿过同类型赋值、实际 helper 参数，
以及符号一致、带 AR no-wrap 标记的加减常量。比较双方属于同一叶子时消去公共项。
例如同一动态 x 的 x+1 < x+2 为真，x+2 < x+2 为假，无须猜测 x 的数值。

该过滤只删除在定义良好、no-wrap 执行中不可能满足的分支。不能证明同源或无溢出时
保留原搜索；合成初始化不参与证明。两次 load 或循环中不同定义事件不能合并。
最终编码仍检查实际值来源、机器整数范围、地址、SC 读来源及同步，未放宽任何确认门。
无符号回绕不允许采用数学整数次序消项。

最终 build、install 同步及二进制一致性检查通过。1029 项冻结集 ILP32 配对为
**TP60→61（标签统计、未全部独立验证）/ TN697→697 / UNKNOWN272→271 / FP/FN/error0**。
唯一变化是 `11_fmaxsymopt-race.i` UNKNOWN→FALSE，旧 60 TP 和 697 TN 全部保持。
搜索边界仍为原有 1/16，未重启此前造成 TP 替换的界限搜索实验。

双架构四例共八项断言通过：offset 与 helper 循环竞争恢复 FALSE，同值最大值安全对照
保持 UNKNOWN；unsigned-wrap 真竞争仍 UNKNOWN，未恢复。双架构 P0 共20观察逐项断言通过。
固定 Dartagnan bound=5 双架构对三个竞争例均 FAIL，对同值安全对照均 PASS。
最终 offset ILP32/helper LP64 导出模型分别映射五个共享内存事件，指定地址/宽度/值为 SAT，
修改指定写值后为 UNSAT_AT_BOUND。局部/nondet、CFG/调用实例、同步历史及总调度仍未绑定，
此证据仅为部分内存模型验证，完整反例验证计数不增加。

[精简证据](artifacts/bmc-affine-guards-2026-10-09/summary.json)保留逐项转移与覆盖限制。
原日志位于 `/tmp/ikos-affine-guards-2026-10-09/`，不进入 git。

```bash
python3 -B docs/review-repros/bmc-affine-guards/run.py --output /tmp/ikos-affine-guards-NEW
```

定向日志显示 offset/helper 搜索次数减少，但未进行隔离性能实验，不宣称稳定加速比例。
后续继续诊断 unsigned-wrap 的混合 Int/BV 求解与来源阻塞，以及更新当前 UNKNOWN 台账。
