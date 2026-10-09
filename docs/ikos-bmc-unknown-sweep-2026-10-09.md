# UNKNOWN 全量遍历与参考驱动精度恢复

本轮按固定的 2026 冻结集、各任务 YAML 机器模型和 no-data-race 性质，遍历原来的
285 个 UNKNOWN。每个任务先运行固定版本 Dartagnan，再对已确认竞争的任务尝试
通用 IKOS 扩展。新增 UNKNOWN 也补充参考分析。本轮没有修改任务、标签、提交包或远端。

同任务清单、同机器模型、4 个 IKOS 工作进程、每任务 30 秒的配对结果：

| 项目 | 修改前 | 修改后 |
| --- | ---: | ---: |
| TP | 35 | **58** |
| FP | 0 | **0** |
| FN | 0 | **0** |
| TN | 709 | **698** |
| UNKNOWN | 285 | **273** |
| 错误/超时 | 0 | **0** |

原来的 35 个 TP 全部保留；23 个原 UNKNOWN 恢复为 TP，全部有固定配置 Dartagnan FAIL。
11 个原 SAFE 退为 UNKNOWN，来自更保守的动态实例/已 joined 覆盖及 aggregate interference。
不能只报告 TP 增长而隐去这个代价。按 TRUE +2 / FALSE +1 的简单正答计数，净变化为 +1；
这不是正式比赛得分，后者还依赖 witness 和官方任务集合。两项最小 FN 变体已修正，
但有限回归 FP/FN=0 不等于普遍 soundness 证明。

每项均有独立记录，包含原始两端命令/日志引用、候选源码、阻塞、实际尝试和剩余前提：

- [296 条逐任务总表](artifacts/bmc-unknown-sweep-2026-10-09/ledger-pruned/results.tsv)
  与 [JSON 总表](artifacts/bmc-unknown-sweep-2026-10-09/ledger-pruned/results.json)。
  覆盖原 285 项及新增 11 项；最终 UNKNOWN 为 273，另 23 项已恢复。
- [修改前全量](artifacts/bmc-unknown-sweep-2026-10-09/full-before-paired/summary.json)
  / [修改后全量](artifacts/bmc-unknown-sweep-2026-10-09/full-pruned/summary.json)。
- [参考全量](artifacts/bmc-unknown-sweep-2026-10-09/reference/summary.json)
  / [补充 10 项](artifacts/bmc-unknown-sweep-2026-10-09/reference-supplemental/summary.json)
  / [补充 aggregate 任务](artifacts/bmc-unknown-sweep-2026-10-09/reference-entry-supplemental/summary.json)。
- [最终证据清单](artifacts/bmc-unknown-sweep-2026-10-09/manifest.json)
  / [全部判定变化](artifacts/bmc-unknown-sweep-2026-10-09/transitions.json)
  / [恢复的 23 项 TP 清单](artifacts/bmc-unknown-sweep-2026-10-09/restored-tp.md)
  / [相对任务开始时源码快照的改动](artifacts/bmc-unknown-sweep-2026-10-09/task-source-changes.patch)。
  此 diff 保留任务开始前已有改动，不把当前全部 git dirty diff 算成本轮工作。

原 285 项的处置结果：23 恢复 TP；135 参考确认竞争、IKOS 仍不确认；
23 参考 PASS；78 参考超时；21 参考运行失败；1 参考 UNKNOWN；4 参考/标签分歧。
补充 11 项为 6 PASS、5 TIMEOUT。对参考不确认的任务也保留 IKOS 复测和诊断，
没有把超时算作明确 UNKNOWN 或 SAFE。

## 方法与参考证据

Dartagnan 的实际运行包为 4.3.1 / `7caef5c850dc2dbf81c5554dcf41e402007f7a19`。
固定 `svcomp.cat`，按 YAML 使用 `-m32/-m64`，关闭 USER assertions，性质为 no-data-race。
每任务外部墙钟预算 90 秒，3 个工作进程；`-Xmx2g` 只限制 launcher，launcher 启动的
engine JVM 使用默认堆设置。原 `config.json` 中的 `java_heap` 元数据已更正为两个独立字段；
执行命令和结果没有修改，不能称 engine 被统一限制为 2 GiB。

