# IKOS BMC 模块设计 —— AR → SMT 映射

> 目标：接在 `--demote-race-to-unknown` 之后，对每个 demote 成 UNKNOWN 的抽象竞争对，
> 用 Z3 精确判定「这两个访问是否真的能并发（真竞争）」。SAT ⟹ 报 FALSE（找回 TP）；
> UNSAT ⟹ 保持 UNKNOWN（有界内无竞争）。永远不报 TRUE（证明 SAFE 交给抽象解释）。

## 0. 复用 checker 已有的提取（不要重写）

`DataRaceChecker` 已经做了大部分事件提取，BMC 模块**先复用它的产物**：

| 需要的东西 | 现有代码 | 说明 |
|---|---|---|
| 访问事件（load/store） | `check_load` / `check_store`（data_race.cpp:2072/2129） | 已解析 `load->operand()`/`store->pointer()` → points-to、offset、thread、locks |
| 线程归属 | `current_thread_id`（data_race.cpp:1480） | 沿 call-context 链上溯到线程入口 |
| 锁集 | `snapshot_locks` / lockset 域 | 抽象锁集（**BMC 不用它，见 §4**） |
| 竞争对 | `_accesses`（AccessRecord 列表）+ 析构里的 pairwise 判定 | demote 后的 UNKNOWN 对就在这里 |

**关键结论**：BMC 模块**不是**从零扫描 AR，而是**接管 checker 析构里被判为「潜在竞争」的那一对访问**（`_accesses` 里的两个 AccessRecord），只对它们做精确判定。查询面窄到 O(1) 对，不用全程序展开所有对。

## 1. 事件模型

BMC 需要两类事件，都从 AR 静态可得：

```
内存事件：
  ar::Load   →  Read 事件，地址 = load->operand()
  ar::Store  →  Write 事件，地址 = store->pointer()

同步事件（建立 hb）：
  ar::CallBase 且 callee 名含 "pthread_mutex"/"pthread.mutex"：
     含 "lock"   → Lock(acquire)  事件，锁对象 = arg0
     含 "unlock" → Unlock(release) 事件，锁对象 = arg0
  pthread_create → Spawn 事件（create-HB）
  pthread_join   → Join 事件（join-HB）
```

每个事件带：全局 id、所属线程（`current_thread_id`）、地址/锁对象、读写类型。

## 2. 程序序（po）

`ar::BasicBlock::_successors` 给 CFG 后继。同一线程内，事件沿 CFG 的先后就是 po。

- **MUST 边**：直线代码段里的 po（无分支时必然成立）。
- **分支/循环**：po 变成 CFG 路径关系，要发 `exec(e)`（事件是否被执行）布尔变量 + 控制流约束（见 §5 的「工程分期」）。

## 3. happens-before（hb）与竞争判定（核心，已用原型验证）

沿用 svcomp.cat 的 SC 语义：`hb = po ∪ 线程内通信 ∪ mutex/原子同步`。

```
对每个事件 e：clock(e) : int        # SC 线性化里的位置（拓扑序）
对每条 MUST hb 边 (x,y)：clock(x) < clock(y)

竞争查询（针对 checker 给的那对 w,m，w=写、m=另一访问，跨线程、同址）：
  SAT(  clock(w) == clock(m)+1  ∨  clock(m) == clock(w)+1 )
```

**「相邻 ⟺ 竞争」已用 `scripts/bmc_race_encoding.cpp` 实测验证**：异锁 SAT、同锁 UNSAT。

## 4. BMC 用哪些、不用哪些（关键架构判断）

| checker 现成的 | BMC 用不用 | 为什么 |
|---|---|---|
| `resolve_points_to`（抽象 points-to） | ❌ 不用 | 它是过近似；BMC 要精确的 may-alias |
| lockset（抽象锁集） | ❌ 不用 | 抽象锁集不精确；BMC 要在 SMT 里重算 mutex 同步 |
| `current_thread_id` | ✅ 用 | 线程归属是静态事实 |
| 竞争对的访问语句（`AccessRecord.stmt`） | ✅ 用 | 定位这两个 load/store 语句 |
| 地址别名 | ✅ 但用**精确 may-alias** | 判断两个访问是否同址；IKOS 有 pointer 分析，看能否给精确 alias |

## 5. 工程分期（从最小到完整）

**第一期（最小可跑，先找回 mutex 竞争的 TP）**
- 只对 checker 报的那一对访问，做**路径敏感的有限展开**：从线程入口展开到这两个访问，展开界 K。
- 编码：`exec(e)`（控制流）+ po + mutex 同步 + clock + 相邻查询。
- 处理直线代码 + 简单分支；循环展开 K 次。
- 地址别名：先假定「同一抽象 cell ⟹ 可能同址」（保守），后续用精确 alias。

**第二期（找回锁无关/原子 TP）**
- 加 MAY 边：`rf`（读从哪个写）、`co`（写序）发自由布尔变量。
- 加原子 acquire/release（若需要）。

**第三期（工程化）**
- 从「只判一对」扩展到「判所有 demote 对」（仍是 O(竞争对数)，远小于 Dartagnan 的 O(事件²)）。
- 求解器缓存、增量求解。

## 6. 集成点

在 `DataRaceChecker` 的析构（pairwise 判定）里，`--demote-race-to-unknown` 把竞争对判成 UNKNOWN 之后，**对每个 UNKNOWN 对调用 BMC 模块**：

```
对每个 demote 成 UNKNOWN 的 (a, b)：
  BMC.check_race(a.stmt, b.stmt, bound=K)   # 路径敏感展开到 a/b
    SAT   → 把这对升级成「definite race」（报 FALSE，+1 分）
    UNSAT → 保持 UNKNOWN（0 分）
```

result：BMC 只「找回 TP」，从不「证明 SAFE」，所以 **FN=0 恒成立**（BMC 报 FALSE 时给出的是真竞争）。

## 7. 待摸清的点（动手前）

1. **精确 may-alias**：IKOS 的 pointer analysis 能不能给「两个 load/store 是否可能指向同一地址」的精确答案（用于 conflict 判定，替代抽象 points-to 的过近似）。
2. **路径敏感的有限展开**：IKOS 现在是 path-insensitive（join）；BMC 需要 path-sensitive 展开到那两个访问。看能否复用 `ar::Code` 的 CFG + 一个轻量展开器，还是直接手写。
3. **mutex 对象的身份**：pthread_mutex_lock(arg0) 的 arg0 指向哪个 mutex（用于 unlock→lock 的同步配对）。checker 里锁集以 mutex 地址为 key，可参考。
