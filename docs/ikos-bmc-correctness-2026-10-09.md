# BMC 正确性审查与回归（2026-10-09）

本轮优先撤回没有充分执行证据的 FALSE，再恢复有依据的确认。最终本地冻结集结果为
**TP=7、FP=0、TN=709、FN=0、UNKNOWN=312、超时=1**。
当前 7 个确认任务均已按实际选中访问对构造、复核有限源码执行前缀；这不是全项目 soundness
证明，也不是独立 witness 验证通过。撤回的任务仍有官方竞争标签，不能把撤回等同于标签错误或不存在竞争。

## 1. 范围与可重复证据

- HEAD：`51b795c4b09f36170a69be4e5ce0a10345db2de6`，包含未提交的地址 A1、候选搜索及本轮修复，未创建 commit。
- 数据集：`/home/ruan/sv-benchmarks-2026`，提交 `0a254fdddcfe24949be9edd47f8d609462eb52fb`。
- 命令：`IKOS_BMC_RECOVER=1 python3 -B svcomp_race_test.py --demote`；默认 8 jobs、30 秒每任务。
- 本地脚本选中 1029 个任务，全部 ILP32。两种位宽的小回归不代表 LP64 全量测试，也不代表已经核实 2027 官方类别任务清单。
- 构建完成后已同步 analyzer、LLVM preprocessor 和 witness writer，构建/安装文件逐字节一致。
  LLVM/Clang 使用本地 14.0.6 工具链。

所有永久证据保存在 [artifacts](artifacts/bmc-correctness-2026-10-09/metadata.json)：

| 文件 | 用途 |
|---|---|
| [metadata.json](artifacts/bmc-correctness-2026-10-09/metadata.json) | 构建/源码指纹、配置、矩阵、验证范围 |
| [source.patch](artifacts/bmc-correctness-2026-10-09/source.patch) | 相对 HEAD 的相关代码和新回归快照；含此前授权的未提交改动 |
| [baseline-dirty.patch](artifacts/bmc-correctness-2026-10-09/baseline-dirty.patch) | 本轮开始时已有的改动，区分工作阶段 |
| [transitions.tsv](artifacts/bmc-correctness-2026-10-09/transitions.tsv) | 同一 1029 个任务的配对 verdict；JSON 同目录 |
| [confirmed-pairs.json](artifacts/bmc-correctness-2026-10-09/confirmed-pairs.json) | 最终 7 项的所有已确认组、选中访问对、创建位置、路径和 SMT model |
| [audited-tasks.json](artifacts/bmc-correctness-2026-10-09/audited-tasks.json) | 旧 27 项和新 3 项的最终诊断；逐任务原始报告和日志在 `tasks/` |
| [minimal-ab.json](artifacts/bmc-correctness-2026-10-09/minimal-ab.json) | 已核实旧 binary/wrapper 的最小用例 A/B |
| [consumption-final.json](artifacts/bmc-correctness-2026-10-09/consumption-final.json) | 本轮正确性回归的 68 条断言结果 |

最初一次临时 wrapper 错误设置了 `settings.IkosAnalyzer`，未选中旧二进制；那次“old”观察已弃用。
这里的有效 A/B 使用 `settings.ikos_analyzer` 和 `settings.ikos_pp`，旧 analyzer SHA-256 为
`e47d8811d0971af28ec354551dfe899d3c1b1c51fb10764899eb459aa600056a`。
最终 analyzer SHA-256 为 `3e2262aa7e2e1177d56aa40ed97a838f07396d5dd5e65d29f44e4466a18a868f`。

## 2. 发现与修复