158 个竞争标签任务得到 FAIL；安全标签的 `libvsync/cnalock.yml` 也得到 FAIL。
26 个得到 PASS，其中 `race-2_3b/2_4b/2_5b-container_of` 的竞争标签与 PASS 不一致；
另外 78 个超时、21 个运行失败、1 个返回 UNKNOWN。这四项参考/标签分歧均单列，
不能把参考 FAIL 当作安全任务的 TP，也没有为了追随参考结果而修改 IKOS 判定。

运行失败包括 double 不支持、动态线程/数组处理异常、unsupported-property 异常和输入编译问题。
原始 stdout/stderr、输入/YAML hash、命令、返回码和超时状态逐项保留。
PASS、UNKNOWN、TIMEOUT、ERROR 分开记录；没有参考竞争确认时不推断可以恢复几个 TP。

在官方包之外提取同提交的源代码，用原 JAR 的依赖编译 env-gated 模型探针，未覆盖参考安装。
三项机制的源码/模型观察如下：

| 用例 | 事先预测 | 实际观察 |
| --- | --- | --- |
| `10-synch_02-thread_nonunique` | 同一个 worker 的不同动态实例写同一全局对象；后续 join 不消除此前竞争 | 参考模型保留多个不同 user thread，导出两对同址竞争；启用/禁用探针均 FAIL |
| `06-symbeq_09-tricky_address4` | worker 锁住 a[i]，改 i 后写 a[i+1]；另一线程可持不同锁访问同一 datum | 参考探针导出同址竞争对；IKOS 补齐符号锁和符号地址 SC 读来源后确认 |
| `04-mutex_19-call_by_ptr_rc` | main 把确定的 bar 地址传入 foo，第一轮可调用 bar；bar 与 worker 持不同 mutex 更新 glob | 参考处理输出和探针导出 glob 的同址竞争对；IKOS 按实际调用参数解析 callback 后确认 |

探针源码、编译产物、阶段日志、模型事件和命令在
[reference-probe](artifacts/bmc-unknown-sweep-2026-10-09/reference-probe/)。
观察到相同语义机制不是一般等价性证明，参考模型也不要求与 IKOS 选同一个反例。

## 实现与支持前提

1. **有界循环与重复创建。** 沿真实 CFG guard 展开，保留动态定义位置；先尝试原来的 bound=1，
   必要时尝试 bound=16，复用原搜索预算。支持 main→helper 的实际创建前缀和同入口的两个实例。
   只有存在合法创建前缀及成功分支，才能确认实例激活。预算耗尽返回 UNKNOWN，不猜循环退出。
2. **地址的定义上下文。** `var_before` 固定指针构造时的下标定义，修改下标后不改变已保存指针。
   执行过的纯常量算术可以折叠，溢出或不支持来源不能作为具体地址证据。
3. **符号 mutex。** 编码具体对象及符号偏移；同址锁的区间必须互斥，匹配的 unlock 必须同址。
   当前仅支持一个 outstanding 符号锁；复杂嵌套、属性和错误返回仍保守拒绝。
4. **startup 读来源。** 子线程可在 main 前缀完成之前不被调度。main load 只能读到 AR 初值或
   它之前最后一次真实写；不会读未来写。共同的 main nondet 来源在 cache_init 中共享。
5. **符号地址 SC 供值。** 读/写是否同址进入 rf 选择及“最近先行写”约束；不同数组位置的写
   不会被误当成最近来源。有限对象的实际 typed initializer 可枚举（上限 256 个 stride 位置），
   不是为每次读制造任意初值。部分重叠供值、padding/undef、pointer load 和更一般对象仍不确认。
6. **按执行前缀检查同步。** 原来的全程序/整函数未建模同步 gate 收紧为实际前缀检查。
   执行到的 C11 atomic load/store 明确成为 Unsupported，未知 API 和 atomic helper 仍缺摘要而拒绝；
   未调用函数或目标之后的同步不能追溯排序已发生的有限竞争。未闭合 verifier atomic 段仍拒绝。
