# 实际 helper 返回的全局指针

原点审查的闭合重叠成员例在Dartagnan已确认竞争，IKOS却因 pointer_on_path 不支持
call结果而UNKNOWN。参考加工IR的两个实际store均为 shared+4、32位值1；
Inlining.java通过独立scope复制callee寄存器、实参赋值，并将实际Return改成结果赋值。
可迁移的前提是完整调用路径和本次返回值，不是任选一个函数返回指针的may摘要。

现在IKOS地址解析对已展开的调用查找实际callee、匹配调用事件下标的Return，
继续沿该返回值在返回事件之前的动态定义追踪。两次helper调用不能混用定义，
返回地址中的实参/下标也用原有实际调用绑定。只接受最终解析到global的地址：
经返回值到达LocalVariable即拒绝；未知外部调用、未执行return、heap来源及其它
未支持的指针操作仍UNKNOWN。暂不证明返回栈地址的生命周期，包括有效的调用者局部
地址透传也保守拒绝；此能力边界不应在未来作为永久要求。

重叠union成员及重复helper的数组正例双架构UNKNOWN→FALSE；不重叠helper对照SAFE保持，
共享selector互斥guard对照UNKNOWN保持（实际SC读来源约束UNSAT），无效栈返回边界未确认。
共十项双架构断言、P0二十观察逐项断言、build/install/cmp通过。
固定Dartagnan bound=2对重复helper正例双架构FAIL，两个安全对照双架构PASS；
重叠成员的双架构FAIL证据已在原点审查中建立，再迁移能力。

当前选中的重叠成员 ILP32 模型在参考端绑定两个写的实例/地址4/宽度4/值1后SAT，
修改指定写值后UNSAT_AT_BOUND。CFG/调用发生次数、局部值和全部同步/调度仍未完整
绑定，不增加完整独立验证计数。源码实际地址和无同步写提供该最小例的具体执行依据。

完整1029冻结集ILP32逐项保持：**TP61→61（标签统计、未全部独立验证）、TN697、
UNKNOWN271、FP/FN/error0**。这是通用地址能力扩展及验证，未恢复额外冻结集TP。
[精简证据](artifacts/bmc-pointer-returns-2026-10-09/summary.json)，原日志位于
`/tmp/ikos-pointer-returns-2026-10-09/`，不提交。

```bash
python3 -B docs/review-repros/bmc-pointer-returns/run.py --output /tmp/ikos-pointer-returns-NEW
```

后续继续按最新参考竞争/诊断选择非返回路径剪枝、完整atomic目标闭包、动态对象或
必要额外线程。全量计数不变不表示已解决这些能力，也不能把当前61当作可恢复上限。
