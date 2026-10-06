# IKOS 条件变量语义建模方案（pthread_cond_wait / signal / broadcast）

> 作者：IKOS 并发分析模块开发
> 前置：本文完全基于 IKOS 现有代码框架（thread-modular + 锁集 digest + ConcurrentGlobalEnv 黑板），
> **soundness 优先**：宁可 FP，绝不 FN。

---

## 1. 调研结论：IKOS 并发同步原语核心架构

### 1.1 分层架构

| 层 | 文件 | 职责 |
|---|---|---|
| 前端导入 | `frontend/llvm/src/import/library_function.cpp:254-277` | `pthread_*` → `ar::Intrinsic` 映射；**cond 系列刻意不映射**，保留为 extern 供引擎按名路由 |
| AR 内建 | `ar/semantic/intrinsic.hpp:165-170` | 仅 `PthreadCreate/Join/MutexLock/MutexUnlock` 四个内建 |
| 执行引擎 | `analyzer/include/.../execution_engine/concurrent_semantics.hpp`（原 `numerical.hpp`，已剥离 commit `56f20a7`） | `exec_pthread_mutex_lock/unlock/join/cond_*` 等按名路由与语义执行（自由函数模板，`numerical.hpp` 内只留门控守卫调用） |
| 锁集域 | `core/include/.../lockset/lockset_domain.hpp` | MUST 锁集 + 读锁集 + 条件锁 + `_joined_threads`(MUST) + `_spawned_threads`(MAY) |
| 全局黑板 | `core/include/.../concurrent_global_env.hpp` | `_global_board`、`_lock_partition`(PBR)、指针黑板、线程句柄映射、escape 集合 |
| 竞争检查器 | `analyzer/src/checker/data_race.cpp` | 析构期 O(N²) 两两配对，用 HB digest 短路 |

### 1.2 锁集（LocksetDomain）—— HB digest 的载体

`lockset_domain.hpp` 把一个「访问点」的并发上下文编码为分量域，其中两个**线程 digest** 是 happens-before 的关键：

```cpp
ThreadSet _joined_threads;   // MUST：join 用「集合交」   (join_digest)
ThreadSet _spawned_threads;  // MAY ：join 用「集合并」   (join_spawned)
bool _joined_top;            // ⊤ = 空集 = 未知
bool _spawned_top;
```

- `_joined_threads`（MUST）：某程序点「**必然**已 join 的线程」。CFG join 用**交集**（两条路径都必须 join 才算）。
- `_spawned_threads`（MAY）：某程序点「**可能**已 spawn 的线程」。CFG join 用**并集**。

这是 HB 边建立的地基：HB 边 =「A 的访问点 digest 里含有 B 的线程标识」。

### 1.3 现有 happens-before 传递（data_race.cpp 析构器）

```cpp
// join 边：A 已 join B ⇒ B 终止先于 A 的访问
if (a.joined_threads.count(b.thread_id)) continue;
// create 边：A 的 spawn digest 不含 B ⇒ A 在 create 之前 ⇒ A 写 HB B 全部
// private-init 边、self-locked 边、offset 消歧 ...
```

**核心结论**：HB 传递是**成对、按 digest 短路**的，不是传递闭包。每个 HB 边对应一个「线程 digest + 检查器短路规则」。

### 1.4 全局黑板（ConcurrentGlobalEnv）

- `_global_board`：地址 → 区间，无锁写干扰的扁平黑板。
- `_lock_partition[lock]`：PBR（保护式读，Goblint SAS'21）按锁分区的干扰值。
- `record_privatized_write / flush_privatized`：持锁写延迟发布。
- `std::mutex` 保护（tbb 并行 checker 读保护）；**已去单例化 commit `bf51ec0`**，由 `Context::concurrent_env` 持有，经 `_ctx.concurrent_env`/`eng.ctx().concurrent_env` 访问（本文下方 §3 代码骨架里的 `ConcurrentGlobalEnv::get_instance()` 是旧写法，现已改为传引用）。

### 1.5 条件变量现状（空桩）

| 原语 | 位置 | 现状 |
|---|---|---|
| `pthread_cond_wait` | `concurrent_semantics.hpp` 的 `exec_pthread_cond_wait` | **可达性 stub**：MUST-持锁时释放+重获（净锁集不变）+ `mem_forget_all()`，返回 nondet |
| `pthread_cond_signal/broadcast` | `concurrent_semantics.hpp` 的 `exec_pthread_cond_signal` | **no-op stub**：仅返回 nondet |
| 路由 | `numerical.hpp` 的 `exec_extern_call` / `exec_unknown_extern_call` 守卫 | 按名路由到上面两个 stub（刻意不走 `exec_unknown_extern_call`，避免 `throw_unknown_exceptions` 把锁集打到 ⊤） |

