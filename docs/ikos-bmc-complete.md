# IKOS BMC 完整设计文档（从零到 sound gate 体系）

> 本文描述当前实现、证据前提和能力边界；读过源码或测试通过不等于获得完整 soundness 证明。
> 最新 UNKNOWN 全量遍历及基线见 [逐任务恢复报告](ikos-bmc-unknown-sweep-2026-10-09.md)。
> 当前冻结集回归：TP=61（标签确认、未全部独立验证）/ FP=0 / TN=697 / FN=0 / UNKNOWN=271 / 错误=0。
> 动态仿射守卫减少无溢出循环的不可行路径，唯一恢复 fmaxsymopt，见[守卫报告](ikos-bmc-affine-guards-2026-10-09.md)（60→61）。
> 创建基数审查修复递归/发现遗忘漏报，并恢复三项join SAFE，见[单次执行报告](ikos-single-execution-2026-10-09.md)。
> SAFE 审查修复stdio访问遗漏，四项TN转UNKNOWN，见[库访问报告](ikos-stdio-effects-2026-10-09.md)。
> sem_getvalue共享输出漏报另已修复，冻结集计数不变，见[输出槽报告](ikos-sem-value-2026-10-09.md)。
> 中间循环界限实验因配对TP回退已撤回，见[搜索实验](ikos-bmc-loop-search-2026-10-09.md)。
> 最新增量见[成功创建返回值](ikos-bmc-create-result-2026-10-09.md)（58→59）及
> [启动数组地址](ikos-bmc-startup-address-2026-10-09.md)（59→60）。历史 23 项新 TP 有参考竞争确认。
> 严格参考检查仍为部分约束验证，58任务/71候选的覆盖和缺口见
> [独立约束报告](ikos-dartagnan-strict-validation-2026-10-09.md)；不得据此宣称全部反例已验证。
> 结构化前缀导出与严格重放进展见 [P0 重放报告](ikos-bmc-replay-2026-10-09.md)。
> 上一轮回归参考：TP=35 / FP=0 / TN=709 / FN=0 / UNKNOWN=285 / 错误=0；从 TP=7 恢复 28 项。
> 实现、架构协同及验证边界见 [精度恢复报告](ikos-bmc-precision-2026-10-09.md)。此前 TP=27→7 的
> 收紧依据保留在 [正确性审查报告](ikos-bmc-correctness-2026-10-09.md)；本轮恢复其中撤回的 23 项中的 19 项。
> 历史 TP=80、53、27 是不同实现的有限实验；撤回 TP 不表示任务标签错误，也不表示每条旧反例都已证明虚假。
> 任务范围是本地脚本扫描到的 2026 冻结集 1029 项，不能直接等同于已核实的 SV-COMP 2027 官方类别清单。
> 本地 GraphML 验证及下一步方案见 [见证验证报告](ikos-bmc-witness-validation-2026-10-09.md)：
> 35 项得到外部竞争结论支持，但旧 validator 不能严格绑定线程实例，原 SMT 模型仍未全部独立重放。

---

## 0. 目标与红线（【铁证】，来自项目约定）

- **目标**：接在抽象解释（over-approx，保 FN=0）之后，对每个 demote 成 UNKNOWN 的候选竞争对，用 Z3 精确判定
  「这两个访问是否真的能并发」。**SAT 且执行/值/地址/同步/实例前提满足 ⟹ 报 FALSE；UNSAT / 前提不可证 ⟹ 保持 UNKNOWN。永不报 TRUE**（证 SAFE 交给抽象解释）。
- **红线 FN=0**：sound 优先于 precision，宁多报 FP 不漏报 FN（当前也守 FP=0，但见 §11：FP=0 是实测结果，
  不是「欠近似」自动保证——BMC 数据上过近似，A0 只是必要边界，调用/锁/创建等前提也必须满足）。
- **资源边界**：当前逐对搜索设预算，耗尽保持 UNKNOWN。未来可采用候选依赖闭包或更完整编码；这不是禁止全程序 SMT 的语义要求。
- **内存模型**：目标 = SV-COMP no-data-race 的 pthread 程序，Dartagnan 官方以 `acyclic hb as sc`（SC）建模（§1.1），
  IKOS 编码使用受限 SC 前缀。执行前缀中的未建模原子操作/API 必须拒绝；不因未调用函数或目标之后的同步直接拒绝所有候选。

---

