# 实际 pthread_exit 完成端点（2026-10-10）

冻结集1029项与adc35a0逐项相同：TP86/FP0/TN697/FN0/UNKNOWN246/error0。
本阶段没有恢复新的标签TP；它解除了真实线程终止路径的展开障碍。
没有frontend修改，build、install同步/cmp完成，14条双架构终止端点断言、
34条atomic断言通过，P0的20条观察逐项相同。

`unroll_completed`此前只寻找AR return，因而无法展开以`pthread_exit(NULL)`
结束的worker。现在还接受实际声明的C ABI、void返回、单pointer参数、
LLVM noreturn的直接CallInst；payload必须是NULL，调用必须位于非main根函数体。
它仍不是普通可返回leaf：不会沿调用后继假造执行，也不会跳过前面的guard。
helper内终止、main终止、非NULL返回值、错误ABI及含未建模callback注册的前缀
继续拒绝。没有声称一般线程生命周期、C未定义行为或所有callback已建模。

固定Dartagnan `7caef5c850dc2dbf81c5554dcf41e402007f7a19` 的
`Intrinsics.java::inlinePthreadExit`将其转为`newThreadReturn(arg)`。
IKOS只迁移上述可核实的受限终止语义，没有沿用忽略callback的假设。
两个最小竞争及改名/字节宽度变体双架构原生参考均SOURCE_RACE，四个实际
选中模型的部分内存约束均SAT；CFG/call、局部值、同步历史、总调度尚未全部
绑定，**完整独立验证计数没有增加**。不可行尾部对照不确认竞争。

具体目标`pthread/fib_unsafe-5-racy.i`已在固定参考bound16双架构确认
SOURCE_RACE。IKOS已能展开终止路径，但仍UNKNOWN：定向观察59次UNSAT、
1次SMT UNKNOWN，消耗原10秒预算。下一步简化SC读源公式中由同路径po已经
确定的最新写，先核实公式等价，再衡量是否能恢复该项；不靠放宽超时碰运气。

[全量配对](artifacts/bmc-thread-exit-2026-10-10/paired.json)、
[统计与原生参考](artifacts/bmc-thread-exit-2026-10-10/summary.json)、
[所选模型部分检查](artifacts/bmc-thread-exit-2026-10-10/models-partial.json)。
