# BMC ERRORCHECK 属性与成功 owned-unlock 前缀（2026-10-10）

冻结集逐文件 TP102→104，FP0、TN696、FN0、UNKNOWN231→229、error0。
旧 TP/TN 全保留，新增 `04-mutex_23-sound_unlock.c` 和
`05-lval_ls_16-idxunknown_unlock.c`。TP 仍是标签确认、未全部独立验证。

## 由真实执行确定扩展范围

固定 Dartagnan 7caef5c 对两文件 ILP32/LP64 全部 SOURCE_RACE。
第一个所选 IKOS 模型取 nondet=0，m 指向 mutex1；主线程成功锁住并
释放 mutex1，随后读 myglobal，与 worker 的受锁写冲突。
第二个取 nondet=4，成功释放实际持有的 m[4]，随后读 data 与 worker
受锁写冲突。所选前缀只需成功的 owned unlock；目标之后的重复 unlock
未进入该有限竞争前缀。本阶段不建失败 unlock 的返回值或释放效果。

不是照搬参考的 pthread 摘要：`Intrinsics.java:829` 的 attr init/destroy
只写布尔状态，settype 直接成功；其 unlock 摘要也不建 ERRORCHECK 的
实际持有者语义。参考竞争与四个所选模型的部分固定内存约束均 SAT，
改坏所选写值四项 UNSAT_AT_BOUND，但库类型、完整路径/同步顺序等仍有
未绑定义务，不能写成完整独立验证。

## 本实现的证据前提

API 必须是匹配 C ABI 和参数/返回形状的真实外部声明，不凭名字放行。
仅支持 glibc GNU/Linux x86 ILP32/LP64 的 ERRORCHECK=2、attr4、mutex24/40
布局。实际 global（不含 TLS）须有真实声明的 attr/mutex subobject；
支持 struct/array 常量成员和顶层同型 mutex 数组的单仿射索引。
AR opaque 指针通过原 LLVM call layout 核对宽度及对齐；buffer cast
不能证明库对象类型。

沿实际 main 前缀、首次创建前验证 attr init→settype(2)→mutex_init，
destroy 后禁止再次使用属性，重复 init、重复/已使用 mutex 的重新 init、
未知类型、显式篡改属性、并发控制变化、堆/局部属性对象均保守 UNKNOWN。
mutex_init 复制属性，随后销毁 attr 不使 mutex 失效；普通 AR 访问若
别名属性/控制存储则拒绝确认。实际 mutex 的成功解锁仍受原 sections
及 SMT 地址所有权等式检查，不能把失败解锁当成释放锁。

POSIX 说明 mutex_init 的 attr 必须已初始化，否则 UB；成功初始化得到
未锁住对象；ERRORCHECK 非持有者 unlock 返回错误。官方依据：
[pthread_mutex_init](https://pubs.opengroup.org/onlinepubs/9799919799/functions/pthread_mutex_init.html)、
[pthread_mutex_lock/unlock](https://pubs.opengroup.org/onlinepubs/9799919799/functions/pthread_mutex_lock.html)、
[pthread_mutexattr_settype](https://pubs.opengroup.org/onlinepubs/9799919799/functions/pthread_mutexattr_settype.html)。
本地双架构原生合同验证了布局、attr 销毁后仍可锁、正常 unlock 成功、
其他线程/再次 unlock 返回 EPERM。此证据不等于全平台 API 证明。

## 回归及实验边界

build/install/cmp 一致；属性 36、旧库来源 22、SC 46、atomic 34、condition 16
断言，以及 P0 双架构 20 项明确 verdict 核对通过。含改名、helper、成员、
符号数组、共同锁/错误解锁对照及未初始化/销毁/递归/重初始化等负例。
实验中改名用例曾因草稿把候选排列当作时间顺序退回 UNKNOWN；已改为
各动态路径分别检查 prior use，并重新完整验证。失败日志保留在临时目录。
最终二进制四项模型映射证书与参考已核对证书逐字相同。

[全量配对](artifacts/bmc-mutex-attributes-2026-10-10/paired.json)、
[汇总](artifacts/bmc-mutex-attributes-2026-10-10/summary.json)、
[最小断言](artifacts/bmc-mutex-attributes-2026-10-10/fixtures.json)、
[所选路径](artifacts/bmc-mutex-attributes-2026-10-10/targets.json)、
[参考结论](artifacts/bmc-mutex-attributes-2026-10-10/reference.json)、
[部分模型验证](artifacts/bmc-mutex-attributes-2026-10-10/selected-models.json)。
原始材料在 `/tmp/ikos-recovery-2026-10-10/mutex-attributes/`，不进 Git。
未改 frontend、AI、SAFE 过滤或查询预算；完整 witness 验证与全部 SAFE
规则证明仍未完成，本阶段没有因此宣称正确性问题已清零。
