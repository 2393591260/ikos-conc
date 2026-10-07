# Dartagnan SV-COMP 2026 权威参照（版本 + no-data-race 原理）

> **本文档是 IKOS no-data-race 工作的唯一 Dartagnan 参照，务必以 SV-COMP 2026 官方版为准，
> 不要被本地 HEAD（SV-COMP 2027 开发版）带偏。**

## 0. 版本（⚠️ 关键，之前踩过坑）

| 版本 | commit | 日期 | 身份 |
|---|---|---|---|
| **SV-COMP 2026 官方（176 TP）** | `7caef5c850dc2dbf81c5554dcf41e402007f7a19`（`Fix free intrinsic`），标签 `4.3.1` 附近 | 2025-11-26 | ✅ **权威，本文档的对象** |
| 本地 HEAD（SV-COMP 2027 开发） | `b81ec6c55503e824bb0ddc4ebac6c6f0a63d56d8`（`Reorganize unit tests`） | 2026-09-24 | ❌ 勿当参照 |

**证据**：`git log --oneline 7caef5c..HEAD -- cat/svcomp.cat` 只有一个 commit
`c802dc442 "SVCOMP races in cat (#1048)"`（**2026-06-25**）把 `dr/conflict/sw/hb-race`
加进 svcomp.cat。也就是说：

- **SV-COMP 2026 版（7caef5c）的 svcomp.cat 只有 `com`+`hb`，没有 `dr/conflict`。**
- no-data-race 检测走**内置 `DATARACEFREEDOM` 属性 + `PropertyEncoder.encodeDataRaces()`**（硬编码的
  hb-相邻判定），不是 `.cat` 里的 `flag ~empty dr as data-race`（那是 2027 才加的）。

之前一份 memory（`dartagnan-rf-co-com-smt-encoding`）记的 `sw/conflict/dr` 是 2027 版语义，**作废重写**。

## 1. svcomp.cat（SV-COMP 2026 完整，共 6 行）

```
SVCOMP
let com = (rf | fr | co)
let hb = po | (com & int) | (rmw^-1? ; (com & ext))
acyclic hb as sc
```

- `com = rf | fr | co`：rf = read-from（读从某写取值）、fr = from-read、co = coherence（同址写全序）。
- `com & int`：线程**内**通信（数据依赖）；`com & ext`：**跨线程**通信。
- `rmw^-1? ; (com & ext)`：RMW 相关的跨线程通信（`rmw^-1?` 是可选的 rmw 反边前缀）。
- `acyclic hb as sc`：hb 无环 = 顺序一致性。

## 2. no-data-race 检测（`PropertyEncoder.encodeDataRaces`，读源码）

`Property.java:17`：`DATARACEFREEDOM, // Special option for data-race detection in SVCOMP only`

`encodeDataRaces()` 的核心（逐行等价）：

```java
Predicate<MemoryEvent> canRace = m -> {
    MemoryOrder mo = m.getMetadata(MemoryOrder.class);
    return mo == null || mo.value().equals(Tag.C11.NONATOMIC);   // 只认非原子
};
for (Thread t1 != t2) {
  for (w in t1 的 WRITE，非 INIT，canRace) {
    for (m in t2 的 MEMORY(读或写)，非 INIT，canRace) {
      if ((w.RMW && m.RMW) || !alias.mayAlias(m,w)) continue;   // 双 RMW 不算，不同址不算
      isConflictingPair = execution(m,w) && sameAddress(m,w);    // 都执行 + 同址
      isAdjacentInHb    = hb(m,w) && clock_hb(w) == clock_hb(m)+1; // hb 里相邻
      hasRace |= isConflictingPair && isAdjacentInHb;
    }
  }
}
// 判定 = 是否存在 hasRace 的可满足赋值；无 → SAFE，有 → 报 race
```

**race 的充要条件（权威定义）**：
1. 两个访问来自**不同线程**；
2. 至少一方是 **Write**（w 必须是 WRITE，m 是任意 memory 访问）；
3. 双方都 **非原子**（`mo == null || NONATOMIC`）——原子/RMW 访问直接不参与竞态；
4. **同址**（`alias.mayAlias` 过近似 + `sameAddress` SMT 相等精确化）；
5. 双方都**执行**（`execution`）；
6. 在 **hb 线性化里相邻**（`clock_hb(w) == clock_hb(m) + 1`）。

关键：**「相邻 ⟺ 无 hb 排序 ⟺ 竞态」**。IKOS 的 `check_race` 正是这个「相邻」判定的两线程版。

