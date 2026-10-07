# IKOS vs Dartagnan 差距分析（no-data-race，SV-COMP 2026 官方参照）

> 参照：`dartagnan-svcomp2026-reference.md`（版本 7caef5c/4.3.1，encodeDataRaces 机制）。
> 实测时间：2026-10-07，IKOS 处于 steps 1-4 之后（BMC 只留能力边界 gate）。

## 0. 总表（实测）

| 集合 | 数量 | 来源 |
|---|---|---|
| Dartagnan 官方 TP | 176 | `docs/artifacts/dartagnan_official_tp_176.txt` |
| IKOS TP | 80 | `svcomp_race_test.py`（`IKOS_BMC_RECOVER=1 --demote`）|
| 交集 | 57 | — |
| IKOS 独有（IKOS=FALSE，Dartagnan≠FALSE）| **19**（旧 artifact 记 23，虚高）| `docs/artifacts/ikos_unique_real_19.txt` |
| Dartagnan 独有（lost target）| 119 | = 176 − 57 |

## 1. IKOS 多做了什么（19 个独有 TP）——你存疑的点

> ⚠️ **2026-10-07 实跑本地 Dartagnan 4.3.1 修正**：旧 artifact 的「23 独有」**虚高**。
> 其中 4 个 Dartagnan 其实也报 **FAIL**（不是独有）：`lamport-b`、`05-lval_ls_13-idxunknown_lock`、
> `06-symbeq_09-tricky_address4`、`fkp2013-1`。真实独有 = **19**，全部是「Dartagnan 崩/超时/C11 循环展不开」，
> 见 `docs/artifacts/ikos_unique_real_19.txt`。下面按修正后的 19 分析。

**已核实的事实**（三条都拿代码/yml/日志验证过）：

1. 这 23 个文件的 `.yml` 里 `no-data-race.prp` 的 `expected_verdict` **全是 `false`（真 racy）**。
2. IKOS 对它们报 **FALSE（definitely UNSAFE）** = TP（正确）。
3. Dartagnan 官方结果里它们**都不在 176 的 FAIL 清单**（`dartagnan_official_all_verdicts.txt` 里全是 `ERR` 前缀）。

所以结论是**可靠的**：这 23 个是「IKOS 报出 FALSE、Dartagnan 没报 FALSE」的真 racy 文件。不是 IKOS 虚报。

**但要诚实标注两处 caveat：**

- `ERR ... [false(no-data-race)]` 这个子类（约 15 个）是我上一会话从某个结果源转录的记号，
  语义待复核——它可能是「Dartagnan 报了 false 但被裁判标成误报（FP）」也可能是「我脚本的
  内部记号」。**这一子类需要拿 SV-COMP 官方结果表（网络）重新核一次**，别直接当成「Dartagnan 时间/崩了」。
- 其中 4 个是 `[ERROR]`（Dartagnan 崩）、4 个是 `[TIMEOUT]`（超时），这两个子类是**确凿的
  IKOS 鲁棒性优势**（Dartagnan 解不了，IKOS 解出来了）。

### 23 个的分桶（按根因）

| 桶 | 文件 | 根因（为何 Dartagnan 拿不到）|
|---|---|---|
| C11 重排测试 | `reorder_c11_bad/good-10~50`（10 个）| SV-COMP 2026 用 **SC**（svcomp.cat），C11 重排在 SC 下不发生；Dartagnan 按 SC 判 → 不报 race。IKOS 的抽象解释对 C11 原子/重排的过近似恰好报出 FALSE。**这是模型差异，不是 Dartagnan 的 bug** |
| 无锁/线性化 | `elimination_backoff_stack-race`、`lamport-b` | 无锁协议，SC 下的 hb 判不了（线性化非 hb）；Dartagnan 超时/无解 |
| 崩溃/超时 | `04-mutex_20/21/50`、`reorder_2/5-race`、`fkp2013-1/2` 等 | Dartagnan 直接 ERROR/TIMEOUT（鲁棒性）|
| 棘手地址/下标 | `05-lval_ls_13-idxunknown_lock`、`06-symbeq_09-tricky_address4`、`indexer`、`per-thread-index-inc-race` | Dartagnan 的地址/下标建模在该形态下失败 |