## 1. 理论依据：Dartagnan SV-COMP 2026 的 race 判定（【铁证】，读源码）

Dartagnan 官方版 = `7caef5c`/标签 4.3.1（SV-COMP 2026），其 no-data-race 判定**不是** cat 文件里的
`dr/conflict`（那是 2026-06 才加的 SV-COMP 2027 版），而是**内置 `PropertyEncoder.encodeDataRaces`**。

### 1.1 svcomp.cat（6 行，【铁证】）

```
let com = (rf | fr | co)
let hb = po | (com & int) | (rmw^-1? ; (com & ext))
acyclic hb as sc
```

- `rf` = read-from（读从某写取值）；`fr` = from-read；`co` = coherence（同址写全序）。
- `com & int` = 线程内通信；`com & ext` = 跨线程通信；`rmw^-1?` = 可选 RMW 反边前缀。
- `acyclic hb as sc` = hb 无环 = 顺序一致性。

### 1.2 race 充要条件（【铁证】，PropertyEncoder.encodeDataRaces）

对每对线程 t1≠t2、t1 里每个非 init 非原子 WRITE `w`、t2 里每个非 init 非原子 MEMORY `m`：

1. `canRace` = `mo==null || mo==NONATOMIC`（**原子/RMW 访问根本不进竞态对**）。
2. 跳过 `w.RMW && m.RMW`、`!alias.mayAlias(m,w)`（不同址）。
3. `isConflictingPair = execution(m,w) && sameAddress(m,w)`。
4. `isAdjacentInHb = hb(m,w) && clock_hb(w)==clock_hb(m)+1`（**hb 里相邻**）。
5. `hasRace |= conflicting && adjacent`。

**该版本完整模型内部的查询形式：conflicting ∧ hb-相邻**（`isAdjacentInHb = hb(m,w) ∧ clock_hb(w)==clock_hb(m)+1`）。
「相邻」指在 hb 的某个线性扩展里两事件连续（中间无其他事件），**不是**「两事件之间没有 hb」——可比且无
中间元素的两事件同样相邻。IKOS 使用类似的相邻查询，但缺少 Dartagnan 的完整执行、值流和实例约束；不能据此宣称两者等价，或把相邻当成无条件的竞争定理。

> 注：这里的 hb 是上面 CAT 的精确表达式（含 rf/fr/co 值流），不能简写成 `po | com | rmw`，也不能直接等同于 POSIX/C11 的
> happens-before；assume/值约束是执行约束、不是事件二元排序关系。IKOS 当前用 po+同步约束和按需 SC 读来源
> 建模受支持的两路径前缀，不是该 CAT 的完整关系编码，见 §8。

### 1.3 Dartagnan 为何慢（【铁证】+ 复杂度分析）

全程序 SMT 包含执行、控制流、内存/值流及线程依赖约束；竞争查询遍历跨线程事件对。
公式规模和求解代价都可能较高，但不能只从一次超时或“全程序”推导指数复杂度，
也不能把完整建模视为性能问题的唯一原因。当前没有同资源、同任务清单的性能归因实验。

---

## 2. 架构：两层判定（【铁证】，来自项目约定 + 代码）

```
抽象解释（over-approx，快）           BMC（有界候选搜索，确认须满足建模前提）
  证明 SAFE（TN=+2）                   确认 RACE（TP=+1）
  报「潜在竞争」→ demote 成 UNKNOWN     对每个候选对精确判定 SAT/UNSAT
        └────────────── 候选对 ──────────────┘
```

- **SAFE 的证明前提**：抽象解释及每条过滤规则都必须覆盖具体行为。当前 FN=0 是有限测试观察；本轮没有完成所有 SAFE 过滤规则的证明。
- **FALSE 方向需主动守 FP=0**：BMC 结构上欠近似（有界路径）、**数据上过近似**（未建模值建成自由 SMT 变量，
  会凭空造出程序没有的执行）——所以「欠近似 ⇒ FP=0」**不成立**。FP=0 还需要创建、完整调用前缀、锁历史及地址等证据；
  有限实验中的 FP=0 不是架构自动保证（见正确性审查报告）。
- **完备性缺失的后果**：硬程序两头都「可能」→ UNKNOWN（0 分），**不是 FP/FN**。soundness 与完备性正交。

---

## 3. BMC 全流程（【铁证】，读 bmc.hpp / data_race.cpp）