| 问题 | 原来缺少的前提 | 当前处理及边界 |
|---|---|---|
| helper 返回后丢失前缀 | 调用之后的访问必须经过 callee 的真实返回路径、条件和同步 | 拼接每条实际 return 前缀；未知、循环或超深调用无摘要时不确认 |
| assume 实参被猜测/遗漏 | 被调用的 assume 要求实际实参非零，不能以附近比较替代 | 编码实际实参；不支持收窄等转换时保持 UNKNOWN |
| 锁指针被伪装为不同身份 | 未解析指针可能别名同一锁 | 只认可可证明的全局地址和常量偏移；未知身份不确认 |
| 只保留最后一段锁历史 | 所有已执行临界区都应遵守互斥和所有权 | 保存完整及未闭合的每段临界区；未知身份、重入和无持有证据的 unlock 不确认 |
| 猜测循环界、删条件或忽略陷阱 | 到目标的所有条件和操作必须可执行 | 删除猜测循环退出，保留 CFG 条件并截断回边；未建模 no-wrap、除法/移位等阻止确认 |
| may-callee 或静态定义代替执行依据 | 确定 callee；定义在选中动态前缀中且早于使用 | 函数常量及赋值/bitcast 链；值编码在 cache 命中前检查动态定义重复和先后关系 |
| 合成未初始化值被当源 nondet | `freeze_uninit` 的抽象初始化不能创造真实源调用 | 对合成 call/store 加 provenance 标记；值编码只接受有源码来源的受支持 nondet |
| 只检查 guard/address 的值来源 | 写入值、返回值、调用实参及 main 创建前缀也不能真实消费 synthetic/undef | `prefix_values_defined` 覆盖全部实参；局部读需要本次 allocation 后真实覆盖写入，或受支持的创建句柄输出 |
| 创建站点数量代替成功激活 | 句柄输出对象有效、参数受支持、选中创建前缀可执行 | 当前只确认默认属性、已知句柄输出对象的成功分支；未知参数/属性保持 UNKNOWN |
| verifier 原子段只当 mutex | 该段不能被其他线程打断，包括原子段外的访问 | 完整段排除另一线程所有事件；尚未闭合的目标前缀需要尾部闭包，当前保持 UNKNOWN |

最小合法负对照已在旧实现复现 FALSE：helper 中的互斥共享守卫、helper assume、嵌套 helper、
普通默认 mutex 连续加锁及其 helper 变体，以及 `value == 256; assume((unsigned char)value)`。
新版不再确认；旁边的真实竞争对照仍能确认。实现不使用文件名、任务标签或 benchmark 家族作为语义条件。
回归还重命名全局、helper、入口函数，验证规则不依赖这些名字。

未初始化值写入、volatile 局部读取、helper 返回及调用实参在收紧前也能触发确认。
这些输入涉及 C 未定义行为，当前作为“不支持的源值来源”边界测试，**不计作标准 C 下合法、race-free 的 FP 证据**。
窄类型 undef 有时已在前端消除，甚至不会进入 BMC；也不能把这种 SAFE 当成 BMC 的正确性证明。

定义性检查与值流编码是两件事：证明某次局部读取有真实初始化，不等于允许它作为自由 SMT 值；
共享 load 在守卫或地址中仍需实际 rf/值流证据。这里没有把共享读绑定“最近写入”。
合成初始化仍可能出现在 AR/SMT 诊断路径中，属于抽象 bookkeeping；构造源码执行时须删除这些事件，
不能把每条 AR 都当作真实源程序指令。现有来源检查拒绝真实消费它们。

## 3. 全量 A/B：精度损失必须单独记录

| 版本 | TP | FP | TN | FN | UNKNOWN | 超时 |
|---|---:|---:|---:|---:|---:|---:|
| 本轮开始 | 27 | 0 | 709 | 0 | 292 | 1 |
| 最终 | 7 | 0 | 709 | 0 | 312 | 1 |

配对结果：旧 TP **保留 4、撤回 23**；原 UNKNOWN 中 **新增 3** 个确认任务，净减少 20。
新增三项是 `lamport-b`、`szymanski-b`、`27_Boop_simple_vf`，依据是更早的真实竞争访问，
不是重新放开自由 load，也不是确认后面的协议互斥失败。最后一次 store/return/call 来源补查没有进一步减少 TP。

23 项撤回原因可在逐任务诊断中核实：未证明的 no-wrap 前缀、未支持调用/值来源、循环目标不再靠猜测到达、
未建模同步、未证明创建和未闭合原子段等。原因可重叠；观察到阻塞不代表已经证明旧反例虚假，
也不能直接估算解除某个 gate 会恢复多少 TP。`scull` 本轮仍为 UNKNOWN。

235 个任务预期存在竞争，最终仅确认 7 个，另 228 个为 UNKNOWN；
794 个任务预期安全，其中 709 个 SAFE、84 个 UNKNOWN、1 个超时。
脚本输出的 `Recall=1.000` 排除了 UNKNOWN，**不能解释成已检出全部竞争任务**。
超时输入为 `linux-3.14--drivers--net--irda--nsc-ircc.ko.cil.i`。

## 4. 最终 7 项的有限执行依据

表中行号对应冻结集实际输入，全部已确认组的精确访问对见 `confirmed-pairs.json`。
对涉及两个子线程的任务，main 可成功创建相关实例后暂停；对 main/子线程任务，创建已在选中 main 前缀内。
库对象和源数据在这些具体前缀中已检查；表内描述的是具体存在性证据，不是一般程序定理。

