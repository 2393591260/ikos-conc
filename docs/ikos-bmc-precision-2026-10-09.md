# BMC 精度恢复与 IKOS 事实接口（2026-10-09）

本轮在[正确性收紧基线](ikos-bmc-correctness-2026-10-09.md)之上恢复精度。没有撤销未知值、未知调用和线程实例存在性检查，也没有按文件名、预期标签或 benchmark 家族设置语义例外。

## 1. 结果及证据范围

| 2026 冻结集，1029 项 | TP | FP | TN | FN | UNKNOWN | 超时/错误 |
|---|---:|---:|---:|---:|---:|---:|
| 正确性收紧基线 | 7 | 0 | 709 | 0 | 312 | 1 |
| 本轮最终实现 | 35 | 0 | 709 | 0 | 285 | 0 |

正常模式和完整诊断模式的 1029 个 verdict 全部一致。逐项配对：原 7 个 TP 和 709 个 SAFE 全部保留，28 个 UNKNOWN 转为 RACE，284 个 UNKNOWN 保持，原 1 个超时转为 UNKNOWN。超时的变化不作为稳定性能收益。

上一轮撤回的 23 个旧 TP 中恢复 19 个，仍未恢复 `04-mutex_23-sound_unlock`、`04-mutex_27-base_rc`、`floating_read-5`、`bigshot_p`。撤回确认不表示任务无竞争，也不表示每条历史反例都已证明虚假。

全量均为 YAML 指定的 ILP32，不能宣称做了 1029 项 LP64 全量。局部回归覆盖 ILP32/LP64，共 208 条断言：正确性 68、地址 28、候选 30、值语义 82，全部通过；另外明确核对了历史 P0 用例的 20 条输出。`published-before-init` 在 BMC 模式仍为 UNKNOWN，不能误写为已恢复堆竞争确认。

这些是有限实验和选中源码前缀审阅。**独立 witness 验证尚未完成；没有获得全项目 soundness 证明。** 得分正确的 TP 不足以独立证明其 SMT 反例真实。

复现命令：

```bash
cmake --build build --target ikos-analyzer -j"$(nproc)"
cp build/analyzer/ikos-analyzer install/bin/ikos-analyzer
IKOS_BMC_RECOVER=1 python3 -B svcomp_race_test.py --demote
```

数据集 HEAD 为 `0a254fdddcfe24949be9edd47f8d609462eb52fb`；2027 目标清单尚未固定。项目 HEAD 为 `51b795c4b09f36170a69be4e5ce0a10345db2de6`，本轮代码未提交。安装与构建 analyzer 的 SHA256 均为 `3043999b0e36620f977aeb8d397afeb3921bc4d445d78df5945eb80ccbd8424c`。

## 2. 实现及其语义前提

### 动态值、参数、返回与地址

`unroll.hpp` 沿实际调用/返回栈绑定实参、线程创建参数和 helper 返回，查找本次 invocation 中实际到达的最近定义。值缓存以变量与动态定义位置区分，不同线程实例使用独立缓存/前缀，同一路径的 guard 与地址共用缓存。缺少真实定义或绑定时，在命中缓存前拒绝，不借旧 invocation 的值确认新访问。

指针沿实际参数解析为对象及仿射偏移；不能解析的函数指针、共享指针 load、未知对象仍不确认。普通有定义的 assume wrapper 展开实际 CFG，不能因名称像 assume 就跳过函数体。

### 机器整数与前缀定义性

`encode.hpp` 按类型位宽和符号编码算术、位运算、转换、除余和移位，约束除零、signed MIN/-1、移位范围、exact 和 no-wrap 等前提。即使 BinaryOperation 的结果未进入 guard/地址，也检查其定义性。真实 nondet 可以提供自由值；`freeze_uninit` 的合成初始化、未知 load 或未知调用不能冒充真实 nondet。

对于满足范围约束的 no-wrap 加减乘，可直接保留数学整数结果，避免等价的 Int/BV 往返。wrapping、收窄和位操作仍保留精确机器整数语义。此优化依据操作前提，不依据某个任务的耗时或标签。

### 初值和按需有界 SC 读来源

