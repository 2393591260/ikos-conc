# BMC 已发布指针槽的纯地址 cast（2026-10-10）

冻结集1029项逐文件不变：TP104/FP0/TN696/FN0/UNKNOWN229/error0。
本阶段扩大通用能力，没有增加比赛TP；未把 malloc 未写字节编成自由值。
TP仍是标签确认，未全部独立验证。

## 从具体输入追到实际阻塞

`02-base_26-malloc_struct.c` 的 main 单次 malloc(8)，将对象写入全局 d，
随后创建 worker。原 LLVM 对 d 都是直接 load/store；AR frontend 却为
结构体及 unsigned 类型的 pointer slot load 加了 bitcast。
`pointer_on_path` 旧代码只认 Load::operand 直接是 GlobalVariable，
所以此文件两架构卡在 prefix_address。

固定参考 7caef5c 新跑两架构均 SOURCE_RACE。预测路径需要区分已写值与
无来源读：worker 在 mutex 中写 x=3、y=8；main 在同 mutex 中读取 x，
释放后读 y。修复 cast 后两个架构已有 SC 来源公式，但仍四次 UNSAT，
没有把该文件恢复为TP。所选两前缀的锁历史及 x 的读源不可同时满足。

为核实参考结果，直接约束参考的 y 读/写竞争对及 main 的 x 读：
x=0 时两架构 SAT，x=3 时两架构 UNSAT_AT_BOUND。这些是参考原生
事件的探针，不冒称 IKOS 所选模型验证。`WmmEncoder.java:65` 默认
memoryIsZeroed=true，653行在 uninit read 时约束读零；此假设未迁移。
当前有来源模型无法确认不等于标签错或源程序SAFE，也不是以后不能
扩展模型的证明。需要先核实无来源读取的 C/LLVM/比赛模型契约。

## 最小通用修改及证明前提

仅增加 pointer-slot 地址归一化：pure global 地址链最终必须是 pointer
类型 global、constant offset=0、无符号下标；拒绝局部、内部字段、未知
或非零偏移。symbolic_byte_offset 仅沿单一定义的值保持 cast/assignment
及常量 shift 追溯，不读共享数据值。

仍由原始 LLVM global 全部 users 证明地址没有逃逸、没有其他写者；
唯一真实 store 必须在所选 main 前缀实际执行，并且早于全部 create。
同一 store 的 AR pointer 用相同规则归一化。读取不能早于该 store，
heap身份、分配次数、宽度、边界、definedness 和实际 SC 供值检查不变。
这是同一不可变已发布槽的精确地址规范化，不是一般 pointer rf/co。

结构体、改名、helper 和 actual-source 正例均恢复；未写 field、再次
发布、逃逸以及共锁对照保留保守结果。此前 unsigned-slot-cast-unsupported
本是合法初始化后读的能力边界，本阶段已恢复，重命名为 race 并核对
双架构固定参考 SOURCE_RACE，未将历史测试限制永久化。

## 验证与记录

build/install/cmp 一致；新16、堆30、属性36、库来源22断言及P0明确20项
双架构 verdict 全部通过。堆 suite 首次因旧 unsupported 预期失败，
核实来源与 unsigned 范围后更新正例并完整重跑；原始失败日志保留。
四类新正例共8项参考 SOURCE_RACE。结构体 field 正例所选模型两架构
部分SAT，改值后两项 UNSAT_AT_BOUND，完整路径/同步验证仍未闭环。
全量配对旧TP/TN全保留；剩余229项再诊断全部UNKNOWN。

[汇总](artifacts/bmc-pointer-slot-casts-2026-10-10/summary.json)、
[配对](artifacts/bmc-pointer-slot-casts-2026-10-10/paired.json)、
[新断言](artifacts/bmc-pointer-slot-casts-2026-10-10/fixtures.json)、
[堆回归](artifacts/bmc-pointer-slot-casts-2026-10-10/heap.json)、
[目标结果](artifacts/bmc-pointer-slot-casts-2026-10-10/targets.json)、
[参考读值探针](artifacts/bmc-pointer-slot-casts-2026-10-10/reference-value-probes.json)、
[部分模型核对](artifacts/bmc-pointer-slot-casts-2026-10-10/selected-field.json)。
原始材料在 `/tmp/ikos-recovery-2026-10-10/pointer-slot-casts/`，不进Git。
本阶段不改frontend、AI、SAFE过滤或查询预算。