| 任务 | 实际冲突与一条可行前缀 |
|---|---|
| `04-mutex_21-sound_base` | main 成功创建 t_fun 后，在 26 行写 `f=bad`，与 t_fun 在 19 行读取 `f` 相邻；竞争发生在函数指针读写，尚不需要执行间接调用 |
| `04-mutex_47-fun_write` | t_fun 持 mutex1，调用 `f()` 实际返回 5，21 行写 myglobal；main 持另一把 mutex2，在 30 行读 myglobal；可让写后立即发生读，后面的递增不在本目标前缀内 |
| `04-mutex_50-funptr_rc` | t_fun 持 mutex1，在 22 行写 `fp=f2`；main 持不同 mutex2，在 31 行读取 fp；同样只需证明读取前缀 |
| `lamport-b` | 两线程首次循环仅各自写 `b1/b2=1`，随后 680/706 行分别写 `x=1/2`；可把两个 x 写排成相邻，尚未消费后续共享守卫 |
| `peterson-b` | 679/688 行写 turn；以及 678/689 行 flag1 写/读、680/687 行 flag2 读/写。此前只有常量 flag/turn 写；目标读后的循环比较尚未执行 |
| `szymanski-b` | 677/694 行 flag1 写/读、678/693 行 flag2 读/写；同样是早期访问，不靠后续协议可达性猜测 |
| `27_Boop_simple_vf` | main 先创建 thr1，再执行循环首次创建 thr2 即可暂停；thr1 常量写 usecount、locked 后在 721 行写 release_thr1，thr2 在 692 行首次读取 release_thr1；无需证明它退出自旋 |

这些源码前缀能明确赋予真实读值并安排访问顺序，不要求后续程序终止。
目前已检查选中源码、路径顺序、目标位置和 SMT model；**尚未通过独立验证器验证所选执行**。

## 5. 验证与剩余正确性工作

已完成：

- `bmc-correctness/runtest`：34 用例 × ILP32/LP64，共 68 条断言；含 `--inline-all` 的来源变体。
- `bmc-candidates/runtest`：30 条断言；`bmc-address/runtest`：28 条断言。合计 **126 条通过**。
- 历史 P0 runner：两架构共 20 条观察，已核对退出码和语义预期；runner 自身不提供断言。
  宽度用例及其同起点对照都确认竞争；发布前初始化用例 default 不报 SAFE，demote 后堆候选仍可 UNKNOWN。
- 冻结集 1029 项最终全量；旧 27 与新 3 项另开诊断，verdict 均与全量一致。
- witness 锚点指向实际成功候选，创建位置只记录所选前缀；源码位置不再错误附上线程入口函数名。

尚未完成的事项决定了保证的上限：

1. **独立验证与比赛格式**：现有 writer 生成 YAML 2.2。本轮查阅的
   [SV-COMP 2026 官方规则](https://sv-comp.sosy-lab.org/2026/rules.php) 在 no-data-race violation 栏只列 GraphML 1.0。
   本地 CPAchecker 4.2.2 对 2.2 的解析失败不是反例有效或无效的结论。
   下一步应导出实际所选创建、访问和必要值约束，在兼容验证器中检验；不能让验证器另找一个 race 就算原反例通过。
   2027 规则未固定，届时必须重新核对。
2. **源程序契约**：本轮补了 synthetic/undef 消费，未完成全部 C/LLVM/AR undefined/poison、
   库错误路径、锁属性、别名和生命周期的通用证明。新增确认能力仍须逐项建立这些前提。
3. **SAFE 分支**：本轮主要审查 FALSE 执行依据，没有证明全部抽象域、fixpoint 和 SAFE 过滤规则。
   全量 FN=0 是有限观察。
4. **恢复精度**：先补机器整数的可执行性/值编码和必要初值；再补参数、返回及动态调用/创建身份。
   共享值守卫和地址最终需要候选依赖闭包及与同一 SC 执行顺序一致的 rf/co。
   原子段内目标需要尾部闭包。不能直接撤销这些 gate 来恢复历史分数。

理论对照使用固定 [Dartagnan 提交](https://github.com/hernanponcedeleon/Dat3M/tree/7caef5c850dc2dbf81c5554dcf41e402007f7a19)
的 ProgramEncoder、PropertyEncoder、LoopUnrolling，并结合上述官方 nondet/atomic 规则。
已修正主文档及参照文档中“相邻等于无 hb”“猜测循环展开等价”等误述；源码相似或术语相同均不构成等价证明。