新增 `initial.hpp` 复用 AR 全局 initializer 与 IKOS DataLayout，读取确切偏移/位宽的整数初值，包括嵌套 struct/array 与 aggregate zero；undef、padding、pointer、未知或部分重叠初始化回退。

guard、地址或定义性需要某个 load 时，将其绑定到同一 SC 顺序里的最近先行写。来源为 AR 初值、实际 main 创建前缀的写，以及选中 A/B 前缀的写；所有所建模内存事件时钟互异，同线程未来写不能供值。main 的 nondet 初始化只产生一个符号，供两 worker 共享，不能让两线程各自猜一个初值。局部栈 load 仅使用本路径最近 allocation 之后的真实写。

任一依赖无法编码时整对不确认。变量地址供值、部分重叠供值、指针 load、创建前缀中的 demanded load 等仍不支持。这是**受限的、按需的有界 SC 值流**，不是一般 K 线程全程序 rf/co，也没有实现弱内存或原子 RMW。

### 锁、调用和创建

锁沿实际参数解析，以精确的全局对象及常量偏移分配共同身份；删除旧 function-level/held 提前门，使用完整临界区历史。支持 rdlock/wrlock/unlock：reader-reader 不互斥，涉及 writer 的区间互斥。未知属性、其他 rwlock API、重入和不平衡历史仍保持 UNKNOWN。完整 verifier atomic 段排除其他线程所有事件；未闭合目标段仍不确认。

仅给极小 stdio 子集摘要：不可变、终止的常量格式，puts、普通文本/`%%` 和受支持的 promoted 32-bit 整数格式。`%n`、`%s`、动态宽度、可变格式及返回值依赖不忽略；新增 `%n` 正/负对照，防止其副作用被当作 no-op。

main 创建前缀保留直线快路径，另用实际 CFG 展开最多 8 个候选前缀；必须含所需的真实创建实例，guard 与 worker 条件一同求解。可支持首次迭代到创建，不能猜整个循环迭代数。startup 中未知副作用、Join、Lock/Unlock 等仍拒绝。修复创建前缀缓存的正反向候选键不一致：统一 `std::minmax`，此前 `qw2004_variant` 的 `map::at` 崩溃已消除，该任务当前 UNKNOWN。

## 3. IKOS 能否继续向 BMC 提供有效信息？

**可以，已有实际接口和消费记录，但更广泛的收益尚未证实。** `facts.hpp::CandidateFacts` 从 checker 经 `confirm_race` 传到 SMT：

| 信息 | 当前用途 | 本轮全量观测 |
|---|---|---|
| AccessRecord 的 may-offset bounds | 约束已有真实值来源的候选地址，不替代值来源 | `ai_candidate_bounds` 256 次，49 项任务 |
| checker 保存的真实 main 创建前缀 | 证明实例存在，提供共同初始化和实际控制条件 | 成功序列随 `confirmed_startup` 保存 |
| 受限局部 no-wrap 证书 | 跳过算术前缀的重复定义性检查；被使用的结果仍精确编码 | 全量消费 **0**；两架构最小正例通过 |
| AR initializer 与 DataLayout | 精确全局整数初值及字节布局 | 与 demanded SC 读来源共同使用 |

全量另记录 `sc_read_from` 648 次/43 项，`integer_operation` 199 次/35 项。这些是编码消费记录，不能将调用次数当作独立收益或因果证明；主要 TP 恢复来自真实语义与依赖绑定。本轮没有做逐条禁用事实的全量消融。

**共享读区间目前不能直接输入 BMC 当作具体值。** `execution_engine/numerical.hpp` 的共享整数干扰在入口 materialize，thread-local memory read 可能过时；本轮观察到简单竞争递增中两线程读区间均为 `[0,0]`。据此限定所有读值会漏掉实际交错。要扩大值证书，必须先核实跨线程不动点、读时干扰及证书适用上下文。

局部 no-wrap 证书只覆盖纯局部常量、真实 nondet 和 SSA 运算，要求 owner 的控制条件不依赖 load/formal/phi/隐藏 helper，并对已检查上下文合取。这个受限接口证明了可接入，尚未证明其可扩展性和全量收益。

