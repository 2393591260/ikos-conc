# Dartagnan SV-COMP 2026 权威参照（版本 + no-data-race 原理）

> 本文固定历史版本，便于复核 2026 实现。源码机制、IKOS 对照和实验分数须分别核实；
> 不能将旧版机制或未配对的分数直接用于 2027。2026-10-09 修正下文“相邻”和“等价”误述。
> 本轮 IKOS 已加入受限 SC 值流及事实接口，验证见 [精度恢复报告](ikos-bmc-precision-2026-10-09.md)。

## 0. 版本（⚠️ 关键，之前踩过坑）

| 版本 | commit | 日期 | 身份 |
|---|---|---|---|
| **固定 2026 源码参照** | `7caef5c850dc2dbf81c5554dcf41e402007f7a19`（`Fix free intrinsic`），标签 `4.3.1` 附近 | 2025-11-26 | 本文机制参照；版本不自动证明任何分数 |
| 此前记录的 2027 开发 HEAD | `b81ec6c55503e824bb0ddc4ebac6c6f0a63d56d8`（`Reorganize unit tests`） | 2026-09-24 | 历史记录，不代表当前本地 HEAD |
| 本地实际运行的 launcher + library JAR | 两个 JAR 内嵌 `git.properties` 均为 `7caef5c…`，4.3.1 | 2026 版 | 2026-10-09 重新核对；指纹见下述材料 |

此前以 `svcomp/target/classes/git.properties` 的 4.1.0 / `5789d532…` 判断实际运行版本有误。
该文件是遗留构建产物；`java -jar svcomp/target/svcomp.jar` 使用的是 launcher
及其 manifest 指定的 library JAR。本轮核对两个 JAR 的内嵌版本及 SHA256，
并重新运行一个差异任务，依据见
[参考迁移材料](artifacts/bmc-reference-guided-2026-10-09/manifest.json)。
旧 README 或目录名仍不能作为实际运行版本的证据。

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
- `com & int`：线程**内**通信；`com & ext`：**跨线程**通信。通信关系不是一般的寄存器数据依赖。
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

**该版本完整执行模型中的竞争查询**：
1. 两个访问来自**不同线程**；
2. 至少一方是 **Write**（w 必须是 WRITE，m 是任意 memory 访问）；
3. 双方都 **非原子**（`mo == null || NONATOMIC`）——原子/RMW 访问直接不参与竞态；
4. **同址**（`alias.mayAlias` 过近似 + `sameAddress` SMT 相等精确化）；
5. 双方都**执行**（`execution`）；
6. 在 **hb 线性化里相邻**（`clock_hb(w) == clock_hb(m) + 1`）。

这里还有显式 `hb(m,w)` 条件。“相邻”不等于“没有 hb 排序”，也不是脱离完整执行模型的竞争定理。
IKOS 的相邻查询缺少同等的执行、值流和实例约束，不能单凭查询形式证明等价或 sound。

## 3. 与 IKOS BMC 的对照（原理级）

| Dartagnan 机制 | IKOS BMC 现状 | 缺口 |
|---|---|---|
| §1 的 CAT 精确 hb 表达式，`com=rf\|fr\|co` | po+同步、按需有界 SC 读来源、完整 verifier 原子段 | 未实现完整 CAT/一般 K 线程 rf/co；load 仍不能自由化 |
| 精确全程序 SMT，N 线程全展开 | 2 线程路径展开（anchor 或全对） | ❌ 缺 N 线程 |
| `alias.mayAlias` + `sameAddress` SMT | `same_global`(singleton pts) + `offset` + SMT 同址门 | 部分（堆对象未放开） |
| `canRace` = 非原子（原子直接不参与） | `has_unmodeled_sync` 挡 UNKNOWN | 支持边界不同，未证明等价 |
| LoopUnrolling + bound，保留实际条件及边界终止 | 当前保留真实条件、截断回边；已删除猜测循环界 | 缺动态迭代及一般展开，未证明等价 |
| may-alias 预筛与 SMT 地址约束 | singleton points-to、字节宽度/范围及受支持符号下标 | 地址抽象不同，未证明等价 |
| 完整执行模型上的 `clock_hb` | 支持前缀上的整数 `clock` | 同类变量不证明关系或查询等价 |

