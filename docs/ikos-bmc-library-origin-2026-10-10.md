# BMC 隐式库写入的值来源边界（2026-10-10）

## 已复现问题与适用环境

reader成功取得默认glibc mutex，读取`mutex.__data.__lock`，release后仅在
读到0时写data；另一个writer写data。mutex内部字段在成功持有时为非零，
而BMC原先只有lock顺序，没有控制字修改，仍用静态初值0作为SC读源。
`7c0fece`双架构错误FALSE。固定参考对控制例双架构SOURCE_PASS。

这是本地glibc x86 ABI的C程序，不将`__data.__lock`内部字段的表示推广为
POSIX可移植契约。本地gcc -m32/-m64 pthread原生assert通过；两份libc
反汇编默认锁路径均向offset0的控制字写1后成功返回。运行和ABI检查是
前提证据，不是完整竞争validator。[库哈希](artifacts/bmc-library-origin-2026-10-10/abi.json)。

同类证明义务包括`pthread_create`输出pthread_t、非空join返回槽等，它们
也是隐式写，没有普通AR Store。不能因为全局integer有静态初值就宣称
该初值在库调用后仍是合法读源。句柄测试只检查unsupported回退，没有
假定POSIX线程ID必须非零，也不把这些输入统一标为真实race-free。

## 通用边界

按实际所选startup/A/B调用参数解析对象、offset、写宽度。mutex/rwlock、
mutex_init及condition控制存储不允许与普通显式内存事件混用；条件控制
原有初始化/所有权检查保留。输出句柄/返回槽仅在值被guard、算术或地址
要求时拒绝，其正常join身份读取不因此整路径封死。常量范围及共同stride下不跨周期的字段余数可证明不相交；其他符号offset
或无法证明不相交时保守拒绝。相同数组的不同record实例不会把普通字段
误并为mutex字段；同一字段的符号索引仍不能借此忽略库更新。

局部存储比较限定为同一动态路径；相同AR局部指针不能合并两个线程实例。
heap使用已证明的分配身份，global用实际global身份。尚未建库输出的
具体值摘要，也没有把隐式写伪装成nondet。当前门可在以后实现受验证的
库值摘要和生命周期后解除。未改frontend/AI/SAFE过滤。

16条双架构断言覆盖默认mutex内部读、改名/helper、移除内部读的竞争、
两项输出句柄guard、同struct不重叠字段、符号数组字段竞争及内部读对照。46条SC、20条P0观察通过且
逐项核实；build/install/cmp一致。冻结集1029项ILP32逐项不变：
TP98/FP0/TN696/FN0/UNKNOWN235/error0。

[修复前](artifacts/bmc-library-origin-2026-10-10/before.json)、
[最小回归](artifacts/bmc-library-origin-2026-10-10/fixtures.json)、
[参考](artifacts/bmc-library-origin-2026-10-10/reference.json)、
[全量配对](artifacts/bmc-library-origin-2026-10-10/paired.json)、
[统计](artifacts/bmc-library-origin-2026-10-10/summary.json)。
原始日志/反汇编在`/tmp/ikos-recovery-2026-10-10/library-origin/`，不进git。
全部SAFE证明及完整独立TP验证仍未完成。

首轮门对所有符号offset一律拒绝，导致tricky_address4 FALSE→UNKNOWN、
TP97。复核后补充通用共同stride字段余数不相交证明及两个变体，最终
全量TP98且逐项全保留。首轮[配对](artifacts/bmc-library-origin-2026-10-10/first-paired.json)
保留，最终结论使用上述新二进制hash，不能把首次失败说成验证通过。
