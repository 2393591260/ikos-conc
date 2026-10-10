# 条件变量的有限虚假唤醒前缀

固定 Dartagnan 本轮双架构重新确认 `condvar_spurious_wakeup.i` 竞争。
worker 持 mutex 等待后读 x，main 不持该锁循环写 x。条件变量可以虚假唤醒，
不必等 main 最后的 broadcast；worker 返回时重新持锁也不保护 main 的写。
旧 IKOS 因 cond_init／wait 没有摘要拒绝这条实际前缀。

依据 [POSIX Issue 8 pthread_cond_wait](https://pubs.opengroup.org/onlinepubs/9799919799/functions/pthread_cond_wait.html)：
调用方须持锁，等待释放 mutex，成功返回时重新持锁；允许虚假唤醒；同时等待
同一 condition 的线程不能绑定不同 mutex。固定参考
`7caef5c850dc2dbf81c5554dcf41e402007f7a19` 的 Intrinsics.java:665 也编码
unlock→lock→success，signal／broadcast 不必建立唤醒 HB。本次移植这一受限
执行选择，另补 IKOS 的对象／初始化及未建模存储检查，没有照搬其忽略属性
和部分初始化前提的处理。

本次支持前提：

- condition 是可解析的非 TLS global 字节范围，大小／对齐可证；main 在任何
  创建前用默认属性初始化一次，不允许重叠初始化、使用先于初始化。
- wait 的 mutex 是实际路径可解析的 global 常量偏移对象。选定线程必须持有
  它；通过显式 release/reacquire 事件分割临界区，沿用锁身份、所有权和互斥
  编码。同一 condition 的所有所选 wait 要绑定同一 mutex。
- 选择一次合法的有限虚假唤醒及成功返回0。保留原调用和返回值约束，信号不
  充当共享数据值或发生先后证据。实际谓词和每条 loop guard 仍由 SMT 编码。
- 用户普通读写若可能覆盖 condition 控制存储，拒绝确认；库内部字节状态
  没有被伪装成普通 rf 源。startup 同样检查别名。destroy、timedwait、非默认
  属性、隐式 initializer、并发初始化、未知地址／生命周期继续 UNKNOWN。

当前把不同 mutex 的顺序等待也拒绝，是暂时的保守能力边界；POSIX 实际允许
所有 waiter 退出后建立新的绑定。未把这个限制写成永久禁止。
导出的 release/reacquire 标为 synthetic_event，不冒充额外源调用。

验证：build、install 同步/cmp；十个最小例共20条双架构断言，竞争、signal、
helper 从 UNKNOWN→FALSE，公共锁对照 SAFE，其余边界不确认。20条 P0 观察、
20条分配锁和16条 global-choice 回归逐项相同。三个正例双架构参考原生 FAIL，
公共锁对照均 PASS。最终二进制导出的目标、写值／地址／相对顺序固定后，
参考双架构 SAT，改写值 UNSAT_AT_BOUND。仍未绑定所有同步／局部／CFG
occurrence，完整独立验证数量不增加。

1029项冻结集 ILP32 全量：TP78→79/FP0/TN697/FN0/UNKNOWN254→253/error0。
除了 condition 用例恢复，lu UNKNOWN→FALSE，thread-join-counter-outer-race-3
FALSE→UNKNOWN。这两项仍需隔离复测，不把净增长全部归因于 condition 摘要；
保留旧成功用例的全量回退，不能只报告总数。逐文件转移及最终可执行文件哈希见
[汇总](artifacts/bmc-condition-2026-10-10/summary.json)、
[配对](artifacts/bmc-condition-2026-10-10/paired.json)。LP64仅定向回归，未做
全量或可比较性能实验；TP是标签确认，不能替代完整反例验证。

barrier_2t／one_time_barrier_2t 定向仍 UNKNOWN，后续需要堆／栈对象、实际
线程参数和搜索预算等支持，不能因为新增 wait 摘要就放松这些 gate。