**现状 soundness 结论**：现有 stub 是 **sound 的**——它靠「互斥锁纪律」提供正确保护（wait 重获 mutex，消费者 wait 后的读与生产者持锁写同锁），**不建立 signal→wait 的 HB 边**（因为该边是 MAY 边，见 §2.4）。代价是 signal-only 排序的 FP（如 `cond-nohb` 测试）。

---

## 2. 条件变量语义建模完整方案

### 2.0 设计原则（soundness 第一）

POSIX 条件变量语义中，**signal→wait 的 happens-before 边本质是 MAY 边**：

1. **伪唤醒**：`pthread_cond_wait` 允许**无信号**就返回（spurious wakeup）。
2. **signal 只唤醒一个**：多个等待者时，signal 只解除**其中一个**；分析不知道是哪个。

因此「A 曾 signal C 且 B 曾 wait C」**不能推出**「A 的 signal 唤醒了 B」——B 可能伪唤醒（早于 A 的 signal），也可能被别的 signal 唤醒。**若据此跳过竞争，就是 FN（漏报），违反红线。**

**结论：sound 的建模必须把 signal→wait 边当作 MAY 边，且不用它去短路竞争。** 真正的 sound 保护来自「互斥锁纪律」。本方案在保住这一底线的同时，做两件 sound 的事：

1. **精化 wait 的「阻塞期间干扰物化」**：用 PBR 分区 join 取代粗粒度 `mem_forget_all`（更精确、仍 sound）。
2. **显式建模 broadcast 与等待队列**（为完整性 + 未来值分析 + 一个 sound 的局部 HB 见 §2.4）。

### 2.1 新增数据结构

**A. `ConcurrentGlobalEnv` 增加条件变量等待队列与信号纪元**（`concurrent_global_env.hpp`，仿 `_thread_handles`）：

```cpp
/// 条件变量等待队列：cond 地址 → 当前阻塞在其上的线程标识集合（MAY）。
/// wait 入队、被 signal/broadcast 出队；用于 (1) broadcast 全唤醒建模
/// (2) 未来值分析的唤醒路径精化。不做竞争短路（soundness）。
std::unordered_map< std::uint64_t /*cond addr*/, std::unordered_set< std::string > >
    _cond_waiters;

/// 信号纪元：cond 地址 → 累计 signal/broadcast 次数（单调）。
/// 供「wait 返回时是否可能被某信号唤醒」的 MAY 判定。
std::unordered_map< std::uint64_t, std::uint64_t > _cond_signal_epoch;
```

**B. `LocksetDomain` 增加两个条件变量 digest**（`lockset_domain.hpp`，仿 `_joined_threads`/`_spawned_threads`，**MAY 语义**）：

```cpp
/// 本线程「可能已 signal 的条件变量」集合（MAY：join 用并集）。
/// 键为 cond 地址（offset_lock_key 打包）。
ThreadSet _signaled_conds;   // 实际是 std::unordered_set<uint64_t>
bool _signaled_top = false;

/// 本线程「可能已从 wait 返回的条件变量」集合（MAY）。
ThreadSet _awaited_conds;
bool _awaited_top = false;
```

> 说明：这两个 digest 记录「发生过 signal / 经历过 wait」的事实，用于 §2.4 的唯一 sound 用法（值分析物化 + 未来扩展）。**竞争短路不用它**。

### 2.2 `pthread_cond_wait` 完整执行语义

POSIX：原子地释放 mutex → 阻塞 → 被唤醒 → **重获 mutex** → 返回。

