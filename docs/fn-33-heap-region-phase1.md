# FN-④（09-regions/33）堆链表 region —— 阶段 1 定位结论

> 目的：定位 33 的 FN 断点是「恢复端 (a)」还是「配对端 (b)」，为阶段 2 的最小修复提供依据。
> 方法：轻量 env-gated 探针（`IKOS_HEAP_DEBUG`），仅在 `on_load_pointer_restore` 的 heap 分支打
> `[heap-hit]`（恢复成功）/`[heap-miss]`（黑板无此键）两行。已核对探针**不改变判定**（6 个 09-regions
> 哨兵 base/probe 全一致，33 也保持 Safe）。

## 结论：既不是 (a) 恢复端、也不是 (b) 配对端，而是「黑板数据欠收敛」

探针输出（33，收敛后）：

```
2 [heap-hit] src_n=2 key=1200000008 tgt_n=1   # D2->next (alloc=18) 早期
2 [heap-hit] src_n=2 key=1200000008 tgt_n=2
4 [heap-hit] src_n=3 key=1200000008 tgt_n=3   # D2->next 收敛到 n=3 ✓
4 [heap-hit] src_n=3 key=1500000008 tgt_n=2   # D1->next (alloc=21) 停在 n=2 ✗
4 [heap-miss] src_n=2 key=1000000008          # D0->next = NULL（没记录，正确）
4 [heap-miss] src_n=3 key=1000000008
```

关键：**`D1->next` 的黑板值停在 n=2（缺 D1），而不是 n=3**。而 `D2->next` 正确收敛到 n=3。

于是遍历 `p = A = {D0,D1,D2}`，`p->next` 恢复成：
- D2->next → {D0,D1,D2}（n=3）
- D1->next → {D0,D2}（n=2，**缺 D1 自己**）
- D0->next → 无（NULL）

`p = p->next` 取并集后 `p` 收敛到 {D0,D2}（n=2，丢了 D1），`printf(p->datum)` 读 n=2 → 配不上
t_fun 的 `D1->datum++`（n=1）→ **FN**。

## 根因假设（待阶段 2 验证）

`insert(D1, &A)` 里两条语句：
1. `D1->next = *list`（= A 的当前值）→ `join_heap_pointer(D1->next, A)`
2. `*list = p`（A = D1）→ `join_global_pointer(A, D1)`

**顺序决定**：语句 1 记录 `D1->next = A` 时，A 还是「语句 2 之前」的值（{D0,D2}），所以 `D1->next`
**缺 D1 自己**。理论上 fixpoint 下一轮 A 的 entry 会含 D1（A=D1 已 flush），`D1->next` 应累积到
n=3；但实测停在 n=2 —— 说明 `join_heap_pointer(D1->next, A)` 在收敛轮**没有以 n=3 再次发生**。

**下一步验证**：在 `join_heap_pointer` 加探针，看 `D1->next` 的 join 在各轮的点集大小轨迹（是
「join 只发生一轮」还是「join 的 A 值一直 n=2」）。这决定阶段 2 是：
- (i) 修 `on_store_privatize` 让 `D->next = A` 也把「A=D 自身」的写回环进黑板（自包含 region）；或
- (ii) 把 heap 黑板 key 从 `(alloc-site, field)` 升级为「alloc-site region」（链表节点共享 region），
  对标 goblint `region` 的 `may_race = region 相交`。

## 阶段 2 边界（防 FP）

- 09-regions 全组（36 个）串行 A/B，尤其 `02-list_nr`/`03-list2_rc`/`23-evilcollapse_rc` 不得翻转。
- region 化若做，合并策略只在不失 sound 的前提下收窄，防 FP 暴涨。

---

## 阶段 2 结论（纠正阶段 1 的根因假设）

阶段 1 假设「`join_heap_pointer(D1->next, A)` 在收敛轮没有以 n=3 再次发生」。阶段 2 在
`join_heap_pointer`/`join_global_pointer` 加 `[join-heap]`/`[join-glob]` 轨迹探针，实测**推翻**该假设：

```
[join-heap] alloc=21 off=8 join_n=2 acc_n=2   # D1->next 第一轮 A={D0,D2}
[join-heap] alloc=21 off=8 join_n=3 acc_n=3   # 后续轮 A={D0,D1,D2}，D1->next 累积到 n=3 ✓
```

`D1->next` 的 join **确实以 n=3 再发生**，且黑板点集**正确累积到 n=3**。问题不在「join 没发生」，
而在**全局不动点提前收敛**：`join_global_pointer`/`join_heap_pointer` 只改黑板内容、**不置 `_is_dirty`**，
于是外层 thread-modular fixpoint 在看到黑板「新 entry / 点集扩张」时**不重跑**。恢复端
`on_load_pointer_restore` 在收敛轮读到的仍是上一轮的 `D1->next={D0,D2}`（n=2），遍历丢 D1 → 配对不上
t_fun 的 `D1->datum++` → FN。

### 修复（通用能力，非文件补丁）

`concurrent_global_env.hpp` 的两个 pointer-board join 在「新 entry」和「点集扩张」时置 `_is_dirty = true`，
让 fixpoint 多迭代到黑板收敛，恢复端读到 n=3：

```cpp
if (r.second) {
  _is_dirty = true; // new entry: a later restore must see it
} else if (!it->second.pts.is_top()) {
  std::size_t old_n = it->second.pts.is_set() ? it->second.pts.size() : 0;
  it->second.pts.join_with(pts);
  it->second.offset.join_with(offset);
  if (it->second.pts.is_set() && it->second.pts.size() > old_n) {
    _is_dirty = true; // expanded: keep iterating so a restore re-reads it
  }
}
```

### 验证

- 33 串行 3 次全 RACE（修复前 Safe/FN）。
- 全量 222 文件 A/B：**FN=0**（TP=131 TN=64 FP=25 ERR=2），FP 全为既有（24 个 pre-existing +
  `27-alloc_buffer` 为 FN-③ 已接受的 1 个），`_is_dirty` 修复**零新增 FP**。
- 09-regions 哨兵组（01/03/23 RACE，02/04/11/13/21/24/32 Safe）无翻转。
- ERR=2 均为前端编译失败（`62-simple_atomic_nr` 用 `_Atomic(struct s)`、`61-invariant_racing` include
  `goblint.h`），与 race 分析无关，非 FN。
