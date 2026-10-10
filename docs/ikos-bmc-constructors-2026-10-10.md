# 构造函数遗漏导致的 BMC 假反例（2026-10-10）

## 已复现的正确性错误

`flag` 静态初值1；GNU constructor 在main前写0。writer持共同mutex写2，
解锁后写data；reader持mutex读flag，解锁后仅在flag==1写data。
reader只可能读0/2，data无竞争，flag访问受共同锁保护。
抽象解释已执行constructor，但入口[0,2]包含1，仍产生may候选。
BMC却仅保留main启动前缀及静态初值1，制造不可能的guard，错误FALSE。
`128a900` ILP32/LP64均复现。gcc -m32/-m64原生assert确认constructor
已在main前把flag改为0；这是启动语义证据，不是竞争validator。

## 修复及前提

bundle中存在`ar.global_ctors`时，`check_race`记录
`global_constructor_prefix`并拒绝确认。该表是前端导入的初始化契约，
不是文件名/benchmark标签。抽象解释原有constructor执行保持不变。
缺少真实pre-main前缀时，不以静态初始化、main激活或任意值代替它。
暂时也拒绝无副作用constructor及可达真竞争；后续可扩展已有路径展开器，
按初始化优先级保留实际constructor前缀、调用/值/线程启动依赖后解除门。
仅禁用初始化常量读取不能覆盖constructor创建线程等其他遗漏。

10条双架构断言覆盖不可达guard、改名/helper/优先级、可达race、无constructor
竞争与对照。正例constructor race UNKNOWN是明确能力边界。

## 固定参考的分歧

固定Dartagnan `7caef5c850dc2dbf81c5554dcf41e402007f7a19`、svcomp.cat、bound2
在这项race-free控制例双架构返回SOURCE_RACE。其processed事件只有
初值`&flag[0]=bv32(1)`、线程写2及读flag，没有constructor写0。
这不是独立验证通过；它对源程序的pre-main行为同样存在模型遗漏。
保留结果与processed事件，不能把参考的FAIL当作本例真实TP。

证据：[修复前](artifacts/bmc-constructors-2026-10-10/before.json)、
[最小回归](artifacts/bmc-constructors-2026-10-10/fixtures.json)、
[参考分歧](artifacts/bmc-constructors-2026-10-10/reference.json)。
原始日志在 `/tmp/ikos-recovery-2026-10-10/constructor-audit/`，不进git。

build/install/cmp通过，双架构P0 20观察逐项相同且核实真实verdict，46条SC
断言通过；冻结集1029项ILP32逐项不变：TP98/FP0/TN696/FN0/UNKNOWN235/error0。
[全量配对](artifacts/bmc-constructors-2026-10-10/paired.json)、
[统计与二进制哈希](artifacts/bmc-constructors-2026-10-10/summary.json)。
未新增完整独立TP验证计数，全部SAFE过滤证明仍未完成。
