# 已释放锁分区写值的入口覆盖（2026-10-10）

本轮在研究不可行路径缓存时加入共同锁保护的共享flag、解锁后访问data的
变体，发现原分析器会漏报。暂停缓存实验，单独修复抽象解释入口摘要。
1029项ILP32配对：TP92→92/FP0/FN0/TN697→696/UNKNOWN240→241/error0。
这不是“没有正确性问题”的证明；新例揭示了冻结集零FN不能覆盖的缺陷。

## 根因和修复

`on_store_privatize`将持锁写延后，unlock把写值放入`_lock_partition`。
此前`ThreadModularAnalysis::materialize_globals`只调用平坦无锁写的
`snapshot_global_board()`；锁分区没有消费者。因此flag初值0保留，
`if (flag) data=1`不可达，两条data访问没有形成候选，最终错误SAFE。
直接访问、pthread实际参数访问、helper及signed char变体均会触发，
并非只有间接参数问题。五个正例双架构旧版均SAFE，新版均FALSE。

改为`snapshot_global_interference()`：在锁内快照所有已发布平坦值和
锁分区值，按对象合并，沿原入口mem_read/join/mem_write通道使用。
不把远端值覆盖本线程值，不按文件、锁名、标签识别，不改变BMC确认门。
摘要忽略发布时间和保护关系，因此扩展入口可达状态，是保守方向。
分区扩展已有dirty通知，驱动原外层fixpoint再次分析线程。
对混合位宽/符号源先选统一类型再逐源cast，避免渐进cast把unsigned
窄值先变负、再错误符号扩展。六种源排列的C++语义断言通过。

Goblint源码对照：`/home/ruan/goblint-analyzer`提交
`bb55b70d91290992629ba3d073185c10cdf356e6`的
`src/analyses/basePriv.ml::ProtectionBasedPriv.read_global/unlock`，
受保护读合并本地CPA和protected全局摘要，release发布实际写。
IKOS没有完整PBR读端及保护证明，本轮选择保守入口摘要，未声称实现
Goblint完整算法或其soundness证明。

## 验证与唯一回退

build、install同步及cmp完成。P0双架构20条逐项与TP92基线相同；
32条原读源/守卫/循环断言、34条atomic断言、14条新变体断言通过。
固定Dartagnan `7caef5c850dc2dbf81c5554dcf41e402007f7a19` bound2，
五个竞争正例双架构SOURCE_RACE，两个安全对照双架构SOURCE_PASS。
共享flag正例选中模型双架构部分约束SAT，改写store值均UNSAT_AT_BOUND。
它们仍未完整绑定CFG、调用及同步历史，完整独立验证计数不增加。

全量唯一TN→UNKNOWN：`28-race_reach_60-invariant_racefree.i`。
main持外层mutex，对x分别在内层global_lock内++/--，释放外层时x回0。
worker持外层mutex读取x，本应只能读0；合并内层释放的中间值丢失外层
排斥/值相关性，产生额外可达分支。新分析器未确认假竞争，返回UNKNOWN。
恢复此TN需要嵌套保护及发布上下文证明，不能重新忽略所有锁分区写值。

## 边界

此修复覆盖已发布锁值的入口通道，未证明后续本地覆盖、跨同步重读、
未释放写、heap内容和所有SAFE过滤的完整并发覆盖。旧validator及部分
Dartagnan模型约束不等于完整见证验证。继续排查这些义务。
缓存实验保存在临时目录，未混入本提交；下一轮独立验证与提交。

[全量配对](artifacts/bmc-lock-interference-2026-10-10/paired.json)、
[统计](artifacts/bmc-lock-interference-2026-10-10/summary.json)、
[新例](review-repros/lock-interference/run.py)、
[原生参考](artifacts/bmc-lock-interference-2026-10-10/native.json)。

C++摘要测试（仓库根）：

```sh
g++ -std=c++14 -Icore/include -Iar/include -Ianalyzer/include -isystem /usr/lib/llvm-14/include docs/review-repros/lock-interference/snapshot.cpp -lgmpxx -lgmp -pthread -L/usr/lib/llvm-14/lib -lLLVM-14 -o /tmp/lock-interference-snapshot
/tmp/lock-interference-snapshot
```