`confirm_race` 接收两个实际访问、宽度、诊断/预算、创建记录及 `CandidateFacts`，依次：

1. 展开实际执行前缀；其中 atomic memory 成为 Unsupported，未知 API / atomic helper 缺少摘要时拒绝确认。
2. `unroll_to(funcA, stmtA)`、`unroll_to(funcB, stmtB)` → 两条路径展开。
3. `rA.incomplete || rB.incomplete` → 构造预算耗尽则不确认。回边截断不制造循环退出。
4. 每条路径对进入 `check_race`，沿实际参数绑定精确锁身份；旧 function-level/held 提前门已删除。
5. 检查前缀支持范围、完整锁历史、实际 assume、机器整数定义性和所需读来源后查询。

上游（data_race.cpp）在预算内逐对重算程序同步、原子混合、singleton 全局对象、偏移、隔代排序及线程激活前提。
非 main 的两个入口需要 main 的实际创建前缀；保留直线快路径，另展开最多 8 个 CFG 前缀，
支持确切条件下的创建、bound=16 内的循环及 helper 创建。同入口两个实例仍须不同动态创建事件。
未知调用、未支持的创建上下文仍保持 UNKNOWN；startup 的锁/atomic 区间必须完整释放。
含 main 的路径对必须在 main 前缀中实际包含对方的 Create 事件。AI 的 may-multiplicity 不证明两个实例确实存在。
确认后，报告锚点、`access_a/access_b` 和 `confirmed_pair_id` 均指向实际成功的候选，供 witness 使用。只透传选中的创建前缀，按执行顺序登记；目标位置不再把线程入口当作源码函数。

---

## 4. 事件模型（【铁证】，读 unroll.hpp）

```cpp
enum class EvKind { Read, Write, Lock, Unlock, Assume, Create, Join, Local, Unsupported };
struct BmcEvent {
  ar::Statement* stmt;      // AR 语句
  EvKind kind;
  uint64_t mutex;           // 编码前沿实际参数重建精确锁身份
  bool reader;              // rwlock reader 区间标记
  uint64_t base; ZNumber const_off; uint64_t var_id; ZNumber coeff;  // 符号地址
  ar::LocalVariable* local_base; // 可检查范围的前缀栈对象
  uint64_t width;           // 每个内存访问的字节宽度，0=未知
  uint64_t thread;          // Create=线程函数指针 / Join=句柄
};
```

`classify_event`：Load→Read、Store→Write、Comparison→Assume、mutex→Lock/Unlock（`__VERIFIER_atomic_begin/end`
→ PSEUDO_ATOMIC_LOCK）、pthread_create→Create、pthread_join→Join。
assume 调用收集实际实参；有定义的普通 wrapper 展开实际函数体。不能证明无阻塞/终止/副作用的调用标记 Unsupported。
支持 rwlock rdlock/wrlock/unlock，Lock 事件另携带 reader 标记；cond 和其他 rwlock API 保持未建模边界。
极小常量 stdio 子集有摘要，`%n`、动态格式、返回值依赖等不当作 no-op。
地址提取失败 / 锁身份提取失败 → 保持 base==0 → 上游保守不确认。

---

## 5. 展开算法（源码与回归已核对）

- `dfs_paths` 保留实际 CFG 分支条件，到 target 截止；后继路径独立复制前缀和栈。
- `Unreachable` 和 `LibcAbort/LibcExit` 终止当前前缀。
- helper 内目标可直接命中；目标在调用之后时，将每条实际 return 路径拼接到调用方续点。
  参数/返回值若无法编码，不能省略相关条件。未知或不可展开的调用不按 no-op 放行。
- 调用支持确定函数常量、赋值/bitcast 链和本次已执行调用的实际 callback 参数，不用 FPA may 集证明 callee 执行。
- 不删除循环比较。先尝试 bound=1，失败时在共享预算内尝试 bound=16；保留每次迭代的真实 guard 和动态定义。
  已执行的纯常量运算可用于路径剪枝/地址折叠；无法编码循环值或退出时保持 UNKNOWN，不能证明无界安全。
- 构造预算：路径 256、事件 1024、DFS 步数 16384、栈深 256、helper 深度 8；耗尽丢弃部分枚举。
- 即使操作结果不参与守卫，no-wrap 算术、除法/移位等也可能阻止后续访问。
  当前检查全部 BinaryOperation 的机器整数定义性；无法编码时保持 UNKNOWN，不省略潜在陷阱。
