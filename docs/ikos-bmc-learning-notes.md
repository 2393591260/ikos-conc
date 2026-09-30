# Dartagnan 内存模型与 race 检测：IKOS BMC 模块学习笔记（一）—— CAT 讲义

> 目的：给 IKOS 的 BMC 模块（从 demote 后的 UNKNOWN 里精确找回真竞争）打理论底子。
> 结论先说：**SV-COMP no-data-race 用的是 SC（`svcomp.cat`），不是 RC11**。所以 IKOS 的 BMC
> 模块只需要「SC 下的 race 检测」，不需要弱内存模型（acquire/release/fence），难度远比想象的低。

## 0. 三个 CAT 文件的关系（先分清用哪个）

| 文件 | 是什么 | IKOS 用不用 |
|---|---|---|
| `cat/sc.cat` | 纯顺序一致性（SC） | 参考 |
| `cat/svcomp.cat` | **SV-COMP 用的 SC 变体** | **✅ 就是它** |
| `cat/rc11.cat` | 修复版 C11（PLDI'17，含 acquire/release） | ❌ 不需要 |
| `cat/stdlib.cat` | 所有模型共用的「基础关系」定义 | ✅ 参考（fr/rmw/po-loc 等） |

`Dartagnan-SVCOMP.sh` 里写死了 `cat/svcomp.cat`（不是 rc11），所以 SV-COMP 的 Dartagnan 提交
做的是 **SC 数据竞争检测**。

## 1. CAT 语言 30 秒速览

CAT（*Herding Cats*，Alglave 等）是**声明式内存模型语言**：用**关系代数**定义一组关系（relation），
再用**约束**（acyclic / irreflexive / empty / flag）声明它们必须满足什么。核心符号：

