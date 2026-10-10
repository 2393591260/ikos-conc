# 真实来源的堆整数读（2026-10-10）

冻结集1029项逐文件全相同：TP101/FP0/TN696/FN0/UNKNOWN232/error0。
这是通用能力补齐，本阶段没有基准TP收益，不能把新正例计入比赛TP。

## 具体任务与能力边界

02-base_24-malloc_races和25-malloc_race_cp先在mutex内读x，再在mutex外
读y；worker在同一个mutex内写x及y。两个任务双架构固定Dartagnan
bound2新跑均SOURCE_RACE，但初始化实际读值的写及mutex顺序会排除
当前目标对：若main先锁，它的x读没有实际初始化写；若worker先锁，
其y写已由锁释放/获取排在main的y读之前。原堆读门解除后公式UNSAT。
26-malloc_struct仍有指针槽cast地址边界，未恢复。
这不是任务标签错误或永久无法实现的结论；是当前受限定义性/内存模型边界。

固定参考WmmEncoder.java:65的memoryIsZeroed默认true；visitReadFrom
允许uninit读并约束其值为零（约653行）。MemoryAllocation.java的
显式malloc初始化选项默认false，不能混淆两种机制。此处没有将该
参考初始内存假设直接移植给IKOS，不能用SOURCE_RACE替代IKOS来源证明。

## 通用实现与证明前提

复用已有main单次malloc身份、实际发布、extent和SC整数编码。
支持精确offset、对齐、1至8字节的整数读与同宽写；整数符号性按类型转换。
按真实allocation身份匹配来源，避免两个heap/local_base=null误当同对象。
静态global初值仅用于global，malloc没有默认值。startup实际同对象写可作为
来源；A/B写通过原clock约束选读之前最后的同址写。只在po/create已确定
顺序时复用现有简化，其他写保留通用SC来源查询。

每个堆读都强制编码来源，包括未参与guard/地址及结果未使用的读。
无来源、循环自供值、混合宽度、指针读、不对齐或复杂地址继续不确认。
整条前缀的堆内存访问也须满足类型ABI对齐，避免unused不对齐写
绕过来源检查；packed对象的合法低对齐访问尚无契约，保守拒绝。
prefix_values_defined只允许将堆load的来源义务交给check_race的这项强制
编码；该函数本身不能证明读值存在。malloc调用/身份和库隐式写门仍保留，
没有SAFE过滤、frontend、filename例外或额外依赖改动。

## 验证与剩余缺口

15个最小用例双架构30条断言通过：startup初始化、实际跨线程写、helper、
宽signed、unused读；wrong-value/distinct-object对照；uninitialized、
循环自供值、partial、pointer、unaligned、unsigned-slot cast边界。
旧main-heap/heap-read-unsupported正例现在恢复FALSE，改名heap-read-race；
26条原堆断言、46条SC、26条byte、16条库来源、34条atomic、20项P0
显式返回码/判定断言全部通过。build/install同步且哈希一致。

两个代表最小正例双架构均参考SOURCE_RACE；startup-initialized所选模型
双架构部分约束SAT。actual-source两项参考绑定chosen target unmapped，
不能算验证成功，也不是模型被证伪。早期单行fixture导致分配源码位置
歧义，调整为分行后重新跑全部fixtures及参考；未放宽mapper的唯一绑定条件。
这些检查仍非完整独立源程序验证。一般heap byte/pointer rf、动态分配/生命周期、
所有SAFE规则及完整witness验证继续未闭环。

[逐项配对](artifacts/bmc-heap-scalar-read-2026-10-10/paired.json)、
[汇总](artifacts/bmc-heap-scalar-read-2026-10-10/summary.json)、
[最小回归](artifacts/bmc-heap-scalar-read-2026-10-10/fixtures.json)、
[任务参考](artifacts/bmc-heap-scalar-read-2026-10-10/reference.json)、
[部分模型验证](artifacts/bmc-heap-scalar-read-2026-10-10/partial-validation.json)、
[三个任务诊断](artifacts/bmc-heap-scalar-read-2026-10-10/targeted.json)。
原始日志、数据库和中间失败在`/tmp/ikos-recovery-2026-10-10/heap-scalar-read/`。