- 定义与参数按实际 invocation 查找；缓存区分动态定义，不把前一次 helper 调用的值用于本次调用。

---

## 6. SMT 编码（【铁证】，读 encode.hpp）

`check_race(pathA, pathB)`：

1. **clock 变量**：每个事件一个整数 clock。
2. **po**：路径内相邻事件 `clock[i] < clock[i+1]`。
3. **spawn HB**：`Create(B) < B 首事件`。
4. **join 的当前限制**：每个 Join 统一施加对方前缀末事件 `< Join` 的额外排序，尚无一般 handle/实例匹配。
   目标在前缀末端，这种中间排序会阻止相邻查询；不能称已精确建模 joined 线程的真实完成。
   后续必须补齐真实身份和完成路径，才能恢复含 join 的相关竞争。
5. **mutex/rwlock 历史**：沿实际调用参数解析全局对象及偏移，每段完整/未闭合临界区分别记录。
   单个 outstanding 符号锁支持地址表达式、unlock 同址和同址区间互斥；复杂符号锁嵌套仍不确认。
   同锁区间互斥，但两个 reader 区间不互斥。未知锁身份、重入和无本路径持有证据的 unlock 不确认，不能只保留最后一次 lock/unlock。
   `__VERIFIER_atomic_begin/end` 还排除另一线程的所有事件，即使它们不在原子段内。
   若目标前缀仍处于未闭合原子段，当前缺少尾部执行闭包，保持 UNKNOWN。
6. **assume 约束**：路径上每条 Comparison 编码。**A1 值来源 gate**：整数常量、明确识别的整数
   `__VERIFIER_nondet_*` 声明 / `IkosNonDet` intrinsic（自由符号 + 类型范围）、实际实参/返回、整数转换及
   受支持算术可编码。转换按位宽及 U/S 处理；no-wrap、除零、移位范围等有定义性约束。
   demanded load 必须有受支持的 SC 读来源，不能自由化；未知调用、指针/浮点比较等仍不确认。
   静态多定义按本次动态前缀的最近实际定义处理；定义须早于使用，缓存包含动态定义位置。
   每路径独立 cache，guard 与地址共享 cache；值前缀 `value_a_` / `value_b_` 与时钟 `a` / `b` 分离。
   nondet 识别不再使用名称子串；旧式 C `f()` 声明允许零实参调用，但须核对返回位宽及符号。
   还编码 assume 的实际非零实参，不能用提取不到比较作为省略理由。`freeze_uninit` 产生的
   `ikos.freeze_uninit` 标记不能成为真实自由值；无源码 DebugLoc 的旧式未标记来源也不确认。
   此外，`prefix_values_defined` 检查所有语句实参（包括 store 值、return 值、调用参数及创建前缀），
   拒绝真实消费 synthetic/undef 来源。合成 call/store 同时标记，二者本身是抽象 bookkeeping。
   局部 load 的定义性只认可本次 allocation 之后真实覆盖的 store 或受支持的 create 句柄输出。
   demanded 整数 load 的值另绑定本路径/另一选中路径/实际 startup 的写及精确 AR 初值；
   内存 clocks 互异，取同一 SC 顺序中的最近先行写。同线程未来写不能供值，main nondet 初值共用一个符号。
   单仿射符号地址供值已支持：rf 和最近写只考虑实际同址写；typed initializer 在小对象中按 stride 枚举。
   startup 中 demanded 整数 load 只读初值或此前写。部分重叠供值、pointer load、一般对象/更复杂地址仍 UNKNOWN。
   create 目前只确认默认属性、已知有效句柄输出对象的成功分支；未知属性/地址不证明激活。
7. **同址门**：两访问 `base` 相同（否则提前返回不确认）+ **字节范围重叠**（`off_a < off_b+width_b ∧ off_b < off_a+width_a`）；
   单个变量下标编码为 `const_off + coeff * encode_value(index)`；不支持的值来源仍不确认。
   前缀中每个内存访问都须有已知对象、宽度和范围；允许已知有界栈对象作为前缀，竞争目标仍限全局对象。
   偏移常量、系数及 SMT 整数常量使用精确整数，避免宿主 `int64_t` 溢出。