| 符号 | 含义 |
|---|---|
| `po` | program order（程序序，同一线程内的指令顺序） |
| `rf` | read-from（读从哪个写读到的） |
| `co` | coherence（写序：每个地址上的写之间的全序） |
| `fr` | from-read（写 → 读了它之前那个写的读） |
| `loc` | same-location（同一地址） |
| `ext` / `int` | external / internal（跨线程 / 同线程） |
| `W` / `R` / `IW` | 写 / 读 / 初始化写 |
| `RMW` / `A` | read-modify-write / 原子（atomic） |
| `&` `\|` `;` `\` `^` | 交 / 并 / 复合 / 差 / 逆 |
| `acyclic X as sc` | 声明「X 无环」并命名这个约束叫 sc |
| `flag ~empty X as racy` | 若 X 非空则报告 racy（竞争） |

**关键直觉**：内存模型 = 「哪些执行是合法的」。一个执行里，读通过 `rf` 连到它读的写，写之间用 `co`
排序，`po` 是程序序。合法的执行必须满足这些关系的约束（如无环）。「数据竞争」= 存在两个冲突访问
不在 happens-before（hb）里。

## 2. `stdlib.cat` 逐条注释（公共基础）

```cat
let fr = rf^-1;co | ([R] \ [range(rf)]);loc;[W]
```
`fr`（from-read）= 两条路：(1) 逆 rf 再 co（写 W1 被 rf 到读 R，R 又 co 在 W2 前，则 W2 --fr--> R）；
(2) 一个没读到任何写的读（未初始化读）与同址任意写的组合。**fr 是「写覆盖了读之前的值」的关系。**

```cat
let po-loc = po & loc            (* 同址的程序序 *)
let rmw = amo | lxsx            (* read-modify-write = 原子 RMW | xchg 等 *)
let rfe = rf & ext              (* 跨线程 read-from *)
let coe = co & ext              (* 跨线程 coherence *)
let fre = fr & ext              (* 跨线程 from-read *)
let rfi = rf & int              (* 线程内 read-from *)
let coi = co & int              (* 线程内 coherence *)
let fri = fr & int              (* 线程内 from-read *)
```
这组只是给跨线程/线程内版本起短名。`ext`/`int` 是 CAT 内建的「不同线程」/「同一线程」。

```cat
let fencerel(F) = po;[F];po      (* fence 夹在程序序中间 = fence 关系 *)
```

```cat
empty (([R] \ [range(rf)]);loc;[IW])
flag ~empty [UB] as undefined-behavior
```
未初始化读是 UB（undefined behavior）：如果没有初始化写，读不能读未初始化内存。

## 3. `svcomp.cat` 逐条注释（**IKOS 要复刻的模型**）

```cat
SVCOMP
let com = (rf | fr | co)        (* 通信关系 = 读序 ∪ from-read ∪ 写序 *)
let hb = po | (com & int) | (rmw^-1? ; (com & ext))
acyclic hb as sc
```

逐条：
- `com = rf | fr | co`：所有「通信」关系（数据是怎么流动/排序的）。
- `hb = po | (com & int) | (rmw^-1? ; (com & ext))`：**happens-before** =
  1. `po`：程序序；
  2. `com & int`：线程内的通信（同一线程里 rf/co/fr 自然有序）；
  3. `rmw^-1? ; (com & ext)`：**跨线程同步**——关键就是这条。`rmw^-1? ; com` 表示「一个原子
     RMW 的读半部分，通过跨线程 rf/co，连到另一个写」，这正是 **mutex unlock→lock** 的建模：
     unlock 是写、lock 是 RMW（读半），lock 通过 rf 读到 unlock 写的值 → 建立 hb。
- `acyclic hb as sc`：**顺序一致性 = hb 无环**。SC 下不允许 hb 成环（即不允许「因果倒置」）。

**一句话**：SV-COMP 的模型 = 程序序 + 线程内通信 + 互斥锁/原子的同步（unlock→lock），全部无环。

## 4. race（数据竞争）的定义（通用，取自 rc11.cat 底部）

```cat
let conflict = ext & ((((W * _) | (_ * W)) & loc) \ ((IW * _) | (_ * IW)))
let race = conflict \ (A * A) \ hb \ (hb^-1)
flag ~empty race as racy
```

逐条：
- `conflict` = 跨线程（ext）∧ 同址（loc）∧ 至少一个是普通写（W，排除初始化写 IW）。
  `W * _` = 任意（写, 任意），`_ * W` = （任意, 写），并起来 = 至少一个写。
- `race` = conflict，去掉 `A * A`（两个原子访问不算竞争，C11 定义），去掉 `hb`（a 在 b 之前），
  去掉 `hb^-1`（b 在 a 之前）。**剩下的 = 两个冲突访问没有 happens-before 关系 = 数据竞争。**
- `flag ~empty race as racy`：race 非空 → 报告「有竞争」。

## 5. 这对 IKOS BMC 模块意味着什么（架构结论）

IKOS 的 BMC 模块 = 一个 **SC 下的有界 race 检测器**。它要编码的关系只有：

1. **po**（程序序）：每条指令在它线程里的先后；
2. **rf / co**（读写关系）：每个读读自哪个写、每个地址的写序；
3. **hb** = po ∪ 线程内通信 ∪ mutex/原子同步（unlock→lock 的 rf 链）；
4. **race 查询**：∃ 两个冲突访问（同址、至少一个写、跨线程、非初始化写）不在 hb 里。

**不需要 rc11.cat 的 acquire/release/fence/`sw`** —— 那是弱内存模型的难点，SV-COMP 用不到。

> 与 IKOS 现有抽象解释的关系：IKOS 现在的 lockset/create-join 是 hb 的**过近似**（可能把真竞争漏判成
> 「已同步」）。BMC 模块要在 **SMT 里精确重算 hb**，才能把「抽象竞争」精确判成「真竞争」或「非竞争」。

## 6. 下一步

- 抓 Dartagnan 的 `EncodingContext` / `WmmEncoder` / `PropertyEncoder.encodeDataRaces` 源码，
  看这套关系是怎么落成 SMT 布尔公式的（学习笔记二）。
- 读 SC 编码的 soundness 论证（`acyclic hb` ⟺ 无竞争，在 RC11/SC 论文里）。

---

# 学习笔记（二）—— Dartagnan 的 SMT 编码怎么落地

> 源码：`dartagnan/src/main/java/com/dat3m/dartagnan/encoding/{EncodingContext,WmmEncoder,PropertyEncoder}.java`（4.3.1，已抓本地）。

## 1. 三个编码基础设施

| 类 | 职责 |
|---|---|
| `EncodingContext` | 提供 SMT 变量的工厂：`edgeVariable(name, e1, e2)`（布尔，表示「e1 --关系--> e2」）、`clockVariable(name, e)`（整数，事件在关系里的拓扑序号）、`execution(e)`（事件是否被执行） |
| `WmmEncoder` | 把 CAT 的关系/约束（如 `acyclic hb`）落成 SMT 公式 |
| `PropertyEncoder` | 把「要验证的性质」（数据竞争）落成 SMT 查询 |

**边变量 + may/must 集合优化**（`EncodingContext.edge`）：
```java
if (!may.contains(e1,e2)) return makeFalse();   // 不在 may 集 → 这条边恒假
if (must.contains(e1,e2))  return execution(e1,e2); // 在 must 集 → 边 = 两事件都执行
return variable.encode(e1,e2);                   // 否则 → 自由布尔变量
```
`RelationAnalysis` 预计算每个关系的 may（可能成立）/must（必然成立）集，把编码量从 O(事件²) 压到只对可能成立的对子发变量。**这是工程上省变量的关键。**

## 2. `acyclic hb` 怎么编码（拓扑时钟技巧）

`acyclic hb` = 「hb 无环」。直接编码「无环」要否定传递闭包（贵）。Dartagnan 用**拓扑序号（clock）**：

对 hb may 集里的每对 (x, z)，若两者都执行：
- 强制 `hb(x,z) ∨ hb(z,x)`（二选一，选定一个方向）；
- `hb(x,z) ⟹ clock(x) < clock(z)`（正向边 ⟹ 序号递增）。

于是「hb 无环」⟺「存在一个与 hb 边一致的拓扑排序（clock）」。这是经典的「无环 = 存在拓扑序」编码。

## 3. 数据竞争怎么编码（`PropertyEncoder.encodeDataRaces`）

```java
hasRace = false;
for (t1, t2 不同线程)
  for (w : t1 的 WRITE 事件, 跳过 INIT/原子)
    for (m : t2 的 MEMORY 事件, 跳过 INIT/原子/双 RMW)
      if (!alias.mayAlias(m, w)) continue;
      isConflictingPair = execution(m,w) && sameAddress(m,w);
      isAdjacentInHb    = hb(m,w) && (clock(w) == clock(m)+1);   // ← 关键技巧
      isRacingPair      = isConflictingPair && isAdjacentInHb;
      hasRace = hasRace || isRacingPair;
