# 原生 FN 病理分布报告：基于抽象解释 Soundness 的根因归纳

> 目标：对 SV-COMP 全集剩余的 **50 个 FN**，自底向上按三大抽象解释维度归类死因，锁定一个「修正底层抽象逻辑即可一劳永逸」的高杠杆靶点，并给出最小样例的根因解剖。
> 日期：2026-09-21。对应代码 `master`（已含 C11 `_Atomic` + MAY digest）。

---

## 1. 病理分布（三大维度 × 大致文件数）

先给出结论表，再逐维度展开。

| 维度 | 死因 | 文件数 | 代表 |
|---|---|---|---|
| **A. Lattice Operation Breach（抽象域操作破裂 / 非正常坍缩为 ⊥）** | 读侧覆盖 `int_set` 是**不健全的窄化**，把 sound 的上界 [0,12] 换成陈旧下界 [0,2] → 循环出口非正常坍缩为 ⊥ → 竞争访问变死代码 | **15** | fib ×12、CAS 自旋锁 ×3 |
| **B. Fallback Failure（未知语义未安全退却为 ⊤）** | 未建模的 int spawn 实参 / C11 内存序，没有退化为 ⊤（保守共享），而是静默 No-op 或使下标变 ⊥ | **27** | per-thread-array/thread-join ×17、reorder_c11 ×10 |
| **C. Over-confident Synchronization（假锁 / 假 HB 边）** | 在不满足 Must-Alias / Strict-HB 时，仍利用模糊别名/线程身份建立「假同步」放过真竞争 | **8** | 堆区域消歧 ×3、无锁栈 ×3、driver ×1、弱内存 ×1 |

**合计 50**。

### 1.1 维度 A：Lattice Operation Breach（15 个，**最高杠杆**）

- **fib ×12**（`fib_safe/unsafe-5/6/7/10/11/12-racy.i`）：见 §3 根因解剖。这是**唯一一个「单个独立死因 + 修正底层抽象逻辑即可全清」的靶点**。
- **CAS 自旋锁 ×3**（`05_tas.i`、`04_incdec_cas-race.i`、`10_fmaxsym_cas.i`）：`while(cond==locked) TAS(...)` 自旋在抽象域里 cond 恒为 1 → 自旋**不收敛** → `c++`（竞争访问）非正常坍缩为 ⊥ → `Total checks = 0`。属于「widening 未能正确抬高自旋出口」的终止性问题，与 fib 同属「出口边非正常 ⊥」。

### 1.2 维度 B：Fallback Failure（27 个）

- **per-thread-array / thread-join ×17**：`pthread_create(&t,0,fn,(void*)i)` 的第 4 实参是 **int 转指针**（不是指向内存的指针），`record_spawn_arg` 拿到空 points-to → 子线程形参 `arg` 停在 ⊤ → `i=(int)arg` = ⊤ → `datas[⊤]` 的下标变 ⊤/⊥ → 竞争访问**未被记录**（`Total checks = 0`）。**正确的安全退却**应是把 int 实参的**值**传播进子线程（或把数组访问保守判为共享），而不是静默丢失。
- **reorder_c11 ×10**（bad/good 各 5）：C11 relaxed 内存序允许 load/store 重排从而构成竞争；引擎**没有 C11 内存模型**，把原子同步当普通操作，既无 acquire/release HB、也无「可能重排 → 竞争」的保守退却 → 漏报。

### 1.3 维度 C：Over-confident Synchronization（8 个）

`04-mutex_44-malloc_sound`（循环 malloc 互斥锁 p≠q 被当同锁）、`09-regions_05/16`（堆链表跨路径别名折叠）、`libvsync/bounded_spsc`、`elimination_backoff_stack`、`safestack_relacy`（无锁结构）、`char_generic_nvram_*`（驱动）、`weaver/popl20-more-parray-copy`（弱内存屏障）。这些都需要更细的堆别名 / 线性化 / 弱内存语义，**不是单一底层 bug**。

---

## 2. 锁定高杠杆靶点：fib 的「读侧覆盖 = 不健全窄化」

**结论**：`fib ×12` 是唯一满足「违背理论底线、且修正底层抽象逻辑即可一劳永逸」的独立死因。它属于**维度 A（Lattice Breach）**，具体是 **unsound narrowing（不健全的窄化）**。

### 2.1 数学表述（为什么它是 lattice breach）

抽象解释的 soundness 要求**转移函数单调**：对共享变量 `x` 的每一次读，抽象值 `d` 必须**上闭**具体值集合 `{x}`：

```
γ(d) ⊇ { 所有交错执行中 x 的取值 }
```

循环内局部 invariant 经 widening 后 `x ∈ [0, 12]`（sound 的上界）。但读侧把 `x` 用全局黑板里的**陈旧 peek 值**覆盖：

```
x := injected  = [0, 2]    ← 这是「赋值/窄化」，不是「join/上闭」
```

`[0,2] ⊂ [0,12]`，即 `γ([0,2]) ⊉ {x 的实际取值}`。**转移函数收缩了状态空间，违反单调性** → 不健全。