后续协同优先方向：用可靠的 may-alias/offset/线程信息缩小依赖候选；用 must 信息在明确的执行条件下简化同步；对纯局部关系和循环建立可检查证书。may 事实可限制可能集合，不能凭空证明某个线程/事件一定存在，must-edge 也不代表两端事件必执行。

## 4. 选中反例的源码复核

35 项任务的 44 组成功访问对、A/B 前缀、创建序列与 SMT model 已保存为 `confirmed-pairs.json`。源码审阅的关键机制包括：

- 简单递增/字段/数组：确切初值、合法算术和同对象；锁不同或一方无锁。变量下标存在满足 guard 的真实取值。
- helper/同入口实例：实际 mutex 和指针参数；两个实例的局部值独立。join 相关任务的竞争目标实际在 join 前，不把结束排序套到提前访问。
- rwlock：确认 reader-reader 期间的真实冲突；`read_write_lock-1b` 确认的是 `y`，不是受 writer 锁保护的 `x`。
- `scull`：main 实际初始化后创建；无锁 loader 与持锁 worker 的早期 `scull_trim` 访问冲突，没有自由 shared load 或猜测循环退出。
- `fkp2013`：首次循环创建即足够支撑访问前缀，不宣称已展开全部 50 次。
- `qw2004-1`：选中 stoppingEvent 写/读；计数更新和 atomic 段完整支持该交错。`qw2004-2` 有拆分 atomic 读写导致的真实丢失更新前缀。
- lamport/szymanski/peterson/Boop：包括早期共享 flag/turn 的竞争；不能把确认这些访问解读为证明协议最终互斥失败。

以上是按所选模型审阅源码，不是一个独立的执行重放器。

## 5. 性能与剩余问题

同前端、前后 analyzer 各交替测量 3 次：`qw2004-1` 由 10.46–11.82 秒（两次 UNKNOWN、一次 RACE）变为 0.43–0.59 秒（全 RACE）；`qw2004-2` 由 1.63–1.82 秒变为 0.29–0.34 秒（均 RACE）。这是含前端的局部壁钟样本，早期测试可能与诊断尾部共享资源，不能推广为全量或普适加速。

当前优先问题仍是独立反例验证及 C/LLVM/AR 契约核对，再扩大动态创建、堆对象身份/生命周期、循环、K 线程和同步摘要。UNKNOWN 原因可以重叠，解除一个 gate 不保证恢复该任务。自动审阅不能可靠替代跨模块 soundness 证明，扩大语义前应保留可检查的证据边界。

## 6. 永久材料与参照

[本轮材料目录](artifacts/bmc-precision-2026-10-09/)保存全量结果、1029 项 YAML/input hash、配对转移、44 组反例、原始诊断压缩包、208 条回归日志、P0 明确核对、局部性能实验、源码快照和相对 HEAD 的 patch。快照含此前未提交改动，不应全归因于本轮。

原理参照固定 Dartagnan `7caef5c850dc2dbf81c5554dcf41e402007f7a19` 的 ProgramEncoder/ExpressionEncoder/LoopUnrolling。此前依据遗留 `target/classes/git.properties` 将本地运行包判断为 4.1.0/2025 有误：实际 launcher 与 library JAR 均为 4.3.1 / `7caef5c…`，重新核对的 hash 和配对运行见[参考迁移材料](artifacts/bmc-reference-guided-2026-10-09/manifest.json)。不能用目录名或未执行的构建产物证明版本。Goblint 参照为 `bb55b70d91290992629ba3d073185c10cdf356e6` 的整数 interval/overflow 实现。

论文参照：CAV 2019 *BMC for Weak Memory Models: Relation Analysis for Compact SMT Encodings*（may/active pair 关系候选压缩）；OOPSLA 2023 *Static Analysis of Memory Models for SMT Encodings*（外部分析 filters、以事件执行为条件的 may/must）。其算法启发了事实接口方向，但本文没有移植完整关系分析，也不引用论文的性能数字作为本项目结果。版本、URL、内容 hash 和阅读范围见本轮 `references/manifest.json`。
