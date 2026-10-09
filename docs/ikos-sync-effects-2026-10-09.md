# 同步 API 参数效果：按确切契约保留用户内存访问

旧 checker 按 pthread_mutex/pthread_cond/sem 等名字家族整调用豁免，未区分同步对象
与调用者的缓冲区。双架构复现五种真实库调用竞争：mutexattr_gettype/condattr_getclock
输出、函数指针在属性getter和无害函数间选择、pthread_create共享句柄输出、
sem_timedwait超时结构读与另一线程写。它们 default/BMC 均错报 SAFE。
另一个 vendor_pthread_mutex_copy 外部调用因包含匹配子串被误豁免；无实现契约时
不能排除它写用户数据，此例检查保守性，不把未知外部实现视为参考工具确认的竞争。

现在外部候选集合中只要存在声明/未知目标，就保留保守用户内存效果，不能因另一目标
有可分析函数体而整调用忽略。同步摘要只适用于确切声明、参数数量、32位int返回及
指定指针槽：同步对象内部状态、once回调描述符、create入口/透传arg可以豁免；
create的pthread_t输出、attr/timeout输入、getter输出继续产生候选。
AR pthread内建保留对应契约。多目标调用不能借集合第一个函数名启用摘要，已有rand/srand
隐藏状态效果对所有可能声明目标求并集。名称变体、错签名或未知摘要走通用保守记录。
用户缓冲区暂按may-read/write记录，未新增精确库返回值或BMC确认语义。

依据为 POSIX Issue 8 原文及本地 pthread.h/semaphore.h：

- [pthread_mutexattr_gettype](https://pubs.opengroup.org/onlinepubs/9799919799/functions/pthread_mutexattr_gettype.html)：初始化attr的类型写入type参数。
- [pthread_condattr_getclock](https://pubs.opengroup.org/onlinepubs/9799919799/functions/pthread_condattr_getclock.html)：通过clock_id指针返回属性值。
- [pthread_create](https://pubs.opengroup.org/onlinepubs/9799919799/functions/pthread_create.html)：成功时在线程句柄指向位置存储新线程ID。
- [sem_timedwait](https://pubs.opengroup.org/onlinepubs/9799919799/functions/sem_timedwait.html)：不能立即获得semaphore时使用abstime超时。

最小例初始化属性、使semaphore初值为0、提供合法输出/时间对象，并允许main在成功创建后
暂停。因此不是依赖初始化错误或“semaphore立即成功而无需读取超时”的伪竞争。
Goblint固定源码的create区分句柄写/attr读/arg不访问，timedwait区分同步对象与abstime读，
可作分层摘要参考；此处仍使用IKOS原有保守访问机制。

固定Dartagnan bound=2 ILP32对共享句柄为FAIL，对两个属性输出与timeout例为PASS。
源码 Intrinsics.inlinePthreadMutexAttr 的get/set仅assignSuccess、没有输出写；另两个
日志明确提示unknown intrinsic。因此这些PASS不能替代POSIX契约，更不能推翻已经复现
的IKOS候选遗漏。这也是迁移参考逻辑前必须核对依赖与库能力的具体依据。

八例×两架构×两模式共32配对断言通过：六个正向/保守性例全部 SAFE→默认抽象竞争、
BMC UNKNOWN；共同锁和局部输出对照保持SAFE。双架构P0 20观察逐项断言、stdio24、
sem12、回绕8断言通过；build/install/cmp一致。1029冻结集 ILP32逐项不变：
**TP61→61（标签统计、未全部独立验证）/ TN697 / UNKNOWN271 / FP/FN/error0**。
[精简证据](artifacts/sync-effects-2026-10-09/summary.json)，原日志在
`/tmp/ikos-sync-effects-2026-10-09/`，不进入git。

```bash
python3 -B docs/review-repros/sync-effects/run.py --output /tmp/ikos-sync-effects-NEW
```

同步对象init/destroy/属性与有效性、未知外部内部状态、free生命周期冲突、结构偏移和
抽象锁身份等仍须继续审查。本次不宣称所有SAFE过滤均已证明；此前free处理保留，
原“free只属于memsafety”的无条件论证不能作为完整C并发语义证明。
