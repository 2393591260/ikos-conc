# docs/ 文档索引

> 本目录存放 IKOS **并发数据竞争检测** 子系统的文档。本索引解释每个文件/子目录的作用，
> 并给出阅读顺序。维护原则：只保留「当前工作实时相关」的文档；一次性笔记、已回退/de-scoped
> 的方案不在此列（见 git 历史 `12d90ec`）。

## 阅读顺序（新人 / 新会话入门）

0. **`../CLAUDE.md`** —— 仓库根自述：项目定位/红线（FN=0）/构建/跑测试/关键文件/核心 gotchas，先读这个
1. **dev-gotchas.md** —— 完整 22 条踩坑清单（改代码前扫一遍，省得重踩）
2. **ikos-architecture-survey.md** —— 先看 IKOS 整体架构（原生版 vs 并发修改版的目录/模块/改动分类/风险）
3. **concurrency-architecture.md** —— 再看并发子系统代码长什么样
4. **ikos-race-theory-mapping.md** —— 再看理论依据与 soundness 前提
5. **fn-root-cause-analysis.md** —— FN 根因病理 / **ikos-race-fp-classification.md** —— FP 根因分类
6. 其余按需查阅（MHP 设计 / condvar 设计 / 上游 diff）

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
| [concurrency-architecture.md](concurrency-architecture.md) | **并发子系统代码架构梳理**：目录结构、运行原理（thread-modular + 锁集 digest + ConcurrentGlobalEnv 黑板）、已知边界。 |
| [ikos-bmc-design.md](ikos-bmc-design.md) | **BMC 找回 TP 设计**：demote→UNKNOWN 后用 Z3 路径敏感展开重确认竞争（只报 FALSE、不报 TRUE 的 sound 架构）、8 个 sound 修复的演进、1476/1478 基线。 |
| [ikos-bmc-helper-inlining-design.md](ikos-bmc-helper-inlining-design.md) | **BMC helper 内联 + 值域可行性（第二期设计）**：找回「访问在辅助函数里」的 TP（pthread-atomic/40_barrier 共 ~11 个）的技术路线——展开器跟进函数调用 + `int_to_interval` 跳过不可达分支防条件写 FP。 |
| [ikos-bmc-learning-notes.md](ikos-bmc-learning-notes.md) | **BMC 学习笔记**：展开器/编码/HB 边的设计记录与踩坑。 |
| [ikos-mhp-design.md](ikos-mhp-design.md) | **MHP（may-happen-in-parallel）对标 goblint 设计**：goblint `mHP.ml`/`threadIdDomain.ml` vs IKOS 现状的逐维度对比 + 实例级 MHP 方案（instance_sid / _site_ancestors / may_happen_in_parallel 四判定）。后续 MHP 专项依据。 |
| [ikos-condvar-modeling-design.md](ikos-condvar-modeling-design.md) | **条件变量语义建模方案**（`pthread_cond_wait/signal/broadcast`）：wait-queue + signal epoch + MAY digest 的设计与 soundness 论证。 |
| [dev-gotchas.md](dev-gotchas.md) | **完整 22 条踩坑清单**：改 build 须同步 install、锁集 MAY/MUST 极性、全局变量 type/points-to 陷阱、fixpoint dirty 收敛、extern-call 写合成双计等。 |

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

## 六、SV-COMP 2027 参赛（提交材料 + 规则翻译）

| 文件 | 作用 |
|---|---|
| [SVCOMP2027-参赛指南.md](SVCOMP2027-参赛指南.md) | **参赛指南（事实已核查官方站点）**：关键日期、计分公式、4 部分提交物、待办、当前基线（demote 1412 / BMC 1476 / 滚动 1478）。**提交总入口，先读这个。** |
| [SVCOMP2027-最终核验清单.md](SVCOMP2027-最终核验清单.md) | **最终核验清单（代码实查版）**：逐条对照代码实查的 A/B/C/D/E/F/G 状态 + 待办排序。提交前照这个打勾。 |
| [SVCOMP2027-规则全文翻译与核验.md](SVCOMP2027-规则全文翻译与核验.md) | 规则页（rules.php）全文翻译 + 逐条讲解（见证格式/计分/环境/opt-out…）。 |
| [SVCOMP2027-提交页翻译与核验.md](SVCOMP2027-提交页翻译与核验.md) | 提交页（submission.php）全文翻译 + 复核（归档 5 部分/FM-Tools/合格条件…）。 |
| [SVCOMP2027-基准页翻译.md](SVCOMP2027-基准页翻译.md) | 基准页（benchmarks.php）翻译（2027 类别清单当时是占位符，以 11-03 冻结为准）。 |
| [SVCOMP-FAQ中文翻译.md](SVCOMP-FAQ中文翻译.md) | 官方 FAQ 中文翻译（跨年通用；机制性回答仍适用，具体格式/链接以 2027 页为准）。 |

---

## 维护约定

- **`.csv` 是 `.md` 报告的数据源**，成对出现，改报告时同步更新。
- 文档里引用具体 commit/数字时，**写清对应 commit**，避免「数字从哪来」说不清。
- 删除文档前先确认没有别的文档/记忆引用它；删除后在本索引里同步移除条目。