return (NOT DATARACEFREEDOM_var, hasRace);   // hasRace 可满足 ⟹ 有竞争
```

结构清晰：**枚举所有跨线程冲突对（同址、至少一个写、非原子），检查「在 hb 拓扑序里相邻」——相邻 ⟺ 两者之间没有 hb 链 ⟺ 无 happens-before ⟺ 竞争。**

**关键的「相邻」技巧**：不直接编码「不存在 hb 传递路径」（否定传递闭包，昂贵），而是用 `clock(w) == clock(m)+1`（拓扑序号差 1）+ `hb(m,w)` 来表达「m、w 在拓扑序里紧挨着」——紧挨着就说明中间没有别的 hb 事件把它们隔开，即没有 hb 链。

> ⚠️ **待验证的细节**：`hb(m,w) && clock(w)==clock(m)+1` 中 `hb` 到底是「直接边」还是「相邻对」的精确语义、以及为什么直接 hb 边不会把真竞争误判成「已排序」，我还没完全钉死（这涉及 hb 的 may 集在 SC 模型下到底包含哪些对）。**动手前必须拿一个具体竞争例子（如 `04-mutex_01-simple_rc.c`）在 SMT 变量层手工推演一遍**，确认「相邻 ⟺ 竞争」的双向等价。这一步不能跳。

## 4. 对 IKOS BMC 模块的落地启示

1. **变量层**：IKOS 要对 AR 的每个访问事件发两个 SMT 变量——`hb(e1,e2)`（布尔）+ `hb-clock(e)`（整数）。事件的全局编号 = AR 语句的唯一 id。
2. **hb 边**：`po`（同一线程程序序）+ `rf/co`（读写关系）+ mutex/原子的 unlock→lock 同步，这些边直接 `hb(e1,e2)=true` 或发自由变量。
3. **无环约束**：对每对可能 hb 的事件，`hb(x,z) ∨ hb(z,x)` + clock 递增。
4. **race 查询**：枚举 IKOS 报的那两个访问（而不是 Dartagnan 那样枚举所有对）——**IKOS 已经知道抽象竞争在哪，BMC 只需要精确判这两个访问是否真竞争**，查询面窄得多，可能不用全程序展开，只需展开到这两个访问可达的路径。
5. **有界性**：循环展开 K 次；SAT ⟹ 有竞争（sound），UNSAT ⟹ K 次内无竞争（incomplete，落 UNKNOWN）。

## 5. 下一步（按顺序）

1. 拿 `04-mutex_01-simple_rc.c` 手工推演「相邻 ⟺ 竞争」的双向等价（钉死 §3 的 ⚠️）。
2. 定 IKOS 的 SMT 求解器绑定（Z3 还是 Yices 的 C++ API；IKOS 已经链 APRON，看能否复用或新增）。
3. 定「只展开到竞争可达路径」的最小展开策略（复用 IKOS 的 AR + 线程结构）。

---

# 学习笔记（三）—— 手工推演 `04-mutex_01-simple_rc.c` 的编码

## 1. 事件图（Dartagnan 的语义层）

程序两条线程，锁不同（mutex1 vs mutex2），所以 `myglobal=myglobal+1` 竞争：

```
t_fun:  L1(lock mutex1) → R1(load myglobal) → W1(store myglobal) → U1(unlock mutex1) → return
main:   L2(lock mutex2) → R2(load myglobal) → W2(store myglobal) → U2(unlock mutex2) → join(t_fun)
```

- **po**（程序序，must 边）：L1→R1→W1→U1；L2→R2→W2→U2→join。
- **同步**（mutex）：L1 是 RMW、U1 是 store，同线程的 lock/unlock 是 po，**跨线程的 unlock→lock 才建 hb**（这里 t_fun 和 main 用不同锁，没有跨线程同步边）。
- **join**：t_fun 的 return 与 main 的 join 之间有一条跨线程 hb 边（create/join）。

## 2. 冲突对（race 候选）

跨线程、同址 `myglobal`、至少一个写：
- (W1, R2)、(W1, W2)、(R1, W2)、(W2, R1)、(W2, W1)、(R2, W1)。

全部**没有 hb 路径**（不同锁、无同步），所以**全部是真竞争**。

## 3. 编码如何判定（关键：`hb` 在编码里是「SC 全序」，不是「偏序 happens-before」）

这是钉死「相邻 ⟺ 竞争」的核心：

- `acyclic hb` 的编码 = **给所有事件一个拓扑序号（clock）**，且对每对事件强制「二选一」方向，等价于构造一个 **SC 线性化（全序）**。clock = 该事件在线性化里的位置。
- 于是 `hb(m,w)` 这个布尔变量在 race 查询里，含义是「**m 在 w 之前（在线性化里）**」，不是「m happens-before w」。
- **race 查询只看跨线程对**（`t1 != t2`），判定：

```
race(m,w) = 都执行 ∧ 同址 ∧ clock(w) == clock(m)+1   （“相邻”）
```

**为什么「跨线程相邻 ⟺ 竞争」**（这是双向等价的关键论证）：

- **⇒**：若 m、w 跨线程且在 SC 线性化里相邻，则它们之间没有别的访问事件。跨线程的 happens-before 只能靠同步事件（unlock→lock / join）建立，而同步事件必然落在两者中间（把它俩隔开）。相邻 = 中间无同步事件 = 无 happens-before = 竞争。
- **⇐**：若 m、w 跨线程且竞争（无 hb），则没有任何约束强迫中间有事件，求解器可以自由地把它们在线性化里排成相邻（clock(w)=clock(m)+1）。

（同一线程内的 po 直接相邻不算，因为 race 查询只枚举跨线程对。）

## 4. 对 04-mutex_01 的落地

- (W1, R2)：跨线程、同址、不同锁无同步 → 可排成相邻 → race 查询 SAT → **有竞争**。✓
- 对照一个**无竞争**例子（同锁）：W1 和 R2 用同一把锁时，lock/unlock 的同步边迫使「W1 → U1 →(同步)→ L2 → R2」成为一条 hb 链，U1/L2 落在中间 → W1、R2 **不可能相邻** → race 查询 UNSAT → 无竞争。✓

## 5. 诚实标注：这个「相邻 ⟺ 竞争」还是我推演出来的，没经过代码逐行验证

- 我读了 `PropertyEncoder.encodeDataRaces`（枚举跨线程冲突对 + `hb(m,w) && clock(w)==clock(m)+1`）和 `WmmEncoder` 的 clock 编码，但**「hb 变量 = SC 全序」这个关键假设，是从『acyclic hb 强制每对二选一』推出来的，没在代码里找到一句注释直接这么说**。
- 正因如此，**第 4 节的同锁对照（无竞争 → 不可能相邻 → UNSAT）必须用一个最小原型实测**：拿同锁版和异锁版各跑一次，看 SAT/UNSAT 是否如预期。这一步是代码级验证「相邻 ⟺ 竞争」的唯一硬证据，也是你最初担心的「只看代码无法确认 soundness」的实证回答。