7. **实际 callback 参数。** 确定函数常量经本次调用实参/赋值/bitcast 可解析；不使用 FPA may 集
   任选 callee。展开事件记录已证实的 callee，以重建动态 invocation，避免反复递归解析历史回调。
   共享内存中的函数指针供值尚未实现。
8. **提前终止不支持的展开分支。** 在 unroll 阶段遇到当前必拒绝的语句/调用，就停止该分支，
   保留具体拒绝语句及原因；其他受支持分支继续搜索。没有扩大该分支的语义，也不从拒绝推断 SAFE。
   此前已知必拒绝的前缀仍占用展开预算，可能遮挡较晚的合法分支。此项恢复第 23 个任务
   `thread-join-counter-outer-race-3`：main 可选 threads_total=1，把 threads_alive 从 -1 加到 0，
   创建并 detach 一个 worker 后跳过 cond_wait。main 的 data 读与 worker 的 data 写可竞争；
   worker 随后的 cond_signal 尚未发生，不阻止选择该有限前缀。

这些扩展不依赖文件名、预期标签或 benchmark 家族。guard、地址、store 值和调用返回继续遵守
来源/定义性检查；合成 freeze-uninit 不作为真实 nondet。新增反例和对照同时覆盖改名、helper、
同入口两个实例、串行 create/join、不同/共同 mutex、先保存地址再修改下标、未来 startup 写、
不同 initializer 元素、共享 guard 一致性以及不同回调的返回值。

## 同时发现并修正的两项上游覆盖遗漏

**重复执行 helper 中的 pthread_create 被认为只有一个线程。**
新用例从 main 两次调用同一 spawn helper，每次创建执行同一写的 worker。
旧二进制返回 SAFE、零候选，Dartagnan 确认竞争。静态 create site 的去重只能去掉 fixpoint replay，
不能证明动态执行次数。现在 helper/worker 内的 create site 保守计为可重复；
function-wide joined 过滤不再用不完整的静态 site ID 覆盖动态实例。
顺序创建的证明也要求真实输出槽和其创建之后的对应 handle load，不能只数 join。

**全局数组/结构的实际写没有传入子线程。**
新用例把静态 `{4,7}` 数组的第一个元素赋为真实 nondet，再创建两个条件 `table[index]==0`
成立时写共享结果的线程；选择 nondet=0 即有真实竞争。旧二进制仍读静态初值而返回 SAFE，
Dartagnan 确认竞争。`materialize_globals` 原来只处理整型标量，忽略黑板中已有写的 aggregate。
黑板没有字段 offset，因此当前对已发布写的非指针 aggregate conservatively forget 整个对象，
不凭空把某个整型区间写入所有字段。此项会损失部分 TN，未来应补字段级 interference。

前后输出、最小源码和参考结果分别保存在 `helper-fn-before/` 和
`aggregate-entry.c`、`aggregate-before.log`、`aggregate-reference.log`。
全量 FN=0 并没有阻止这些新变体暴露漏报；因此不能宣称所有正确性问题已经不存在。

## 与 IKOS 现有能力的协同及消融

**仍然可以完善，而且必须结合 IKOS 的上下文和抽象域契约。**
checker 提供 may-candidate、对象/offset/width、真实创建前缀；AR 提供 typed initializer、
目标 DataLayout、源码和函数参数。这些信息把 BMC 限定到可复核的候选执行，
已支持有界循环、main 初始化和具体 callback，而没有建立第二套 C 前端或一般 CAT 引擎。
两项新 FN 也说明 BMC 精度依赖上游覆盖：候选被删掉时，更强 SMT 无法补救。

本轮消融只关闭 `CandidateFacts` 的候选 offset bounds 与局部 no-wrap 证书，保留真实
startup 和源码来源。这两个数值证书关闭后，已选恢复任务的 TP 没有减少。
具体为 callback 扩展前选取的 21 个恢复任务，21→21，消融补丁与原始结果在
`ai-ablation.patch`、`ai-ablation/`。此处不包括随后恢复的 callback 任务。
因此本轮不能声称它们增加了 TP，也不据混合负载下的耗时声称稳定加速。
typed initializer/布局、创建事实和候选筛选并未在此消融中关闭，不能称为“关闭全部 IKOS”。