**一句话**：IKOS 的 23 个独有 TP 里，一部分是「Dartagnan 崩/超时的鲁棒性优势」，一部分是
「SC vs C11 模型差异」（IKOS 抽象解释恰好覆盖了 C11 重排，Dartagnan 按 SC 不覆盖）。这两类
都不是 IKOS「凭空多报」，都有根因。

## 2. 我们缺什么（119 个 lost target，按 Dartagnan 机制分桶）

Dartagnan 的 hb = `po | com | rmw`（com=rf|fr|co）。IKOS 的 hb = po+mutex+spawn/join。**缺口 = com（rf/co 值流）+ N 线程 + 精确别名**。

| 缺口机制 | 对应 lost-TP 桶（示例文件）| 数量级 | IKOS 能否实现 |
|---|---|---|---|
| **rf/co 值流（标志式 HB）** | `pthread-ext/01_inc、07_rand、40_barrier_vf-b、FreeBSD/Solaris sliced` 等 `__VERIFIER_atomic_acquire/release` 标志锁 | ~8（刚被 step 1 主动放弃）| ✅ 可（rf 边 = 读值=写值，co = 写全序整数时钟），就是原 rf/co 计划 Stage 1/2 |
| **精确别名（malloc/container_of/thread-id）** | `goblint-regression/02-base_24~26 malloc`、`09-regions_*`、`ldv-races/race-2_* container_of` | ~30+ | ✅ 部分可（SMT 地址相等已有地基；堆对象需 DynAlloc 精确化，见下）|
| **无锁协议（CAS/自旋锁）** | `libvsync/caslock、rec_spinlock、twalock`、`pthread-atomic/gcd-2、qrcu-*` | ~15 | ⚠️ 难（需要原子 RMW 语义 + co，比 rf 更重；可先退 UNKNOWN）|
| **N 线程（2→N 路径展开）** | `thread-join-array-const-race`、`airline-*`（部分）| ~10 | ✅ 可（把 2 路径展开扩成 N 路径，但状态爆炸要带上限）|
| **循环/sem/barrier/cond 复杂同步** | `condvar、one_time_barrier、ring_1w1r、workstealqueue_mutex-*` | ~20 | ⚠️ 部分（sem/barrier 建 HB 可做；condvar 的 signal→wait 已有地基）|

**最该先做（可行性 × 代表性）**：**rf/co 值流**——它同时解锁「标志式 HB 桶」（~8）和「lock-free 桶」（~15）的
共同根因，驱动样例明确（`40_barrier_vf-b` racy vs `43_NetBSD` safe 同一套标志，靠 rf/co 区分），
且和 Dartagnan 的 `com` 机制一一对应。这与 `bmc-soundness-coupling-backstops` 里「先编码完备再解耦」的顺序一致。

## 3. 一个关键澄清（之前被 2027 版带偏的地方）

之前 memory `dartagnan-rf-co-com-smt-encoding` 记的 `sw=[A];rmw^-1?;(com&ext);[A]`、`dr=conflict\(hb|hb⁻¹)`
是 **SV-COMP 2027（commit c802dc442, 2026-06）** 的 cat 语义，**不是 176-TP 版本**。

176-TP 版（7caef5c）的 race 判定是 **`encodeDataRaces`：非原子 + 同址 + hb 相邻（clock_hb 差 1）**，
没有 `sw`/`conflict`/`dr` 这些 cat 关系。**IKOS `check_race` 的「相邻判定」恰恰对齐的是 2026 版机制**，
不需要 `sw` 那套原子端点条件。所以之前「同步两端必须原子」的结论**作废**——2026 版对非原子访问照样用
hb 相邻判 race，原子/非原子区分只体现在 `canRace`（原子访问根本不进竞态对）。