### 2.2 为什么会坍缩成 ⊥（bottom）

循环出口判定 `x < 12` 现在在**窄化后的 [0,2]** 上求值 → 被判「恒真」→ 出口边后置态 = **⊥**。⊥ 沿控制流传染：`return prev`（fib 出口）→ main 里的 `i <= correct && j <= correct`（**竞争访问**）全部 unreachable → `check()` 见 `inv.is_normal_flow_bottom()` 直接 return → **竞争访问根本没记录** → FN。

> 即：错误的窄化 → 错误地**证明**了「x 恒 < 12」→ 把真实会执行到的竞争访问当死代码藏掉。

---

## 3. 根因解剖（最小样例 fib_safe-5-racy.i）

### 3.1 源码结构（关键三处）

```c
int x, cur = 1, prev = 0, next = 0;      // fib 的全局循环变量
int i, j;                                 // 竞争变量

int fib() {
  for (x = 0; x < 12; x++) {              // ① 全局计数器循环
    next = prev + cur; prev = cur; cur = next;
  }
  return prev;                            // ② 出口（坍缩为 ⊥）
}

void *t1(){ for(p=0;p<5;p++){ __VERIFIER_atomic_begin(); i=i+j; __VERIFIER_atomic_end(); } }
void *t2(){ for(q=0;q<5;q++){ __VERIFIER_atomic_begin(); j=j+i; __VERIFIER_atomic_end(); } }

int main(){
  ... pthread_create(t1); pthread_create(t2);
  int correct = fib();
  __VERIFIER_atomic_begin();
  _Bool c = (i <= correct && j <= correct);  // ③ 竞争访问（读 i/j）
  __VERIFIER_atomic_end();
}
```

### 3.2 唯一的源码决断点

**[numerical.hpp:2100-2102](analyzer/include/ikos/analyzer/analysis/execution_engine/numerical.hpp#L2100-L2102)**（`exec(Load)` 读侧）——**（注：此段为历史快照，该注入逻辑现已移入 `execution_engine/concurrent_semantics.hpp` 的 `on_load_shared_uninit`/`on_load_pointer_restore`，且 `did_inject` 变量已随 PBR 改造移除；commit `56f20a7`）**：

```cpp
if (did_inject && lhs.var()->type()->is_integer() && ...) {
  this->_inv.normal().int_set(lhs.var(), injected_interval);   // ← 不健全窄化
}
```

- 前面 `mem_read(lhs, ptr, size)` 已经把**本地 invariant 里 widen 好的值** `x ∈ [0,12]` 读进 `lhs`。
- `read_global_with_lockset` 返回的是全局黑板里**陈旧、未 flush 的延迟写 peek** `[0,2]`（循环自增一次得到的值）。
- `int_set(lhs, [0,2])` 用陈旧值**覆盖**本地值，丢弃了 [3..12]。

**插桩铁证**（早前取证）：

```
local=[0, 12]           injected=[0, 2]   ← 本地已 widen 到 [0,12]，注入值是陈旧 peek
local=[0, 2147483647]   injected=[0, 2]   ← 本地 widen 到 [0,INT_MAX]，注入仍停 [0,2]
```

### 3.3 为什么不是「语法补丁」能修的（理论红线）

直接删掉覆盖会丢跨线程干扰（t1 看不到 t2 写的 j）→ 引入另一类 FN；改成 `local ⊔ injected`（join）会让 widen 出的 ⊤ 经 store 的 `record_privatized_write` 漏进全局黑板 → 全集 +80 FP +2 FN（早前 1a 实验已证伪）。

**唯一 sound 的底层修复是 thread-local semantics（Mukherjee et al. SAS'17）**：

1. **函数入口物化**：把 `_global_board` 当前值 join 进本地全局 cell（跨线程干扰物化一次，替代逐读覆盖）；
2. **去掉读侧覆盖**：读全局用本地 cell（自己的写 + 入口物化的干扰）→ 循环计数器 x 正常 widen → 出口可达；
3. **store 改边界发布**：过程内不再逐 store 发布，改函数出口/迭代边界一次性发布（避免循环内 ⊤ 泄漏）。

这三点都是**抽象域/通信协议层面的修正**，全程只有 join（⊔，单调）、无 narrowing，属于「修正底层抽象逻辑」，而非对 fib 语法的特判。

---

## 4. 交付结论

- **高杠杆靶点**：fib ×12（维度 A，unsound narrowing）。**一次修复清零 12/50 = 24% 的 FN，且是架构级 sound 修复（thread-local semantics），不是语法补丁。**
- 次高杠杆：per-thread-array ×17（维度 B，int spawn 实参的值传播），需「spawn 实参按值传播」这一底层特性。
- 其余 21 个（reorder_c11 ×10 + 堆/无锁 ×8 + CAS 自旋 ×3）分属 C11 内存模型 / 堆消歧 / 自旋终止，各需独立立项。

**建议**：先立项 fib 的 thread-local semantics 修复（§3.3 三步，sound、一劳永逸），它同时是维度 A 的范式修正——其余维度 A 的 CAS 自旋 ⊥ 也可能因同一「读侧覆盖」机制受益。