**当前差距**：Dartagnan 有完整程序执行/关系模型；IKOS 已把所需整数 load 绑定到初值、实际 main 创建前缀或
两条选中路径的最近先行写，但仍受地址、循环、对象及线程数边界限制。写值与读值须兼容同一个执行顺序；
不能只看到 `assume(MTX==0);MTX=1` 就推导互斥，这个检查与写入本身未必原子。

IKOS 还用 `CandidateFacts` 接收 checker 的 may-offset、实际创建前缀和受限局部 no-wrap 证书；
可参考 Dartagnan 的关系分析与外部 filters 接口进一步压缩候选，而非照搬完整程序编码。

## 4. 历史交集记录（不作为当前配对结论）

下表保留此前 2026-10-07 的记录，本轮没有重跑并核实其 Dartagnan 二进制、任务清单和资源配置。
IKOS 的 80 TP 是旧实现，其中存在已复现的缺陷；当前 35 TP 与该表不能直接相减估计收益。

| 集合 | 数量 |
|---|---|
| Dartagnan 官方 TP | 176 |
| IKOS TP（steps 1-4 后） | 80 |
| 交集 | 57 |
| IKOS 独有（IKOS=FALSE，Dartagnan≠FALSE） | 23 |
| Dartagnan 独有（lost target） | 119 |

- 23 个 IKOS 独有清单：此前指向 `docs/artifacts/ikos_unique_23.txt`，本轮未重新验证其有效性。
- 119 个 lost target 清单：`docs/artifacts/dartagnan_real_target_110.txt` 是旧版（IKOS 更高 TP 时算的），
  当前以本表 119 为准。

## 5. 关键 commits（5789d53→7caef5c 的 151 个里，机制类）→ IKOS 缺口映射

此前 `git log 5789d53..7caef5c` 记录了以下机制线索。提交差异不能独立证明分数增量，
也不能从标题断言 IKOS 完全缺少对应机制；引入时须重新读所需源码。

| commit | 机制 | 对应 IKOS 缺口 |
|---|---|---|
| `664d81f7f` `0e1e73128` `ab0610069` (#841/#847/#905) | pthread_join 新模型 + IR 线程 + detach | join-in-loop/数组桶（IKOS join HB 只做了直连，循环里 join 未展开）|
| `55590d7c7` (#892) | **跨线程 SCCP + thread-local 分配** | thread-id 桶（IKOS 抽象解释的 thread-id 坍缩）|
| `95224e40c` (#820) | **rf 边上的数据类型自动转换** | **rf/co 值流桶（IKOS 的核心缺口）**|
| `ee5a58ae9` (#904) | MemToReg + 指针分析修复 | 别名精度桶（malloc/container_of）|
| `24a65b237` (#944) | 带地址的 devirtualization | 函数指针；IKOS FPA may 集不能直接证明实际被调用者，不宣称对齐 |
| `e45ac070e` (#840) | 混合字长访问 | 精度（次要）|
| `e8622b751` `d8e6066d6` `7950dd782` `81da44f69` (#786/#877/#797/#827) | spinloop 检测 / 控制屏障 / 循环展开 + bound | 历史线索；IKOS 当前截断回边，不能称基本对齐 |
| `5fc2bcb78` | no-data-race witness 生成 | 当前 adapter 为 YAML 2.2，独立验证未完成；2026 race 格式契约及 2027 支持须核实 |

优先研究线索包括 rf 边类型转换、跨线程分析/thread-local、指针分析修复；具体优先级要结合
当前诊断和实现前提确定，不能用历史提交数量或任务桶直接估计可恢复 TP。