```cpp
void exec_pthread_cond_wait(ar::CallBase* call) {
  // (1) 解析 cond 地址（arg0）与 mutex（arg1）
  std::uint64_t cond_key = resolve_cond_addr(arg0);     // 失败则 ⊤，仍 sound
  std::uint64_t mutex_key = resolve_mutex_addr(arg1);   // 单点 points-to + 常量偏移

  // (2) 释放 mutex：若 MUST-持锁则从锁集移除（sound：wait 确实释放它）
  bool held = this->_inv.normal().lockset_holds_lock(mutex_key);
  if (held) this->_inv.normal().lockset_remove_lock(mutex_key);
  //    若锁集为 ⊤ 或 mutex 非单点 → 不移除（保守：可能还持有 → 少移除只多报 race）

  // (3) 入队：登记本线程阻塞在 cond 上（MAY）
  auto& env = ConcurrentGlobalEnv::get_instance();
  if (cond_key != 0) env.cond_wait_enter(cond_key, this->thread_id());

  // (4) 阻塞期间干扰物化（sound 精化点）：
  //     其他线程可能改共享全局 → 用 PBR 分区 join + 忘记共享内存，替代粗粒度 mem_forget_all
  env.forget_shared_memory(this->_inv, /*also_publish=*/true);   // 见 §3 代码

  // (5) 重获 mutex：POSIX 保证 wait 返回时持有 mutex
  if (held && mutex_key != 0) this->_inv.normal().lockset_add_lock(mutex_key);

  // (6) 标记「已 wait 过 cond」（MAY digest，供未来值分析）
  if (cond_key != 0) this->_inv.normal().lockset_add_awaited_cond(cond_key);

  // (7) 返回 nondet
  scalar_assign_nondet(ret);
}
```

**关键 soundness 点**：
- **步骤 2/5 的释放-重获**：`held` 为 MUST-持锁时才动锁集。锁集 ⊤ 或非单点时不加锁（少认锁 → 只多报 race，绝不漏报）。这与现有 stub 一致。
- **步骤 4**：现有 `mem_forget_all()` 把所有内存（含本线程局部变量）打 ⊤，是 sound 的但过粗。精化为「忘记**共享**内存 + join 该 mutex 分区的干扰」，保留线程局部精度。

### 2.3 `pthread_cond_signal` / `broadcast` 完整执行语义

```cpp
void exec_pthread_cond_signal(ar::CallBase* call) {   // broadcast 用同一函数 + 标志
  std::uint64_t cond_key = resolve_cond_addr(arg0);
  auto& env = ConcurrentGlobalEnv::get_instance();
  if (cond_key != 0) {
    env.cond_signal(cond_key, /*broadcast=*/is_broadcast);  // 纪元 +1，唤醒 1 个/全部等待者
  }
  // 标记「已 signal cond」（MAY digest，供未来值分析）
  if (cond_key != 0) this->_inv.normal().lockset_add_signaled_cond(cond_key);

  // 无锁集/内存副作用：signal 只解除阻塞，不保护任何数据
  scalar_assign_nondet(ret);
}
```

- **signal**：`_cond_waiters[cond]` 中移除至多一个等待者，`_cond_signal_epoch[cond]++`。
- **broadcast**：移除全部等待者，`_cond_signal_epoch[cond]++`。
- 两者**都不动锁集、不动内存**——这是 sound 的：signal 本身不提供互斥。

### 2.4 happens-before 边的建立与传递

**sound 的 HB 只有一条（互斥锁纪律），signal→wait 边因 MAY 性不用于短路：**

| HB 边 | 语义 | 是否用于竞争短路 | 理由 |
|---|---|---|---|
| **mutex 纪律**（wait 重获 m） | 生产者 `lock(m); 写 data; unlock(m); signal` ↔ 消费者 `lock(m); wait; 读 data`（wait 返回重获 m） | ✅ 是 | 双方 data 访问同锁 m，锁集交非空 → 不报 race |
| **signal→wait**（A signal C，B 从 wait C 返回） | A 在 signal 前的写 HB B 在 wait 返回后的读 | ❌ **否**（MAY 边） | 伪唤醒 / 多等待者使该边非 MUST，用于短路会 FN |

**MAY digest 的唯一 sound 用途（值分析物化，非竞争短路）**：

wait 返回后，本线程要「看到」唤醒它的线程在 signal 前写的值。sound 做法是：**signal 时把「当前持锁者可能发布的私有化写」并入全局黑板 / 该 mutex 分区**，wait 返回时用现有 `read_global_with_lockset` 读到。这一步已在 §2.2 步骤 4 的 PBR 精化中覆盖——不引入新的、可能 unsound 的 HB 边。

> 一句话：**signal→wait 的 HB 通过「mutex 分区干扰的发布-读取」体现，而非通过一个新的成对短路 digest**。这样既拿到值传播，又不牺牲 soundness。

---

## 3. 代码修改方案

### 3.1 修改文件清单