更有价值的下一步是：为共享对象建立带 offset/width 的 interference 摘要；
从 AI may-points-to 提供候选集合，再在 BMC 中绑定 allocation/生命周期和实际选择；
用上下文明确的 may-loop invariant 限制搜索、用已验证的必须成立事实简化公式。
共享读的 stale interval 仍不能直接作为具体值证书。

## 保留任务与后续落点

每个剩余任务记录实际输入 hash、机器模型、参考结果、最终 IKOS 结果、候选访问位置、
AR/源码阻塞示例、已尝试的通用扩展和剩余语义前提。原因会重叠并遮挡后续阶段。
“未实现”表示当前受支持范围，不是数学上的不可能；不承诺解除一个 gate 就恢复整桶。

| 缺口 | 下一增量的合理起点 | 不能省略的前提 |
| --- | --- | --- |
| 动态堆对象、堆上的线程句柄 | `04-mutex_38-indexing_malloc` 的单次 main allocation | 分配实例/大小、非空成功分支、指针供值、生命周期和访问范围 |
| 未闭合 atomic 目标、atomic helper | `fkp2014` 或 `13_unverif` | 目标索引与完整执行尾部区间分离；helper 原子包围语义及值流一同实现 |
| join 数组/多级线程 | `thread-join-array-const-race` | 创建实例与具体 handle 的匹配、被 join 线程的真实完成；不能把每个 join 都当所选 worker 的 join |
| 额外线程的共享读依赖 | 选择模型中实际 rf 来源最少的任务 | 候选驱动依赖闭包、更多实例和 rf/co；不能把缺失线程的写当自由值 |
| cond/barrier/semaphore/C11 原子 | 先选明确单 API 的最小已确认任务 | 调用契约、失败/唤醒行为、同步关系及相应内存模型 |
| 搜索预算/复杂 CFG | 保留阶段统计后改路径调度及分组预算 | 不制造退出、不用不完整 UNSAT 证明 SAFE、验证搜索公平性 |

本轮不需要用户提供更多本地源码。未来的官方 2027 配置和兼容并发 witness 2.2 的严格 validator
仍需外部公布/确认；论文级全系统正确性需要独立专业复核。
现有旧 GraphML 投影不能绑定全部实例、值、rf 和精确访问对，外部竞争确认不等于原 SMT 模型重放。

## 验证与 witness 投影修正

双架构回归通过 100 个值/SC/回调检查、80 个正确性检查、16 个动态实例检查、
28 个地址检查、30 个候选/公平性检查，共 254 项。
P0 的 16 条原始观察也已检查：exclusive-flag 在 BMC 中不确认 FALSE；
overlapping-widths 在两模式均确认竞争；published-before-init 在默认模式不再 SAFE，
BMC 中因当前动态对象支持边界保持 UNKNOWN。后者不能表述为 BMC 已确认该堆竞争。

保留了首次失败记录：旧 candidate case 3 曾要求循环后 UNKNOWN，现在它的两个有限循环
已经实际展开，可确认真实竞争，因此更新其期望；公平性 fixture 原来的容易前缀也已可确认，
改成在访问前真实分支爆炸以继续检验昂贵组不会阻塞后续简单竞争。
新增 aggregate 正例最初确实返回 SAFE，随后修复上游覆盖才通过，未把期望改成 SAFE。

对恢复任务的 26 对已保存反例另外做了旧 GraphML 投影检查。
首次 CPAchecker REJECT 的 `05-lval_ls_01-idx_rc` 暴露导出器把 create 挪到初始化循环之前；
现已按记录的 startup/主线程顺序输出 create，缺少实际创建前缀时拒绝导出，
加入了顺序和缺失前缀对照。修正后 CPAchecker 为 17 ACCEPT、7 UNKNOWN，
Dartagnan 为 24 ACCEPT；另外 2 对因目标实例歧义拒绝导出。
剩余 CPAchecker UNKNOWN 涉及数组句柄、指针分析等本地 validator 支持范围。

