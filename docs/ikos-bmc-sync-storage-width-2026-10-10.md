# BMC 同步控制存储的完整宽度（2026-10-10）

## 新发现的假反例

已提交的库值来源门只挡住 mutex 第一个字节：AR frontend 把真实
`pthread_mutex_t*` 参数变成 intrinsic 的 `si8*`，编码器按 AR pointee
计算 footprint，得到 1。默认锁成功后，glibc 会向 offset 8 写入
非零线程 ID，但读取 `mutex.__data.__owner` 仍被旧 BMC 用静态零供值。
释放锁后以 `owner == 0` 为条件访问 data，是不可达的竞争路径。
HEAD `eadf7b2` 的直接读取和 helper 变体，ILP32/LP64 四项均错误 FALSE。
这不在冻结集里，说明基准 FP=0 不能证明反例可靠。

本地 glibc x86 32/64 两架构原生断言通过；实际 libc 的成功路径均向
对象 offset 8 写入线程 ID。此表示是本地实现合同，非 POSIX 可移植定义。
符号数组的显式 mutex_init 已有完整 footprint，旧/新均 UNKNOWN，
不能把它算作新修好的 FP。

## 通用修复及边界

通过 AR statement 保留的原 LLVM CallBase 取原始参数 pointee，结合
模块 DataLayout 恢复完整 allocation size（含 padding）。同一 helper
用于锁事件边界和所有 opaque 库控制存储 footprint；condition wait 的
第二参数同样使用原始 mutex 类型。AR `si8*` 不再作为库修改宽度的证据。
原始类型缺失、opaque、不定长、byte 指针或零大小时回退 UNKNOWN。
普通 pthread_t 输出与 join 返回槽仍使用原有输出宽度路径。

继续按真实对象、offset、共同 stride 判断普通访问是否别名库存储；
别名时拒绝确认，不给 mutex 内部字段制造 SC 值源。不修改 frontend、
SAFE 过滤、rf/co 或互斥定义；未实现库内部字段的具体值摘要。
这些前提只覆盖宽度门，不能据此声称已证明全部锁生命周期或库 ABI。

## 验证

build、install 同步及 cmp 通过。库来源 22、SC 46、atomic 34、condition 16
断言及 P0 明确的双架构 20 项 verdict 检查全部通过。
两个旧 FP 的直接/helper 变体均退回 UNKNOWN，数组对照保持 UNKNOWN；
同 struct 非 mutex 字段竞争及其他既有正例保持 FALSE。
冻结集 1029 项逐文件不变：TP102/FP0/TN696/FN0/UNKNOWN231/error0。
全量配对及二进制 hash 见下列汇总。
TP 仍是标签确认、未全部独立验证；全部 SAFE 规则证明仍未完成。

[修复前](artifacts/bmc-sync-storage-width-2026-10-10/before.json)、
[定向断言](artifacts/bmc-sync-storage-width-2026-10-10/fixtures.json)、
[P0](artifacts/bmc-sync-storage-width-2026-10-10/p0.json)、
[原生 ABI 证据](artifacts/bmc-sync-storage-width-2026-10-10/abi.json)、
[逐项配对](artifacts/bmc-sync-storage-width-2026-10-10/paired.json)、
[汇总](artifacts/bmc-sync-storage-width-2026-10-10/summary.json)。
原始日志与二进制在 `/tmp/ikos-recovery-2026-10-10/mutex-attributes/`，不进 Git。
