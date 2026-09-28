# docs/ 文档索引

> 本目录存放 IKOS **并发数据竞争检测** 子系统的文档。本索引解释每个文件/子目录的作用，
> 并给出阅读顺序。维护原则：只保留「当前工作实时相关」的文档；一次性笔记、已回退/de-scoped
> 的方案不在此列（见 git 历史 `12d90ec`）。

## 阅读顺序（新人 / 新会话入门）

0. **ikos-architecture-survey.md** —— 先看 IKOS 整体架构（原生版 vs 并发修改版的目录/模块/改动分类/风险）
1. **concurrency-architecture.md** —— 再看并发子系统代码长什么样
2. **ikos-race-theory-mapping.md** —— 再看理论依据与 soundness 前提
3. **ikos-race-fp-classification.md** —— 当前 FP 状态与修复演进
4. **fn-root-cause-analysis.md** —— 当前 FN 根因病理
5. 其余按需查阅（验证报告 / 对比报告）

---

## 零、整体架构

| 文件 | 作用 |
|---|---|
| [ikos-architecture-survey.md](ikos-architecture-survey.md) | **IKOS 整体架构调研**：原生版逐目录讲解（ar/core/frontend/analyzer 各自在流水线哪一步）、原生扩展范式、并发修改版改动清单（A~G 分类 + 是否侵入内核）、整体风险清单。**入门/评估改动的首选。** |

## 一、理论与根因分析

| 文件 | 作用 |
|---|---|
| [ikos-race-theory-mapping.md](ikos-race-theory-mapping.md) | **理论依据与语义建模总纲**。讲清两件事：IKOS 原生（非并发）已有哪些抽象解释理论、soundness 前提是什么；并发这一块建了哪些语义（lockset/region/cond-var/happens-before…）、各自按 A/B/C 分类、实现到哪一步、靠哪些测试兜底、现在的 soundness 依据。**最常翻的「为什么这么做」文档。** |
| [fn-root-cause-analysis.md](fn-root-cause-analysis.md) | **FN（漏报）根因病理报告**。把剩余 FN 自底向上按三大抽象解释维度（值域/别名/锁集…）归类死因，锁定高杠杆靶点，并给出最小样例的根因解剖。**漏报排查从这里入手。** |
| [ikos-race-fp-classification.md](ikos-race-fp-classification.md) | **FP（误报）分类清单**。当前 FP 按根因三档：A 完全未建模原语（信号量）/ B 半建模缺 happens-before（cond-var、C11 acquire-release、无锁协议）/ C 精度前端问题（⊤ points-to、region 折叠、跨函数传播丢失…）。每条附代表文件与修复演进史（从基线 227 → 当前）。**误报排查从这里入手。** |

## 二、设计与实现

| 文件 | 作用 |
|---|---|
| [concurrency-architecture.md](concurrency-architecture.md)（+ `.tex` 源） | **并发子系统代码架构梳理**：目录结构、运行原理（thread-modular + 锁集 digest + ConcurrentGlobalEnv 黑板）、已知边界。`.tex` 是 LaTeX 源（用于出图/排版）。 |
| [ikos-condvar-modeling-design.md](ikos-condvar-modeling-design.md) | **条件变量语义建模方案**（`pthread_cond_wait/signal/broadcast`）：wait-queue + signal epoch + MAY digest 的设计与 soundness 论证。 |

## 三、验证与基准

| 文件 | 作用 |
|---|---|
| [ikos-race-verification-report.md](ikos-race-verification-report.md) | **边界验证报告**：逐用例完整复现（每条指令 + 终端输出），作为回归证据。 |
| [ikos-vs-goblint-race-comparison.md](ikos-vs-goblint-race-comparison.md)（+ `.csv`） | **IKOS vs Goblint 对比报告**：同一套 1030 个 no-data-race 任务逐个跑两工具，对比判定（Race/Safe/Unknown/…），列不一致组 + Goblint 异常组 + 根因初判。`.csv` 是原始逐文件数据。 |
| [ikos-on-goblint-tests.md](ikos-on-goblint-tests.md)（+ `.csv`） | **IKOS 跑 Goblint 自家竞态回归测试**（262 个文件，7 个组）：TP/FP/TN/FN + 逐文件 FN/FP 根因。暴露了 sv-benchmarks 测不出的边角漏报（部分 join、undef 锁指针、extern spawn…）。`.csv` 是原始数据。 |

## 四、上游对比

| 文件 | 作用 |
|---|---|
| [ikos-upstream-diff.md](ikos-upstream-diff.md) | **本地 vs 上游（NASA-SW-VnV/ikos @ ac7f7c1）的源码 diff 客观清单**：哪些是并发分析新增/改动、哪些是原版就有。用于区分「我们的改动」和「上游基线」。 |

## 五、复现用例

| 文件 | 作用 |
|---|---|
| [fp-repros/](fp-repros/) | **FP 最小复现 `.c` 用例**（8 个）：`c1-lock-pointer-alias`、`c3-per-thread-slot`、`c5-c11-acqrel`、`c6-cas-spinlock`、`c7-struct-field-collapse`、`c8-container-of` 等。每个对应 FP 分类清单里的一个根因，用于改动后做最小回归验证。 |

---

## 维护约定

- **`.csv` 是 `.md` 报告的数据源**，成对出现，改报告时同步更新。
- 文档里引用具体 commit/数字时，**写清对应 commit**，避免「数字从哪来」说不清。
- 删除文档前先确认没有别的文档/记忆引用它；删除后在本索引里同步移除条目。
