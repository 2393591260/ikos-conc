# 创建前main写源的SC顺序简化（2026-10-10）

守卫缓存将较大fib推进到实际可行查询，但1000ms SMT仍未确认。
先固定参考确认safe/unsafe10、11（bound26）和12（bound28）双架构
原生SOURCE_RACE，再读代码：main写i/j初值后创建worker，worker循环
atomic加法，main计算局部fib后读取i/j。旧读源collect仍把main初值
当成与worker前驱写自由交错的来源，虽有po/create约束会排除那些顺序，
公式中的选择和符号读没有提前简化。

## 机制和前提

在原exact-region同路径简化上，允许相关对侧写来自main且位于创建
本worker的真实Create事件之前。原公式已有main po与create→child首
事件约束，因此这些main写先于所有child写；child此前有写时选其最新
写，否则选最新main写，再否则用原证明的typed初值。

保留全部collect及递归store值、类型/位宽、地址来源与范围检查。
所有相关写必须精确同offset，load必须constant-offset global；
任一post-spawn、符号或未支持来源仍走原general SC rf或不确认。
不是“最近语法store”猜测，不把抽象may值当具体读值，不跳过未使用
算术/trap/poison，也不移除原po/create/锁/atomic约束。
两前缀内没有其他writer的证明不能升级成全程序private或K线程证明。
source表达式本身仍可携带需要SC求解的依赖，原等式和值流约束保留。

固定Dartagnan对照：`program/Thread::getSpawningEvents`用真实
creator关联start store/load，`encoding/ProgramEncoder::threadIsEnabled`
要求creator实际执行，`cat/svcomp.cat`以po/com约束SC。
IKOS用其已编码的创建边简化选择，并未搬入完整CAT关系。

## 验证

build/install/cmp、P0双架构20条逐项观察相同；46条SC/来源断言、
14条锁干扰断言、34条atomic断言通过，没有frontend改动。
新增main初始化、helper、真实nondet正例实际使用sc_spawn_read_from；
main创建后写正例仍使用general sc_read_from；不可变nondet守卫互斥
安全对照未确认FALSE。六个新例双架构固定参考bound6：五个正例
SOURCE_RACE，对照SOURCE_PASS；全部参考源哈希与最终文件一致。

六个fib定向ILP32恢复FALSE，约2.95–5.22秒。unsafe10/12另外LP64
也FALSE。时间是定向观察，不是公平的整工具性能排名或全局提速证明。
unsafe10的ILP32/LP64四个125事件模型、unsafe12的ILP32两个147事件
模型在参考部分约束查询均60秒TIMEOUT；改值均UNSAT_AT_BOUND。
不能把负向探针拒绝算成原模型验证通过，完整独立验证计数不增加。
完整CFG/call、同步历史及全部局部值也仍未绑定。

首轮全量TP97：新增六个fib，ticket-lock FALSE→UNKNOWN。隔离旧/新
均约8.2秒FALSE，诊断相同且没有使用sc_spawn_read_from。新/旧
阈值附近的搜索预算会受资源负载影响，保留首轮证据和配对，不删回退。
全量复跑TP98/FP0/TN696/FN0/UNKNOWN235/error0，只有六个fib新增，
旧TP/TN全保留；不宣称稳定全局性能。

[全量配对](artifacts/bmc-spawn-source-2026-10-10/paired.json)、
[首轮配对](artifacts/bmc-spawn-source-2026-10-10/first-paired.json)、
[统计](artifacts/bmc-spawn-source-2026-10-10/summary.json)及部分模型证据已保留；一项历史嵌套锁
TN回退仍计入新基线，不会把它静默丢掉。

## 后续

刷新全部剩余UNKNOWN台账，保留哈希一致的历史参考记录并补齐新项；
区分实际unsupported、搜索预算和参考无结论。进一步优化继续先参考
确认、预测具体执行、核实模型，再迁移通用实现。一般heap读、动态
对象与K线程闭包仍需实际来源和生命周期义务，不能直接放开gate。