| 文件 | 改动 | 位置 |
|---|---|---|
| `core/include/.../concurrent_global_env.hpp` | + `_cond_waiters`/`_cond_signal_epoch` 成员 + `cond_wait_enter/cond_signal/cond_wait_exit` + `forget_shared_memory` | 私有成员区（`_thread_handles` 旁）、公开方法区（`map_thread_handle` 旁） |
| `core/include/.../lockset/lockset_domain.hpp` | + `_signaled_conds`/`_awaited_conds`（MAY digest）+ `add_signaled_cond/add_awaited_cond/signaled_conds/awaited_conds` + join/meet/leq/eq 分量 | 仿 `_spawned_threads` |
| `analyzer/include/.../execution_engine/concurrent_semantics.hpp`（原 `numerical.hpp`，已剥离 commit `56f20a7`） | 重写 `exec_pthread_cond_wait` / `exec_pthread_cond_signal`（§2.2/§2.3）；路由 `pthread_cond_broadcast` 到 signal(广播)。现为自由函数模板 `exec_pthread_cond_wait(E& eng, call)` / `exec_pthread_cond_signal(E& eng, call, broadcast)` | `concurrent_semantics.hpp` 的 `exec_pthread_cond_*` + `numerical.hpp` 的按名路由守卫 |
| `core/include/.../lockset/lockset_domain.hpp` | + `lockset_add_signaled_cond/lockset_add_awaited_cond/lockset_signaled_conds/lockset_awaited_conds`（**（已重构）初稿写「仿现有 `lockset_add_spawned_thread` 转发链」，但该转发链已随 `f5c0fe6` 删除——现直接加到 `LocksetDomain` 本体，numerical 侧经 `inv.second()` 访问，无需 5 层 forwarder**） | — |
| `analyzer/src/checker/data_race.cpp` | **不改**（HB 短路不新增 cond 边） | — |

### 3.2 核心代码片段

**（a）`concurrent_global_env.hpp` 新增方法：**

```cpp
void cond_wait_enter(std::uint64_t cond, const std::string& tid) {
  std::lock_guard< std::mutex > lock(_mutex);
  _cond_waiters[cond].insert(tid);
}
void cond_signal(std::uint64_t cond, bool broadcast) {
  std::lock_guard< std::mutex > lock(_mutex);
  _cond_signal_epoch[cond] += 1;
  auto it = _cond_waiters.find(cond);
  if (it == _cond_waiters.end()) return;
  if (broadcast) { it->second.clear(); }
  else if (!it->second.empty()) { it->second.erase(it->second.begin()); }  // 唤醒 1 个
}

/// 阻塞期间干扰物化：join 本 mutex 分区的干扰到局部 + 忘记共享内存。
/// 替代粗粒度 mem_forget_all —— 保留线程局部精度，仍 sound（只多忘，不少忘）。
void forget_shared_memory(value::AbstractDomain& inv, bool also_publish) {
  // 1) 把「其他线程已发布到 _lock_partition / _global_board 的值」join 进局部
  //    （复用 read_global_with_lockset 的 (b) 路径，锁定不变）
  // 2) 对共享内存（全局 + 从全局可达的堆）做 mem_forget_reachable，局部栈/寄存器不动
  // （实现依赖 memory 域的 forget 接口；此处给语义，不展开模板细节）
}
```

**（b）`lockset_domain.hpp` 新增 MAY digest（仿 `_spawned_threads`）：**

```cpp
std::unordered_set< std::uint64_t > _signaled_conds;
std::unordered_set< std::uint64_t > _awaited_conds;
bool _signaled_top = false, _awaited_top = false;

void add_signaled_cond(std::uint64_t c) { _signaled_top = false; _signaled_conds.insert(c); }
void add_awaited_cond(std::uint64_t c)  { _awaited_top  = false; _awaited_conds.insert(c); }

// join_with 中：
join_spawned(_signaled_top, _signaled_conds, other._signaled_top, other._signaled_conds);
join_spawned(_awaited_top,  _awaited_conds,  other._awaited_top,  other._awaited_conds);
// meet_with / set_to_bottom / set_to_top / leq / equals / is_top 同步镜像
```

**（c）`numerical.hpp` 重写两个执行体（骨架）：**