结果见 [投影复验](artifacts/bmc-unknown-sweep-2026-10-09/witness-validation-fixed/summary.json)。
这批 pairs 取自分阶段的确认记录，逐项记录其 IKOS report 来源；不能称最终每个 SMT 模型已独立重放。
原 35 TP 的外部结论支持也来自上一轮保存材料。严格绑定实例/取值/rf/精确访问对的验证缺口继续保留，
没有用 legacy ACCEPT 抹掉该限制。

最后新增的 counter 任务单独保存验证结果：CPAchecker UNKNOWN，Dartagnan 对完整源行投影 REJECT，
见 [第 23 项投影](artifacts/bmc-unknown-sweep-2026-10-09/witness-validation-prune-extra/summary.json)。
这个拒绝没有计入前面 22 项/26 对的统计，也没有隐藏。IKOS 保存的目标顺序为 worker 写在前、
main 读在后；参考该版本的竞争查询要求读在前、写在后并在 hb clock 上相邻，见完整文档 §1.2。
因此程序存在竞争不保证这一排序的投影能通过该版本 validator。
只保留原目标顺序、以及把目标改为读先写后的两次对照也都是 CPAchecker UNKNOWN / Dartagnan REJECT，
见 `witness-validation-prune-extra-targets/` 和 `witness-validation-prune-extra-read-first/`。
后者只是派生顺序对照，不是原模型重放；因此没有把拒绝归因成“只需翻转顺序”或宣称导出问题已修复。
此外 `WitnessGraph.encode` 把相邻源行的内存事件强制连成 CAT hb；IKOS 的全局调度 clock
可排序无通信关系的不同锁访问，不能直接等同于该 hb。源行合并、遗漏值/控制流以及这种排序解释
均使投影无法承担精确执行重放的职责。

另对原始 counter 输入运行 bound=2 的参考源码探针，得到 FAIL 及真实 data 读写对：
main 读 threads_alive=0，data 读为事件 240，worker 的 data 写为事件 276、同址 48；
所选模型中的 worker 递减计数发生在 main 读取 data 之后。原始模型和命令在
[counter 探针](artifacts/bmc-unknown-sweep-2026-10-09/reference-probe/counter/)。
它支持这个具体竞争机制，仍不证明 IKOS 导出的原 SMT model 已严格重放。
一次探针命令构造错误的原日志保留在 `reference-probe/counter-invalid-command/` 并明确标为无效证据。

## 最终运行一致性与代价

最终安装与构建二进制一致，SHA256 为
`c807451340ba4cb6bfcfe03d872d6ebc15f9d3fac9c7d65578b089f2adbeadc2`。
实际运行 `IKOS_BMC_RECOVER=1 python3 svcomp_race_test.py --demote --jobs 4 --only-fail`，
最新常规模式与诊断模式的 1029 项判定逐项一致，没有错误或超时。
见 [常规运行日志](artifacts/bmc-unknown-sweep-2026-10-09/pruned-normal-suite.log)
和 [逐项一致性](artifacts/bmc-unknown-sweep-2026-10-09/pruned-normal-summary.json)。
此前中间实现一次 nsc-ircc 超时记录仍保留，不是最终结果。

各任务 wall duration 之和：修改前 631.4 秒；提前终止优化前 1530.7 秒；最终 719.7 秒。
最终中位数约 0.289 秒、p95 约 1.17 秒，超过 10 秒的任务 20 个（修改前 9 个）。
这些是并行 sweep 中逐任务耗时的合计，不是整体墙钟耗时，也不是隔离资源下的严格性能实验。
它们说明本轮增加语义后仍有搜索代价，提前终止缓解了无效展开，不能声称全面加速。

运行 `python3 docs/artifacts/bmc-unknown-sweep-2026-10-09/check_final_evidence.py` 可核对
配对集合、逐项记录覆盖、参考确认、全部状态变化、回归记录和二进制一致性，并重新生成证据清单。
它只核对保存结果，不重新运行 analyzer 或参考工具。