## 3. 与 IKOS BMC 的对照（原理级）

| Dartagnan 机制 | IKOS BMC 现状 | 缺口 |
|---|---|---|
| `hb = po \| com \| rmw`，`com=rf\|fr\|co`（**值流**） | `po + mutex + spawn/join`（**无 com/rmw**） | ❌ 缺 `com`（rf/co 值流）= 标志式 HB / lock-free 桶 |
| 精确全程序 SMT，N 线程全展开 | 2 线程路径展开（anchor 或全对） | ❌ 缺 N 线程 |
| `alias.mayAlias` + `sameAddress` SMT | `same_global`(singleton pts) + `offset` + SMT 同址门 | 部分（堆对象未放开） |
| `canRace` = 非原子（原子直接不参与） | `has_unmodeled_sync` 整体挡 UNKNOWN | 等价但更粗（整程序挡，非逐访问） |
| LLVM `-loop-unroll` + bound | `constant_loop_bound`(≤8) + `incomplete` | 等价 |
| Andersen 精确地址 | FunctionPointerAnalysis（流不敏感 Andersen） | 等价 |
| 时钟变量 `clock_hb` | `clock` 整数变量 | 等价 |

**核心差距一句话**：Dartagnan 的 `hb` 含 `com`（rf/fr/co 值流），IKOS 的 `hb` 只有 po+mutex。
标志式 HB（`assume(MTX==0);MTX=1`）的互斥正是靠 `co`（写全序）+ `rf`（读从某写取值）提供的——
这是「标志式 HB 桶」和「lock-free 桶」的共同根因，也是 rf/co 值流建模要补的。

## 4. 176 TP 与 IKOS 的精确交集（2026-10-07 实测）

| 集合 | 数量 |
|---|---|
| Dartagnan 官方 TP | 176 |
| IKOS TP（steps 1-4 后） | 80 |
| 交集 | 57 |
| IKOS 独有（IKOS=FALSE，Dartagnan≠FALSE） | 23 |
| Dartagnan 独有（lost target） | 119 |

- 23 个 IKOS 独有清单：`docs/artifacts/ikos_unique_23.txt`（已验证，根因见 `ikos-vs-dartagnan-gap.md`）。
- 119 个 lost target 清单：`docs/artifacts/dartagnan_real_target_110.txt` 是旧版（IKOS 更高 TP 时算的），
  当前以本表 119 为准。

## 5. 关键 commits（5789d53→7caef5c 的 151 个里，机制类）→ IKOS 缺口映射

`git log 5789d53..7caef5c` 过滤机制关键字，能跑出 176 TP 的原理增量（即 IKOS 还没实现的）：

| commit | 机制 | 对应 IKOS 缺口 |
|---|---|---|
| `664d81f7f` `0e1e73128` `ab0610069` (#841/#847/#905) | pthread_join 新模型 + IR 线程 + detach | join-in-loop/数组桶（IKOS join HB 只做了直连，循环里 join 未展开）|
| `55590d7c7` (#892) | **跨线程 SCCP + thread-local 分配** | thread-id 桶（IKOS 抽象解释的 thread-id 坍缩）|
| `95224e40c` (#820) | **rf 边上的数据类型自动转换** | **rf/co 值流桶（IKOS 的核心缺口）**|
| `ee5a58ae9` (#904) | MemToReg + 指针分析修复 | 别名精度桶（malloc/container_of）|
| `24a65b237` (#944) | 带地址的 devirtualization | 函数指针（IKOS 已用 Andersen FPA，基本对齐）|
| `e45ac070e` (#840) | 混合字长访问 | 精度（次要）|
| `e8622b751` `d8e6066d6` `7950dd782` `81da44f69` (#786/#877/#797/#827) | spinloop 检测 / 控制屏障 / 循环展开 + bound | 循环桶（IKOS 已有 constant_loop_bound ≤8，基本对齐）|
| `5fc2bcb78` | no-data-race witness 生成 | witness（IKOS 已有 GraphML 见证，见 dartagnan-witness-graphml-validation）|

**结论**：这 151 个 commit 里对 IKOS 最有价值的 3 个 = **rf 边数据类型转换(#820)、跨线程 SCCP/thread-local(#892)、指针分析修复(#904)**，
正好对应 IKOS 的三个缺口桶：**rf/co 值流、thread-id、别名精度**。和 §3 的对照表一致。