8. **IKOS 事实**：`CandidateFacts` 提供候选 may-offset bounds、真实 main 创建前缀及受限局部 no-wrap 证书。
   offset bounds 只限制已有真实值来源的地址，不替代来源；不能把可能 stale 的共享读区间当具体值。
   startup 的实际 guards 与两 worker 条件一起求解。证书只省去重复定义性检查，demanded 结果仍精确编码。
9. **竞争查询**：两访问能否相邻（`clock 差 1`，两方向各查一次）。SAT 只有在执行、值、地址、同步和线程实例前提成立时才可用于确认。每次 SMT 检查限时 1000 ms；UNSAT/UNKNOWN 均不证明 SAFE。

---

## 7. sound gate 全清单（【铁证】，steps 1-4 之后）

每个 gate 都是**能力边界**（「BMC 建不了 X → UNKNOWN」），不是文件补丁：

| # | gate | 能力边界 |
|---|---|---|
| 1 | 执行前缀中的 Unsupported / call summary | C11 原子及无摘要的 API 不能按普通访问或 no-op 编码；未调用函数和目标后的同步不直接否决全部候选 |
| 2 | `incomplete` | 构造预算耗尽；回边截断自身不证明不可达或安全 |
| 3 | `transitive_ordered` | 隔代 create/join（2 线程模型看不到中间线程的 join）|
| 4 | `same_global` + offset | 非 singleton / 非同一全局对象不确认；偏移作可能重叠预筛，支持的变量下标交 SMT |
| 5 | 临界区历史 | 已取代旧 held 提前门；同锁及 reader/writer 模式在完整历史中编码 |
| 6 | 资源上限 | 路径 256 / 事件 1024 / DFS 步数 16384 / 栈深 256；每组尝试 32 对、全局 256 对 / 1024 次路径查询 / 10 秒搜索预算，耗尽不确认 |
| 7 | 地址/锁身份/所有权提取失败 | 未知别名不能创建不同假锁；重入及不平衡历史不确认 |
| 8 | A1 值/地址来源 gate | 真实合法来源、本次 invocation 的最近定义；load 需受支持的 SC 供值；合成初始化不作真实 nondet |
| 9 | 线程激活 / 实例存在性 | 需要具体创建前缀；may-multiplicity、FPA may 回调和条件/循环的站点数量不能证明实例存在 |
| 10 | 调用和前缀可执行性 | 无摘要的调用和可能陷阱的操作不省略；helper 需实际返回前缀 |
| 11 | 消费值的来源/定义性 | store/return/call/main 创建前缀也检查；合成初始化不证明真实赋值，局部 load 不复用前一次 allocation 的 store |
| 12 | verifier 原子段 | 完整段排除所有其他线程事件；未闭合目标前缀缺尾部闭包，不确认 |

地址 A1 的实现、回归范围及全量 A/B 记录见 [本轮验证报告](ikos-bmc-a1-address-2026-10-08.md)。

**去启发式史（steps 1-4）**：acquire/release 归入 unmodeled、`function_has_sync_recursive` 补查 mutex/cond_wait、
`is_induction_var` 只留局部 SSA、held 锁门注释改写成边界。TP 96→80，FP=0/FN=0 全程保持。

**已实现诊断**：`IKOS_BMC_DIAGNOSTICS=1` 在组报告 `info.bmc` 中记录候选编号、观察到的原因计数、
首次记录的阻塞、路径数量及有上限的 AR/源码示例。独立的 checker 前提可同时记录；被阻塞后未执行的阶段不推断原因。
`python3 -B scripts/bmc_diagnostics.py --output <新目录>` 保留原始报告、日志和逐任务结果，统计所有 UNKNOWN（包括预期安全任务）。
原因可重叠，候选层和任务层计数不可混用，拦截任务数不等于解除 gate 后能恢复的 TP 数。
历史候选轮及本轮诊断与正常模式的 1029 个 verdict 全部一致。`confirmed_path_a/b`、
`confirmed_startup`、`smt_model` 供复核；startup 的 clock 是前缀顺序标签，不是 solver clock。
这些是本编码的证据，不是独立验证。详见 [精度恢复报告](ikos-bmc-precision-2026-10-09.md)。

---

## 8. 与 Dartagnan 的对照（【铁证】机制映射 + 【待验证】缺口实现）

