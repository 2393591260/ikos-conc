# IKOS-ConC 抽象解释正确性审查

审查日期：2026-10-10。代码基线：`b3e6706efd1eaf9e69e128d8235d50b8add680e6`。
最初审查阶段只做实验和保存文档，**当时没有修复分析器**；后续实施状态如下。

**2026-10-11状态说明（支持范围的工程验收）**：原F1–F6已经修复或保守封堵；
后续§9.24修复create对may-output逐目标强更新的FN，§9.25–26恢复4个TN，
§9.27修复截断守卫及重复helper激活导致的条件锁假相关，新增48次检查中的16次旧漏报消除。
§9.28继续修复整数表示指针发布FN：stack/TLS/heap等5类双ABI/双模式共20次旧SAFE已退UNKNOWN。
内存域32项表示检查的16项旧失败消除，完整单元值仍精确；非零整数转指针保留未覆盖别名边界。
该阶段1029项AI/BMC配对无判定变化；§9.29又独立修复无损memory length导入并恢复复制崩溃用例。
§9.30恢复实际阻塞加锁分支的普通MUST，48项正反变体及全量通过、冻结集无变化。
§9.31精确普通调用上下文恢复3TN和2TP；新增TP的双ABI执行均独立AR ACCEPT。
§7已汇总16类当前规则/覆盖通道的工程验收；已登记且可复现的FN均已修复或封堵。
启用数值域/覆盖查询及模型选域原型的8组全量消融已完成；默认与强域查询各134条所选AR
执行独立ACCEPT，分别覆盖106个TP任务。132项safe-label UNKNOWN已逐项记录，非全部解决。
这不是全语言形式化证明，完整POSIX失败、opaque句柄来源及弱内存等仍有明确边界。
当前阶段、开放任务与验收条件统一见[优化计划§0](ikos-ai-optimization-plan-2026-10-10.md#current-roadmap)。

后续实施状态：F1 的 self-consistency 访问过滤已撤销，F3 已加入未覆盖线程体的 UNKNOWN 回退；
F5 已撤销区间上界 MUST join，F6 已改为汇总所有可能写，F4 已加入整数读时干扰；
F2 已禁用无完整逃逸证明的 fresh 排除并保守覆盖指针存储的目标。原六类回归均通过。
后续多源指针读取审查复现的独立 FN 已修复，摘要/读取保留全部可能来源；
耦合摘要重算/flush及最终检查变更回退已实施；效果覆盖审查另复现once控制对象身份
漏报已修复；完整标量global的共享字节copy/set已发布，其他布局保守回退。
后续heap通配字段摘要遗漏已修复，精确读/未知读均覆盖可能重叠的字段；
LP64大偏移锁身份碰撞已加入表示边界，无法表示时不建立MUST持锁；
未知extern和未展开internal已加入访问覆盖边界，经过核实的声明合同保留；
递归遗漏写FN已回退；外部原子库效果已加覆盖边界，GCC名字前缀不再凭空建立伪锁。
库buffer写入已加入跨线程效果边界；局部%n输出havoc，zero-byte read合法续行已保留。
合成线程句柄的可观察数值表示和伪造join来源已加保守边界，见优化计划§9.16；
完整身份值流及剩余SAFE前提仍需核实，尚非完整证明。
2026-10-11又复现共享指针逐字节写入后的旧摘要漏报：scalar global的有效指针复制及NULL复制，
ILP32/LP64、AI/BMC均误报SAFE；已在global/heap摘要层加入字节重叠失效，双ABI与全量配对通过，见优化计划§9.20。
尚不能宣称所有SAFE规则已证明。
修复及新实验见[优化计划 §9](ikos-ai-optimization-plan-2026-10-10.md#9-实施进度与配对证据)。
下面六类现象及指纹描述原审查基线，不能当作新代码仍全部存在或已全部修复。

**原审查结论（`b3e6706`）：当时确认六类独立漏报机制，均在 ILP32/LP64、BMC 关闭/开启下误报 SAFE。**
这些现象描述历史版本；当前仍不能以有限测试结果宣称普遍零FN。
它们涉及访问过滤、对象逃逸、线程覆盖、读时干扰、join 身份和写摘要汇总。
既有冻结集 `TP106/FP0/TN696/FN0/UNKNOWN227/error0` 是保存的有限测试结果，
与原审查在集合外发现 FN 并不矛盾。原只读审查阶段没有重新跑冻结集；
后续修复阶段已完整配对，仍没有给所有 SAFE 规则完成全语言证明。

配套：[优化计划](ikos-ai-optimization-plan-2026-10-10.md)、
[AI/BMC 双向反馈研究](ikos-ai-bmc-feedback-research-2026-10-10.md)。
BMC 实现仍统一维护在 [complete 文档](ikos-bmc-complete.md)。

## 1. 证据如何取得，结论适用到哪里

### 1.1 实验指纹与参考工具

| 项目 | 本轮实际配置 |
| --- | --- |
| IKOS | 当前安装二进制 SHA256 `a88dc47be73510ddb380bbb9b842d2ce448f96755d99a29a72192732900f90ff` |
| 性质 | no-data-race；正常成功的 pthread 创建/锁/join 执行，具体异常分支另述 |
| IKOS 模式 | `--analyses=race --concurrency=auto --demote-race-to-unknown -m 32/64`；默认数值域 interval；BMC 对照用环境变量是否存在区分 |
| 输入 | 19 个 C 文件，包括正例、无竞争对照、仍有竞争的消融和改名变体；SHA256 已保存 |
| IKOS 实验 | 19 × 2 位宽 × 2 BMC 模式 = 76 次，全部正常退出 |
| Dartagnan | 固定源码档案 4.3.1 / `7caef5c850dc2dbf81c5554dcf41e402007f7a19`，原生引擎、`svcomp.cat`、DATARACEFREEDOM、bound=3；38 次新分析 |
| Goblint | 本地源码 `bb55b70d91290992629ba3d073185c10cdf356e6`，`conf/ldv-races.json` + no-data-race.prp；38 次新分析 |

证据：[summary.json](artifacts/ai-audit-2026-10-10/summary.json)、
[IKOS 逐次结果](artifacts/ai-audit-2026-10-10/ikos-results.json)、
[参考逐次结果](artifacts/ai-audit-2026-10-10/reference-results.json)、
[原生输出摘录](artifacts/ai-audit-2026-10-10/reference-excerpts.json)、
[C 输入](artifacts/ai-audit-2026-10-10/cases/)。这些输入的预期性质来自下面的源码推理，
**不是比赛 YAML 标签**。数据库、大日志和下载的 PDF 不进入 Git。

参考结果有三项重要限制：

- Dartagnan 在六个主反例的两种位宽下均原生 `FAIL`，三个无竞争对照均 `PASS`。
  本轮没有调用改造的严格探针或拿 IKOS witness 让参考工具接受；源码竞争前缀与原生结果相互佐证。
  有界参考 `PASS` 不作为普遍安全证明，无竞争对照另有源码依据。
- 本地 Goblint **全部 ILP32 分析**带有 `Machine definition not available for selected architecture` 的 Unsound 警告。
  这属于不支持的参考配置，不能用于正确性背书。下文的 Goblint 比较仅采用 LP64，并保留警告。
- Goblint LP64 在五类主反例上发出竞争警告，最终结果是 `unknown`；这是 may-warning。
  在间接线程入口反例上，它同时输出 `Created a thread from unknown function @dummy` 的 Unsound 错误和 `SV-COMP result: true`。
  **该 true 不可信，不能把 Goblint 当作自动真值来源。**其 ESOP 2023 artifact 也明确排除了未知 spawn 指针的 `ypbind`。

### 1.2 与用户所参考论文的对应

已取得并阅读相关章节的原文是 Antoine Miné，
[Static Analysis of Run-Time Errors in Embedded Real-Time Parallel C Programs](https://lmcs.episciences.org/799/pdf)，
LMCS 8(1:26), 2012，DOI `10.2168/LMCS-8(1:26)2012`。
它是 ESOP 2011 工作的扩展版，不能记成“ESOP 2012”。

论文的实验对象是静态线程、有限互斥锁、无递归/动态分配的嵌入式程序；原型名为 Thésée，基于 Astrée 思想。
不能据此推断闭源商用 Astrée/AstréeA 的当前内部实现，也不能直接证明 IKOS 新增的 malloc、间接线程创建和 POSIX 模型。
但论文 §4.4.5 明确讨论了向多处理器/非实时系统的适配：将 `islocked` 视为非确定测试。
所以也不能说其理论只能用于实时单处理器。

本轮主要核对：

1. §3.2.1 / Fig. 7：共享读允许本线程当前值 **以及**其他线程干扰值。
2. §3.3 / Fig. 13 / Theorem 3.3：线程效果在外层不动点中汇总；转移覆盖所有允许干扰。
3. §4.4.1–4.4.4：well-synchronized 的 unlock 最终值具有保护锁和执行配置前提。
4. §4.4.6 / Fig. 20：数据竞争还需要读/写访问收集及干扰兼容性判定，值域正确不自动保证访问覆盖。
5. §3.4 / Theorem 3.5 的 weakly consistent 结论针对论文定义的“reasonable transformations”，
   不能概括成已经证明完整现代 C11 弱内存语义。

下面反例均可在 SC 前缀中发生；原子 flag 用默认 SC 操作，不依赖弱内存特例。
“存在数据竞争”是待检测性质，不能因该竞争在 ISO C 中导致 UB 就将它从检测目标排除；
其他无关 UB 则必须单独处理，尤其是 §2.5 的无效句柄支路。

## 2. 原审查基线的六类 FN（修复状态见文首）

表中 SAFE 是 IKOS 实际结果；每行均覆盖四次 IKOS 分析和两次 Dartagnan 分析。

| 编号 | 主反例 | IKOS：32/64，BMC off/on | Dartagnan：32/64 | 直接原因 |
| --- | --- | --- | --- | --- |
| F1 | `distinct-field-locks.c` | 全 SAFE | 全 FAIL | 各持对象内不同锁，也被当作共同保护 |
| F2 | `transitive-publication.c` | 全 SAFE | 全 FAIL | 发布父对象后，可达子对象仍保留 fresh |
| F3 | `dynamic-entry.c` | 全 SAFE | 全 FAIL | 未解析的线程入口没有贡献访问 |
| F4 | `atomic-guard-refresh.c` | 全 SAFE | 全 FAIL | 局部写覆盖入口干扰，后续共享读不再加入干扰 |
| F5 | `join-upper-bound.c` | 全 SAFE | 全 FAIL | 把句柄区间上界当作确定 join 身份 |
| F6 | `sentinel-writer-union.c` | 全 SAFE | 全 FAIL | 跨线程写队列最后访问者覆盖此前写值 |

### 2.1 F1：同一对象里的不同字段锁并不互斥

[输入](artifacts/ai-audit-2026-10-10/cases/distinct-field-locks.c)：

```c
struct object { pthread_mutex_t left, right; int value; };
/* 线程 A */ lock(state.left);  state.value = 1; unlock(state.left);
/* 线程 B */ lock(state.right); state.value = 2; unlock(state.right);
```

两个 mutex 不同，可以同时获得。两次 `value` 写之间没有共同锁或 join 顺序。

代码：[data_race.cpp](../analyzer/src/checker/data_race.cpp)，`self_locked`，基线第 888–916 行。
每个访问分别检查“锁与数据具有相同 base/instance”，然后
`self_locked(a) && self_locked(b)` 就删除候选。
这里没有证明 **a 和 b 持有同一个 mutex 字段/地址**。
注释中的“same instance ⇒ same lock”遗漏字段身份，是不成立的推理。

对照证据：

- `same-field-lock-control.c`：两线程都持 `left`，IKOS SAFE，参考 PASS，源码确实互斥。
- `unlocked-field-control.c`：第二线程不持锁，IKOS off=UNKNOWN / on=Race Found，参考 FAIL。
- `renamed-field-locks.c`：改名后仍 SAFE，说明不是某个 benchmark 名字触发的问题。

修复方向（未实施）：只接受经证明相等的完整锁身份，再结合锁模式/动态实例判互斥。
对象 base、结构体实例、下标关系都不能代替 mutex 字段身份。相等性不可证时保留候选。
符号分支还存在 `sym == rec.region` 时未使用返回 lock base 的疑点；本轮没有单独复现，勿合并为第七个已证 FN。

### 2.2 F2：发布对象应传递到其可达子图

[输入](artifacts/ai-audit-2026-10-10/cases/transitive-publication.c)：

```c
p = malloc(...); q = malloc(...); /* 均检查非空 */
p->next = q;
published = p;
pthread_create(..., worker, ...);  /* worker: published->next->value = 3 */
q->value = 2;                     /* main，join 之前 */
```

初始化发生在创建前。发布 p 后，子线程可以经 `published->next` 到达 q，
main 的 `q->value` 与子线程写冲突；创建只排序此前初始化，不排序此后的两次写。

代码：[concurrent_semantics.hpp](../analyzer/include/ikos/analyzer/analysis/execution_engine/concurrent_semantics.hpp)，
`unfresh_pointer_targets` / `on_store_privatize`，第 1466–1567 行。
把 q 存到仍 fresh 的 p 时，两者保持 fresh；随后把 p 存到全局时只移除 p 的直接目标，未沿 p→q 传播。
[data_race.cpp](../analyzer/src/checker/data_race.cpp) 第 2218–2220 行据 fresh 跳过 main 对 q 的访问。
这是 **MUST 私有事实前提错误**；fresh 在域合流时做交集本身并不能修正错误来源。

对照：直接把 q 作为线程参数的 `direct-publication-control.c` 得到 UNKNOWN；
把 main 写移到 join 之后的 `publication-after-join-control.c` 得到 SAFE、参考 PASS。

修复方向（未实施）：根据保守 may 可达图传播共享/逃逸闭包，纳入全局存储、线程参数、共享字段及未知调用。
图出现 TOP/不完整时撤销相关私有证明。不能用 `_thread_creators` 或直接 points-to 集合代替可达闭包。
抽象分配点可能代表多个对象，还需证明 fresh 与动态分配身份相容。

### 2.3 F3：未知线程入口需要“不完整分析”状态

[输入](artifacts/ai-audit-2026-10-10/cases/dynamic-entry.c)：

```c
static void *(*volatile entry)(void *) = worker;
pthread_create(&t, 0, entry, 0); /* worker 写 x */
x = 2;
pthread_join(t, 0);
```

volatile 使入口读取没有被折成函数字面量；具体执行入口仍是 worker，worker 写与 main 写无顺序。

代码：`concurrent_semantics.hpp`，`exec_pthread_create`，第 1639–1726 行。
解析 bitcast/formal 后只有 `FunctionPointerConstant` 注册线程。
未解析入口只日志提示，并把 spawned digest 置 TOP。
[thread_modular.cpp](../analyzer/src/analysis/value/thread_modular.cpp) 第 433–452 行按已注册函数重建 worklist。
TOP 会阻止部分 HB 过滤，但 **并没有补上遗漏的子线程访问，也没有阻止最后 SAFE**。

字面量入口 `direct-entry-control.c` 变成 off=UNKNOWN / on=Race Found，参考仍 FAIL。
Goblint LP64 此例出现 true + Unsound；这是参考工具限制，不是 IKOS SAFE 的佐证。

修复方向（未实施）：有保守有限 targets 时全部分析；无法覆盖 targets 时，把相关性质标成分析不完整并返回 UNKNOWN。
单独 havoc 数值或设置 MAY spawned TOP 不能覆盖未知访问、同步及再次创建线程等效果。

### 2.4 F4：入口干扰汇入不等于任意读点干扰闭包

[输入](artifacts/ai-audit-2026-10-10/cases/atomic-guard-refresh.c)：

```c
/* reader */ atomic_store(&flag, 0);
             if (atomic_load(&flag) == 1) value = 1;
/* writer */ atomic_store(&flag, 1); value = 2;
```

合法 SC 前缀：reader 写 flag=0 → writer 写 flag=1 → reader 读到 1 → 两线程分别写 value。
读 flag 从 writer 的 SC 写取值，只使 reader 相关后续访问位于该原子写之后；
writer 的 value 写也位于该写之后，**两次 value 写之间仍无 happens-before**。

代码：`thread_modular.cpp` 的 `materialize_globals` 在函数入口加入干扰；
[numerical.hpp](../analyzer/include/ikos/analyzer/analysis/execution_engine/numerical.hpp) 第 1815–1843 行明确禁止每次整数读注入。
reader 本地 store 强更新为 0 后，后续 `mem_read` 一直得到 0，竞争 guard 分支消失。

删掉旧的 `int_set(lhs, stale_summary)` 覆盖是必要的：它可能错误收窄循环状态。
但删除该错误覆盖 **不等于可以删除所有 read-time interference**。
Miné Fig. 7 要求读考虑局部值与允许的外部干扰的并集。

对照：去掉 reader 自己的 store，或者把 flag 声明为 volatile atomic，两个变体均 UNKNOWN，参考 FAIL。
只靠 volatile nondet 恢复覆盖不算一般模型。

修复方向（未实施）：在共享读点使用覆盖 local 与允许 interference 的转移；
relational 域须保守处理被外部改写变量的关系，不能把一个精确结果直接覆盖为较窄摘要。
只有共同保护等前提经证明时，才能排除部分干扰。

### 2.5 F5：区间 `[0,S]` 不证明只有句柄 0 或 S

[输入](artifacts/ai-audit-2026-10-10/cases/join-upper-bound.c)：main 创建无操作线程 a 和写 value 的线程 b，
`chosen` 初始 0，两个 nondet 条件可分别把 chosen 设成 a 或 b；随后 join(chosen)，main 写 value。

**其中 chosen=a 是有效执行**：join 的是无操作线程，b 仍可与 main 写竞争。
输入还包含 chosen=0 的无效句柄支路；本轮不拿这条支路证明竞争。
即使排除该支路，不能因此排除区间内部的有效句柄 a。
这是“存在有效的违反性质前缀”的反例，不是全路径都没有其他 UB 的测试。
后续回归还应添加可证明排除无效支路的等价句柄变体。

代码：`concurrent_semantics.hpp`，join 身份提取，第 963–975 行。
只要区间下界 0、上界为注册 SID，就把上界加入 MUST joined。
数值区间是多个身份的 hull，不提供“只来自这个 create”的 provenance。

对照：`join-inert-control.c` 直接 chosen=a，off=UNKNOWN / on=Race Found，参考 FAIL；
`join-writer-control.c` 直接 chosen=b，IKOS SAFE、参考 PASS。

修复方向（未实施）：使用带来源的句柄抽象和动态实例集合；
对多个可能有效目标，MUST 完成信息只能取各可能执行共同保证的部分。
除非独立证明只有 {0,S} 且 API/创建成功条件契合，否则不能据区间上界生成 MUST HB。

### 2.6 F6：分析器遍历顺序不能当成跨线程程序顺序

[输入](artifacts/ai-audit-2026-10-10/cases/sentinel-writer-union.c)：

```c
/* a_set    */ atomic_store(&flag, 1);
/* b_reader */ if (atomic_load(&flag) == 1) value = 1;
/* z_reset  */ atomic_store(&flag, 0); value = 2;
```

合法前缀：a_set 写 1 → b_reader 读 1 → z_reset 写 0，随后两个 value 写没有共同保护。
reader 没有先写 flag，因而这里与 F4 的本地 store 覆盖机制不同。

代码：[concurrent_global_env.hpp](../core/include/ikos/core/domain/concurrent_global_env.hpp)，
`record_privatized_write` 第 941 行、`flush_nolock` 第 1039–1045 行，
以及 `thread_modular.cpp` 第 417 行。
未持锁写都进入同一个 sentinel=0 队列；队列不带线程/路径所有权。
flush 以 `latest[addr] = pw.val` 覆盖，不做 join。
外层按函数名分析，z_reset 的 0 覆盖 a_set 的 1，reader 的 guard 被消除。

代码注释称“队列中后写由 program order 排序”，但队列可以跨不同线程；
分析器访问顺序没有这样的语义保证。
Miné §4.4.1 的 well-synchronized unlock 终值也不能移用到这个无锁跨线程队列。

最强对照：`sentinel-renamed-writers.c` 仅交换两个 writer 的函数名，
语义保持相同，结果却在四种 IKOS 配置下 **SAFE→UNKNOWN**，参考始终 FAIL。
已有 debug 模式摘录显示连续外层迭代先排入 `[1,1]`，再排入 `[0,0]`：
[sentinel-excerpt.txt](artifacts/ai-audit-2026-10-10/sentinel-excerpt.txt)。本轮没有插源码探针。

修复方向（未实施）：全局效果汇总须覆盖所有线程/上下文/分支写值。
同一具体路径局部强更新与不同执行间 join 是两种操作，应在状态所属层区分。
受同锁保护的临界区终值可在满足保护和路径前提后发布，不能用一个共享日志替代局部状态。

校准：`branch-release-union.c` 的同锁分支例当前正确报告 UNKNOWN / Race Found，参考 FAIL。
因此本轮**没有证明所有 locked flush 或所有分支汇总都会漏报**，仅确认 sentinel 跨线程机制。

## 3. 原审查基线的 SAFE 规则检查（当前验收见 §7）

下表是源码审查结果，不能读作已完成全部形式化证明。

| 规则/通道 | 合法的充分前提 | 当前判断 |
| --- | --- | --- |
| 两次读 | 属性针对读写/写写冲突 | 基本原则成立，仍依赖访问分类与 API 效果覆盖 |
| 双原子访问 | 都确实是该性质不计竞争的原子访问，宽度/对象/翻译正确 | 原则成立；不能因此忽略原子值对其他访问可达性的影响，F4/F6 即反例 |
| 同线程顺序 | 同一动态实例 | 当前已检查 uniqueness/创建次数及调用执行前提；不能退回“同一函数名就是同一线程”的旧假设 |
| join / pre-create / creator 顺序 | 真实创建、正确动态目标、全部 relevant instances 被涵盖 | F5 明确违反句柄前提；其余 transitive/重复创建规则只局部查阅，未全证 |
| points-to 不交、字节范围不交 | may 集覆盖全部对象，offset+width 覆盖全部访问 | width-aware 范围是正确方向；其输入与字段/heap summary 覆盖仍需证明 |
| 普通共同锁 / RW 模式 | 完整锁身份确定相等，成功持锁，模式确实互斥 | 基本方向合理；symbolic hash/unknown instance/API 失败等前提仍需完善 |
| self-consistency / region | 地址关系同时证明数据和共同锁身份 | F1 已反证其中 flat shortcut；语法扫描 `region_tainting_sound` 不是全局别名/逃逸证明 |
| heap fresh / stack / TLS 私有 | 所有跨线程可达路径均被排除 | F2 已反证 heap publication；stack/TLS 也依赖完整 escape 覆盖 |
| private_init | 首写时未发布，后续保护/顺序成立 | 当前 `kEnablePrivateInit=false`，不能再把这条已禁用规则列成当前已生效能力 |
| 无候选/无记录 | 所有相关线程和访问已覆盖 | F3/F4/F6 反证了“空报告自然安全”的充分性 |
| lockset join | MUST 用交，MAY 用并，并有正确 top/bottom | 当前相关字段基本采用这一极性；来源错误的 MUST 事实仍可能漏报 |

特别注意：`is_unique_thread` 当前还调用创建点循环/函数执行次数检查，
本轮没有发现新反例，不能仅根据历史问题指控它“只数静态 create 点”。

## 4. 原审查阶段的风险清单（后续处理见优化计划§9）

这些条目应加入后续证明/回归清单，但**不计入六个已确认机制**。

| 风险 | 代码证据 | 待证/待测内容 |
| --- | --- | --- |
| 摘要变了但没触发外层重算 | `join_global_pointer` / `join_heap_pointer` 对 offset 做 join，但 dirty 主要按 points-to 数量变化设置；`set_join_summary` 覆盖但不通知 dirty | 同一对象集合下 offset 扩张、转 TOP、MUST 摘要变化能否令依赖分析过早停止 |
| heap pointer 的未知写未进入摘要 | `join_heap_pointer` 第 1795 行对 `pts.is_top()` 直接返回，global pointer 通道则会纳入 TOP | 本地内存/其他失效机制是否足以覆盖未知 heap 字段写；不能默认“不入摘要”就是安全 |
| 指针恢复覆盖本地/初始化来源 | `on_load_pointer_restore` 用黑板 points-to/offset 恢复读取值 | 摘要是否包含所有 init/local 写与可见干扰，是否需要 union；`initial-pointer-source.c` 当前 UNKNOWN，**未复现 FN** |
| 最终检查阶段仍有摘要副作用 | 外层 dirty 停止后 `run_checks` 重新执行函数；没有看到完整耦合状态的独立 postsolution 检查 | 最终扫描能否改变摘要/访问覆盖而未再收敛；read-only 检查或版本校验方案 |
| symbolic lock hash 当作身份 | `record_lock_symbolic_index` 用 64 位混合值作为锁 key，注释称碰撞“overapprox sound” | MUST 锁身份碰撞可能虚构共同保护；应结构化 intern/复核完整 tuple。未构造碰撞输入 |
| unknown instance 与 0 混同 | `lock_instance` 查不到时返回 0 | 未知身份是否能与一个具体实例 0 错误相等 |
| region taint 证据不完整 | `region_tainting_sound` 主要扫描 loaded-pointer store 的语法模式 | phi/cast/helper/copy/未知调用的跨实例别名闭包；不能由没发现该模式推出不逃逸 |
| 锁分区与 PBR 前提混用 | 当前按每个 held lock 记录写；没有同等的 `M[g]` 保护集与路径私有状态证明 | 部分同锁、嵌套锁、不同保护锁、锁外读写；一个 held lock 不一定是该变量的全局保护锁 |
| 库失败/未知效果 | pthread 与未知 extern 分派 | 创建/trylock/join/once/cond/semaphore/barrier 的返回值条件、失败路径、未知回调效果是否齐备 |

`read_global_with_lockset` 当前检索未发现实际调用者，只有定义/注释。
不能把其设计注释当作现在每个读点已经消费锁分区的证据。
方法注释把某些 widening 到 TOP 称为“unsound”也不准确：覆盖正确的 TOP 通常降低精度，
需要区分真收窄与纯保守退化。

## 5. 论文/参考实现能提供什么依据

| 原始来源 | 本轮核对部分 | 能移用的原则 / 不能直接移用的结论 |
| --- | --- | --- |
| Miné LMCS 2012 | §3.2.1、§3.3、§4.4.1–4.4.6 | 干扰闭包、配置化互斥、访问收集；不直接证明动态堆/线程 API 或 C11 |
| [The Astrée Analyzer，ESOP 2005](https://www.di.ens.fr/~cousot/publications.www/CousotEtAl-ESOP05.pdf) | §4–6 | reduced domains、packing、widening/narrowing 和 post-fixpoint；不揭示闭源当前实现 |
| [Improving Thread-Modular Abstract Interpretation，SAS 2021](https://arxiv.org/pdf/2108.07613) | §2–5 | PBR/lock-/write-centered 及组合必须保留各自前提；论文说明这些分析精度并非一方全面支配另一方 |
| [Clustered Relational Thread-Modular Abstract Interpretation with Local Traces，ESOP 2023](https://link.springer.com/content/pdf/10.1007/978-3-031-30044-8_2.pdf) | §2–8 | local traces、thread IDs/uniqueness、MHP、变量 clusters；小 cluster 的等精度结论限定于其 2-decomposable 假设 |
| 固定 Goblint 源码 | `basePriv.ml`、`mutexAnalysis.ml`、`threadEscape.ml`、`threadId.ml`、`raceAnalysis.ml` | 可借鉴具体接口和保守摘要；本轮未知入口 Unsound 说明不能盲搬整套边界行为 |

Goblint 关键源码位于父目录固定 commit：
`src/analyses/basePriv.ml:933` 的 `ProtectionBasedPriv`，
`mutexAnalysis.ml` 的 `MustProtectingLocks` / `MayBePublicWithout`，
`threadEscape.ml:29` 的 `ReachableFrom` 与 `threadspawn`。
它的解锁发布会检查实际保护集和局部私有状态；传播逃逸会查询可达集合，
比“每个 held lock 一份日志”和“只移除直接 malloc 目标”多了必要前提。
同时其 ReachableFrom TOP 分支还有其他 invalidation/warning 协作，不能单拷空集回退。

下载来源、PDF SHA256 和实际查阅章节见
[publications.json](artifacts/ai-audit-2026-10-10/publications.json)。
本轮没有逐行审计这些论文附录的全部证明，也没有把其他模型生成的项目文档当成一级证据。

## 6. 可复核命令与本轮结束条件

从项目根目录选择一个已保存输入，例如：

```bash
case_path=docs/artifacts/ai-audit-2026-10-10/cases/distinct-field-locks.c
env -u IKOS_BMC_RECOVER install/bin/ikos --analyses=race --concurrency=auto --demote-race-to-unknown -m 32 "$case_path"
IKOS_BMC_RECOVER=1 install/bin/ikos --analyses=race --concurrency=auto --demote-race-to-unknown -m 64 "$case_path"
```

`IKOS_BMC_RECOVER=0` 仍有环境变量，会开启 gate；关闭必须 unset。
机器可读逐次记录含完整命令和 SHA256。参考环境另需：

```bash
# 在 /home/ruan/dartagnan-svcomp2026/dartagnan，JAVA_BIN 指向 Java 17
DAT3M_HOME="$PWD" CFLAGS='-fgnu89-inline -m64' "$JAVA_BIN" -Xmx2g \
  -cp 'dartagnan/target/dartagnan.jar:dartagnan/target/libs/*' \
  com.dat3m.dartagnan.Dartagnan cat/svcomp.cat "$case_absolute_path" svcomp.properties \
  --property=DATARACEFREEDOM --bound=3 --program.processing.skipAssertionsOfType=USER
# 本地 Goblint；仅 LP64 被本轮当作支持的参考配置
/home/ruan/goblint-analyzer/goblint --conf /home/ruan/goblint-analyzer/conf/ldv-races.json \
  --set ana.specification /home/ruan/goblint-analyzer/tests/sv-comp/no-data-race.prp \
  --set exp.architecture 64bit "$case_absolute_path"
```

最初审查交付完成了主反例 + 对照/变体、双位宽、BMC开关、原生参考比较和根因定位。
当时尚未实施六类修复；它们此后已处理，见文首与配套优化计划§9。
全部SAFE规则证明、完整C/POSIX/弱内存规范审计及官方witness验收不能从本报告推出；
后续工程验收范围按优化计划§0逐项登记，本报告不作发布时“已证明零FN”的声明。


## 7. 支持范围的工程验收与剩余义务（2026-10-11）

本轮工程P0出口是：已复现FN已处理；每个当前SAFE入口有明确前提、fallback和可复核
证据；不覆盖的效果保留Warning/UNKNOWN或显式工具error；默认同配置全量配对完成。
**并不是**对全部C/LLVM/POSIX、所有编译变换或任意动态生命周期完成soundness定理。
当前明确支持的是经现有AR前端处理、常规有效/成功pthread合同下的no-data-race检查，
竞争本身不因ISO C UB被排除；其他未定义行为与不支持语义单列边界。

以下逐项查阅当前消费点及所属事实来源；“验收”指源码论证+针对性检查的工程状态。
完整机器记录见[inventory.json](artifacts/ai-precision-2026-10-11/final-acceptance/inventory.json)。

| 当前规则/通道 | 必须成立的前提 | 当前处理与证据 |
| --- | --- | --- |
| normal bottom | transfer、初始化、环境干扰及收敛覆盖全部支持状态 | 读时干扰/写并集/摘要变更检查；不是全部IR转移的形式证明 |
| 私有访问 | 排除全部跨线程alias，TOP不能被当成私有 | heap fresh排除禁用，stack/TLS逃逸保守传播；整数发布及字节重建边界已处理 |
| read/read | 只检测读写/写写，相关库及生命周期效果没有漏记 | 写/未知效果保留；free/lifetime一般语义未认证 |
| 同一线程 | 同一动态实例或实例不可重叠，不能只比函数名 | 创建次数/静态生命周期证书；非unique访问复制保留self-race |
| join HB | 有来源的确定动态目标、成功join及全部相关实例覆盖 | 区间端点捷径禁用；opaque数值/动态数组句柄保守拒绝 |
| create HB | 确定祖先/直接creator、单次父实例及未spawn摘要 | 检查完整创建链及MAY摘要，未知线程体保留覆盖警告 |
| may points-to不交 | 所有可能对象/别名均在集合内 | 多源/init/local/interference并集，TOP与所有对象配对，整数alias保留边界 |
| 双atomic | 两个真实AR atomic，定义的race性质不计该对 | mixed pair仍保留；不由此推断完整C11 weak-memory值/同步语义 |
| byte区间不交 | 实际start范围及width覆盖访问 | ZNumber宽度运算，未知width不排除；依赖有效对象和表示边界 |
| 实际共同锁 | 完整具体锁身份、可表示offset、有效成功持锁及互斥模式 | 大offset/实例/属性门、未知unlock撤销；read/read锁不能保护两个write |
| 条件锁 | 固定值的零值等价和单次动态激活 | 同位宽常量零偏移按优化计划§9.35证明；跨宽度算术/截断/phi/循环/重复helper不剥层；执行分支保留普通MUST |
| 相对锁 | 每条前缀保留data+delta的mutex，重叠可推出同start/mutex | 全局对象+stride/width引理；精确普通调用绑定，循环/未知call/unlock/rebind/预算均拒绝 |
| 旧private-init/region/TOP猜测 | 完整发布/别名/锁证明 | 不再作SAFE过滤；部分region/provenance注释标签仍保留，不能当成能力证明 |
| 库/internal效果 | 全部读写、回调、同步及跨线程摘要覆盖 | 未覆盖extern/internal/byte效果警告；显式import拒绝是error，不假称UNKNOWN |
| 耦合fixpoint | 所有可能写和pointer/heap/join摘要变化使依赖失效 | 每轮flush、语义dirty及最终检查变更回退；还没有独立通用post-fixpoint认证器 |
| opt-in AI覆盖查询 | 稳定may interval/congruence、独立两个局部状态、共同非TLS global及准确width | 模块化byte公式UNSAT仅排除该候选；矛盾事实/超时/不支持保留。定向及启用/关闭强弱域全量均验证，冻结集无新增TN；结果另见研究篇§9 |

本次额外重查15个既有runner，共714次analyzer定向观察及相关核心单元检查通过。
另两个历史runner的更强精度断言原样失败：`single-execution`要求loop-helper必须FALSE，
`lock-allocation-identity`要求stack-main控制必须SAFE。在前驱`62e9015`二进制也同样失败。
另按比赛模式完整核对11个输入×2ABI×AI/BMC×前后=88次，均满足“竞争不得SAFE、安全不得
确定FALSE”；前后判定完全一致。原失败记录保留在清单，没有通过删除断言冒充完整质量通过。
`once-helper-join-control`/`sequential-helper-control`及stack-main等仍UNKNOWN，是现存精度边界。
原非demote AI模式可能把may-warning标成definite error；本验收不把这种输出当作已验证竞争。

最新默认1029冻结集：AI TN661/U367；BMC TP106/TN661/U261，FP/FN0/error1。
本次恢复的两个TP另有双ABI、动态实例/值/地址/同步一致的独立AR执行ACCEPT。
随后默认interval→BMC与强域查询→BMC分别对全量106个TP任务的134条所选执行重新独立
检查，均ACCEPT；并没有用强域的模型替代默认模型检查。它们不是仅按YAML计数，
也不是官方source witness验收，更不认证SAFE覆盖。数值域/查询的8组启用消融结果见
[研究篇§9](ikos-ai-bmc-feedback-research-2026-10-10.md#prototype-results)。

已经没有登记为已复现而未处理的本轮FN；剩余**证明或能力义务**包括：完整POSIX失败及
非默认属性、动态opaque handle值流、一般heap/lifetime/free冲突、完整C11弱内存、
全前端/抽象域证明与通用归纳后解认证。当前部分功能以保守Warning/error封堵，部分属于
明确不支持的语言/API合同；不声称任意范围都已经被一个总gate完全覆盖。
后续加入新语义仍需独立验收其访问、值、对象、实例及失效链，不能从本表继承普遍正确性。

证据：[原始runner退出/支持义务](artifacts/ai-precision-2026-10-11/final-acceptance/inventory.json)、
[定向计数](artifacts/ai-precision-2026-10-11/final-acceptance/directed-counts.json)、
[旧用例前后完整检查](artifacts/ai-precision-2026-10-11/final-acceptance/legacy-contracts.json)、
[零变化](artifacts/ai-precision-2026-10-11/final-acceptance/legacy-transitions.json)。


### 7.1 第一版结案及可复查的边界

本轮工程验收完成，原审查六类FN与后续本轮具体漏报已修复或保守封堵，16类当前支持规则
逐项记录实现、前提、失败回退和证据。已完成工程出口，不意味着项目支持全部C/POSIX，
也不把有限FP/FN=0当作数学上的零漏报保证。

最后build成功、build/install二进制hash相同；旧三机制及对照的双ABI20项观察和前驱结果
逐项一致。714次额外既有runner观察、88次旧精度失败用例的前后性质检查都保留原记录；
两个历史精度runner仍有必须FALSE/SAFE断言失败，不能改称“所有测试全绿”。
所有当前safe-label UNKNOWN在第二篇的132项台账中有状态；正常参考TRUE38项需要下一版
一般机制，94项参考未证实，8个heap诊断明确部分完成，不宣称已经排除所有潜在根因。

反馈原型默认关闭。启用/关闭及强/弱数值域的全量验收支持“这些有限任务判定无退化”，
不补齐基础AI所有语义的证明。模型只选择重算域；超时/失败保留初轮，模型不成为假设。
未来要扩大SAFE能力，仍必须逐项满足访问覆盖、动态实例、字节对象、值来源与干扰闭包义务。

最终证据：[8组配对与执行检查](artifacts/ai-precision-2026-10-11/feedback-corpus/summary.json)、
[最终build/install/双ABI旧回归](artifacts/ai-precision-2026-10-11/feedback-corpus/final-checks.json)、
[268条配置各自所选执行](artifacts/ai-precision-2026-10-11/feedback-corpus/executions.json)。
保留的事实范围是AR前端快照及显式库合同；官方source/witness验证、一般POSIX失败/非默认
生命周期、完整C11弱内存、通用归纳后解认证未完成。原型文档没有把这些延期项包装成已实现。


### 7.2 第二版守卫证书的增量验收（2026-10-11）

优化计划§9.35加入纯、固定单次激活的同位宽常量add/sub抵消；所有继续执行满足
`result = source + delta mod 2^w`，只有delta=0时共享零值身份。截断/非零偏移/跨宽度
抵消不获证书，循环/phi/重复调用及预算回退保留。AR no-wrap溢出会终止，不能把该证明
用于溢出后任意行为；源码真实入口额外排除了INT_MAX。
64项新双ABI/双模式对照、旧guard48/MUST48/原审查76/BMC20及全量通过。
当前TP106/TN662/UNKNOWN260/FP0/FN0/error1，安全标签UNKNOWN131。
新定向FALSE的12条AR执行已独立ACCEPT；冻结集没有新TP。本次没有重新声称所有106任务
的本次所选模型都重放过，第一版的134条默认所选执行仍是那个版本的证据。
依据：[guard-cancellation](artifacts/ai-precision-2026-10-11/guard-cancellation/summary.json)。

### 7.3 嵌套线程单生命周期证书验收（2026-10-11）

优化计划§9.36允许字面pthread_create入口作为单次调用链的一条边，父生命周期仍须
唯一且非循环，隐式main有显式入边、逃逸或多入边时拒绝。不是由运行时发现次序或
“目前只见到一个线程”证明唯一。native双ABI正反例48项及旧相关回归通过，7项真实
安全任务在其ILP32配置恢复；旧single-execution精度断言仍有缺口，另行配对记录。
完整1029：TP106/TN669/U253/FP0/FN0/error1。没有新冻结FALSE；历史第一版模型证据
保留原版本范围。完整POSIX失败、opaque句柄及一般实例来源仍不属于这个证书。
证据：[nested-lifetime](artifacts/ai-precision-2026-10-11/nested-lifetime/summary.json)。