```cpp
void exec_pthread_cond_wait(ar::CallBase* call) {
  std::uint64_t mutex_key = /* 解析 arg1 单点 points-to + 常量偏移 */ 0;
  std::uint64_t cond_key  = /* 解析 arg0 */ 0;
  bool held = mutex_key && this->_inv.normal().lockset_holds_lock(mutex_key);
  if (held) this->_inv.normal().lockset_remove_lock(mutex_key);

  auto& env = ConcurrentGlobalEnv::get_instance();
  if (cond_key) env.cond_wait_enter(cond_key, current_thread_id(call));
  env.forget_shared_memory(this->_inv, true);     // 精化 mem_forget_all
  if (held) this->_inv.normal().lockset_add_lock(mutex_key);  // 重获
  if (cond_key) this->_inv.normal().lockset_add_awaited_cond(cond_key);
  scalar_assign_nondet(ret);
}
void exec_pthread_cond_signal(ar::CallBase* call) {
  std::uint64_t cond_key = /* 解析 arg0 */ 0;
  auto& env = ConcurrentGlobalEnv::get_instance();
  if (cond_key) env.cond_signal(cond_key, /*broadcast=*/ call->called_name()=="pthread_cond_broadcast");
  if (cond_key) this->_inv.normal().lockset_add_signaled_cond(cond_key);
  scalar_assign_nondet(ret);
}
```

---

## 4. 风险与局限性

### 4.1 可能引入的问题

| 风险 | 说明 | 缓解 |
|---|---|---|
| **伪唤醒导致 signal→wait 是 MAY** | 本方案**不**用它短路竞争，因此无 FN；代价是 signal-only 排序仍报 FP | 明确接受（soundness 优先） |
| **signal 唤醒「一个」的不确定性** | `cond_signal` 只从等待集合任意移一个，若分析模型后续用该集合做值传播，可能错配唤醒者 | 当前仅用于纪元计数，不做唤醒者绑定 |
| **`forget_shared_memory` 的实现风险** | 若「共享内存可达闭包」算错（漏了某共享对象），会少忘 → **unsound** | 必须用与 `touches_shared_memory` 一致的共享判定（全局/堆/escaped local），宁可多忘 |
| **cond 地址解析失败（⊤/非单点）** | `cond_key==0` 时退化为「不登记、不标记」 | 退化为现有 stub 行为，仍 sound（少记录只影响精度） |
| **等待队列在全局 fixpoint 迭代中的增长** | 反复 replay 会重复入队 | 每次迭代边界清空 `_cond_waiters`（仿 `reset_spawn_counts`） |

### 4.2 未覆盖的语义场景

1. **signal-only 排序（无互斥锁）**：`cond-nohb` 类 FP——生产者无锁写 data、仅靠 signal 排序。**sound 建模无法消除此 FP**（见 §2.4），除非引入「无伪唤醒」假设（违反 FN=0 红线）。
2. **条件变量的谓词式协议**（`while(!pred) wait()`）：其正确性依赖 `pred` 标志本身的 race-free 传递（数据依赖 HB），这是**通用 flag-HB 特性**，不是 cond-var 专属。
3. **`pthread_cond_timedwait`**：带超时的 wait，超时返回不重获语义需单列。
4. **信号丢失 / 跨 cond 的错误用法**（POSIX UB 场景）不做建模。

---

## 5. 验证计划

| 用例 | 预期 | 验证点 |
|---|---|---|
| `cond-mutex-safe`：生产/消费都持 m 访问 data（正确纪律） | **Safe** | wait 重获 m 后，双方同锁 → 不误报 |
| `cond-mutex-race`：wait 但 data 无锁访问 | **Race** | 无公共锁 → 不漏报 |
| `cond-nohb`：data 仅靠 signal 排序（无锁） | **Race（文档化 FP）** | 确认 MAY 边不短路 → 守 FN=0 |
| `cond-broadcast`：一个 broadcast 唤醒多个 waiter，各自持 m 写不同槽 | **Safe** | broadcast 全唤醒 + 锁纪律 |
| `cond-signal-multi-waiter`：一个 signal、多个 waiter、共享 data | **Race**（保守） | signal 只唤醒一个 → 保守报 race |
| `cond-wait-relock`：wait 返回后立即读被保护的 data | **Safe** | 步骤 5 重获锁生效 |
| `cond-spurious`：无 signal 的 wait（依赖伪唤醒退出的程序） | **Race 或 Safe 按 data 锁纪律** | 伪唤醒建模不影响 soundness |

**回归**：全量 `svcomp_race_test.py`（1030 任务）+ clean-121 边界套件，断言 **TP 不降、FN=0**（本次改动的红线）。重点盯 `thread-join-counter-*`（现有 cond 可达性 FN 修复的哨兵）与 `40_barrier_vf`（手写 barrier，依赖 cond 宏）不回归。