| Dartagnan 原理 | IKOS 现状 | 缺口 |
|---|---|---|
| §1.1 的 CAT hb，`com=rf\|fr\|co` | po+同步与按需有界 SC 读来源 | 未实现完整 CAT/一般 K 线程 rf/co；两者不是等价编码 |
| 全程序 SMT、N 线程 | 2 路径、有预算的多候选搜索 | 缺一般创建上下文、动态实例和 N 线程 |
| Andersen + `sameAddress` | singleton pts + offset + SMT 同址门 | 部分（堆对象未放开）|
| `canRace` 非原子 | 原子混合和未建模同步保守挡住确认 | 支持边界不同，未证明等价 |
| loop-unroll + bound | 保留真实条件和动态定义，尝试 bound=1/16 | 预算受限且没有无界 SAFE 证明；不能称等价 |
| violation witness | adapter 使用 YAML 2.2 `violation_sequence`；另有本地 GraphML 投影验证 | 2027 要求并发 violation witness 2.2 或以上；本地 4.2.2 不能解析 2.2，旧 GraphML 验证不能严格绑定实例/值/候选对，需现代 validator 与结构化反例 |

---

## 9. 当前未完成项与顺序（实测问题和设计建议分开）

后续采用[参考工具驱动的逐用例实施方案](ikos-bmc-reference-guided-plan.md)：
先预测具体执行，再观察固定版本 Dartagnan，迁移通用语义并验证正反例与全量结果。
每次精度扩展同时补齐所需反例证据，不必等待完整通用验证框架全部完成。
原来的 285 个 UNKNOWN 已全部参考分析，新增 UNKNOWN 也补齐记录；
动态创建、符号地址/锁、startup load 和实际 callback 已按该流程扩展。
逐任务结果、模型探针、两个新上游 FN 变体与后续范围见[本轮报告](ikos-bmc-unknown-sweep-2026-10-09.md)。

1. **先维持可复核的正确性基线**：按源码前缀和实际访问对复核每个确认 TP；增加安全对照与变体。
   SAT、YAML 标签及得分一致都不能替代执行存在性证据。当前逐项记录见正确性审查报告。
2. **完成源程序/前端契约与独立 witness 验证**：保留真实 nondet 与合成初始化的区别；
   store/return/call 的 synthetic/undef 消费检查已补齐；仍须核对其他 undefined/poison、
   内存有效性及库契约，并把实际所选访问对送入兼容格式的数据竞争验证器。
   旧 GraphML 本地验证已落地，局限和严格反例记录方案见 [见证验证报告](ikos-bmc-witness-validation-2026-10-09.md)。
   没有证明所有 C 未定义行为前提或所有 SAFE 过滤规则。
3. **扩大 IKOS 事实协同**：候选 offset、真实创建前缀、AR 初值和受限局部无溢出证书已接入。
   本轮关闭候选 bounds/no-wrap 数值证书的消融没有减少所选任务的 TP，尚无稳定性能收益证据；
   共享读区间可能 stale，不能直接用作值证书。aggregate interference 暂通过 forget 保持覆盖，需字段化恢复精度。
   下一步需为关系/别名/循环信息建立上下文明确的 may/must 契约及消融实验。
4. **扩大创建/调用动态实例、锁属性与库摘要**：实际参数/返回、条件创建、锁指针和 reader/writer 模式已有受限支持；
   仍缺一般间接调用、失败行为/属性和更完整创建上下文。每次扩展须保留 FP 对照，不能用 may 信息证明实际存在。
5. **扩大候选依赖闭包 + 有界 SC 值流**：机器整数、AR 初值、startup 及两前缀 demanded load 已有受限实现。
   单仿射符号地址供值和有界迭代已实现；仍需混合宽度/指针供值、必要的其他线程、join 身份及 atomic 目标尾部闭包。
   遗漏行为须证明无关或给出摘要。
6. **再扩大堆对象、K 线程与搜索精度**：建立动态对象/线程身份、生命周期后推进；
   诊断计数是阻塞观察，不能直接换算 TP 收益。

多候选搜索和诊断已经实现。之前 53→27→7 属于正确性收紧，本轮 7→35 属于在保留检查前提下的精度恢复。
不能把一次增加 `scull` 或总体 FP=0 当成 soundness 证明；完整配对和支持前提见本轮报告。

此前 TP≈140、“约 35 个数学上追不到”及未经同配置配对的 Dartagnan 分数对比没有充分证据，已撤回。
未知任务规模不是可恢复收益，也不能从有界架构直接推导某个固定任务集合不可处理。

---

## 10. 设计决策史（从 ikos-bmc-design.md 并入，避免文档冗余）

### 10.1 核心设计决策：不重写 checker，复用它的产物（【铁证】）

BMC 不是从零扫 AR，而是接管抽象解释判为「潜在竞争」的那一对访问：
- **用 checker 的**：访问事件 / 线程归属 / 竞争对语句（定位 load/store + 线程入口）。
- **复用 may 信息但不作具体证据**：points-to/offset 可预筛及约束候选；同抽象 cell 不能证明同一动态堆对象。
- **同步需具体身份和历史**：抽象锁集可以提供分析信息，但不能凭 may-held 宣称互斥；当前 SMT 重算实际前缀。
- **已实现事实接口**：`CandidateFacts` 传入 offset bounds、实际创建前缀与受限 no-wrap 证书，
  `initial_integer` 复用 AR 初值及 DataLayout。当前数值证书消融及进一步协同的依据见 UNKNOWN 全量报告。

### 10.2 helper 内联暴露的 4 类 FP（gate 的历史根因）（【铁证】）

| FP 机制 | 代表文件 | 对应 gate |
|---|---|---|
| 单调用原子函数内联（裸访问） | pthread-atomic | `bmc_skip_inline` |
| helper 带循环（历史 DAG 展开不精确） | — | 已替换为保留 guard/动态定义的有界展开和预算检查 |
| 无锁协议（线性化非 hb） | elimination_backoff_stack | `function_has_atomic_section` |
| 隔代 create/join（2 线程看不到中间线程） | race-4_1 | 隔代祖先门（`_thread_ancestors`−`_thread_creators`）|

### 10.3 可发展的架构机会（需要配对实验）

| 方向 | IKOS 可利用的能力 | 需要建立的证据 |
|---|---|---|
| 数值关系及浮点 | 现有数值域、前端和不动点分析 | 域与机器/浮点语义契约、任务覆盖及候选查询；不能从 APRON 存在推导 race 能力 |
| 数组及 join 句柄 | 地址/数组摘要、创建与 joined 信息 | 动态实例及句柄绑定证明，不能从数组摘要直接证明 join 哪个线程 |
| 候选关系压缩 | points-to、offset、线程事实 | may/must 的适用条件、剪枝保持性与同配置消融 |

原“Dartagnan 浮点直接 CRASH、数组 join 全 UNKNOWN”的对比缺少当前版本及同配置配对证据，
不作为确定优势；待复核后再比较。

---

## 11. 已复现并修复的 soundness 缺陷（2026-10-08，Codex 审查）

Codex 审查（`docs/ikos-bmc-review-2026-10-08.md`）复现了 1 FP + 2 FN，均已修复并提交：

| 用例 | 问题 | 根因 | 修复 commit |
|---|---|---|---|
| `exclusive-flag.c` | **FP**：无竞争程序报 FALSE | 自由变量：两线程读同一 shared var 各取不同值，造出程序没有的执行 | `4910339`（A0 gate）|
| `published-before-init.c` | **FN**：真竞争报 SAFE | `private_init` 未证对象未发布即删候选 | `df63329`（禁用）|
| `overlapping-widths.c` | **FN**：真竞争报 SAFE | 偏移过滤无访问宽度 | `df63329`（width+范围重叠）|

**核心教训**：「BMC 是欠近似 ⇒ FP=0」证伪。BMC 结构上欠近似（有界路径）、数据上过近似（自由变量），
两种近似方向同时存在，不能靠模块名称判断。FP=0 是 A0 gate 主动封自由变量后的**实测结果**，不是架构保证。

**历史代价**：TP 80→50，30 项被撤回确认。旧编码缺少充分值来源前提，但不能据此断言每条旧反例虚假，
更不能说这些任务没有真实竞争。恢复须补齐值、执行及实例证据，不能再用自由 load。
本轮在后续更严格的 TP=7 基线上实现算术、参数及受限 SC 值流，恢复到 35。
复现脚本 `docs/review-repros/bmc-2026-10-08/run.py`。

---

*铁证来源：Dartagnan `7caef5c` 源码（svcomp.cat / PropertyEncoder.encodeDataRaces / VisitorLlvm）、
IKOS `unroll.hpp` / `encode.hpp` / `bmc.hpp` / `data_race.cpp`、实跑 Dartagnan 4.3.1 与 IKOS A/B。*
