# docs/ 文档索引

> 本目录存放 IKOS **并发数据竞争检测** 子系统的文档。本索引解释每个文件/目录的作用，并给出阅读顺序。
>
> 维护原则：只保留**当前工作实时相关**的文档在根目录；历史快照、已落地的设计、官方页面翻译一律进
> `archive/`（不再更新，只读）。判断标准：**快照（带时点数字）vs 结论（永久有效的框架）分开**；
> **已落地设计 → 结论并入 theory-mapping、原文归档**；**未落地设计 → 保留但标注「未实施」**。

## 阅读顺序（新人 / 新会话入门）

0. **`../CLAUDE.md`** —— 仓库根自述：项目定位/红线（FN=0）/构建/跑测试/关键文件/核心 gotchas，先读这个
1. **dev-gotchas.md** —— 完整 22 条踩坑清单（改代码前扫一遍，省得重踩）
2. **ikos-architecture-survey.md** —— IKOS 整体架构（原生 vs 并发修改版：目录/模块/改动分类/风险）
3. **concurrency-architecture.md** —— 并发子系统代码长什么样（文件:行号锚点）
4. **ikos-race-theory-mapping.md** —— **理论总纲**：A/B/C 分类、soundness、两层判定架构（demote+BMC）
5. **ikos-bmc-design.md** —— BMC 模块设计 + 演进史 + 未做项（读 theory-mapping §2.1.17/§2.4 后看这份）

---

## 一、理论总纲

| 文件 | 作用 |
|---|---|
| [ikos-race-theory-mapping.md](ikos-race-theory-mapping.md) | **最常翻的「为什么这么做」**。IKOS 原生有哪些抽象解释理论 + 并发这一块建了哪些语义（lockset/HB/PBR/region/atomic/fresh-heap/BMC…）、A/B/C 分类、soundness 前提、两层判定架构（抽象过近似 + BMC 欠近似 = FN=0/FP=0）、235 UNKNOWN 拆桶与路线。FN/FP 根因的分类框架也收在这里 §3.2（原 fn-root-cause / fp-classification 已归档）。 |

## 二、架构

| 文件 | 作用 |
|---|---|
| [ikos-architecture-survey.md](ikos-architecture-survey.md) | **架构入门（宽）**：原生 IKOS 流水线 + 并发修改版改动清单（侵入内核的点、风险）。 |
| [concurrency-architecture.md](concurrency-architecture.md) | **并发子系统深潜**：五层分层、写/读数据流、核心算法伪代码、判定 10 闸门、数据结构。与 survey 一宽一深。 |
| [ikos-upstream-diff.md](ikos-upstream-diff.md) | **本地 vs 上游（NASA-SW-VnV/ikos @ ac7f7c1）源码 diff 客观清单**：区分「我们的改动」和「上游基线」。 |

## 三、设计与实现

| 文件 | 作用 |
|---|---|
| [ikos-bmc-design.md](ikos-bmc-design.md) | **BMC 模块设计 + 演进**：AR→SMT 映射、svcomp.cat（SC）理论前提、soundness 精确条件、工程分期、helper 内联 4 类 FP gate、落地结果（1503）、尚未做项（精确 create/join HB / 无锁协议 / 值域可行性）。由原 bmc-design + helper-inlining + learning-notes 三份合并。 |
| [ikos-mhp-design.md](ikos-mhp-design.md) | **⚠️ MHP（may-happen-in-parallel）对标 goblint 设计（未实施）**：实例级 create/join HB + 祖先关系的方案。待办专项，落地前仅供思路参考。 |
| [dev-gotchas.md](dev-gotchas.md) | **完整 22 条踩坑清单**：build 须同步 install、锁集 MAY/MUST 极性、全局变量 type/points-to 陷阱、fixpoint dirty 收敛等。 |

## 四、SV-COMP 2027 参赛（提交前活跃，提交后整体归档）

| 文件 | 作用 |
|---|---|
| [SVCOMP2027-参赛指南.md](SVCOMP2027-参赛指南.md) | **提交总入口**：关键日期、计分公式、提交物、待办。 |
| [SVCOMP2027-最终核验清单.md](SVCOMP2027-最终核验清单.md) | **提交前打勾清单（代码实查版）**：逐条对照官方要求 + 代码证据 + 状态。 |
| [SVCOMP2027-干净环境检测流程.md](SVCOMP2027-干净环境检测流程.md) | **考官视角免 Docker 流程**：全新 VM + 22 包 + BenchExec 跑全量，预期 1503。 |

## 五、复现用例

| 路径 | 作用 |
|---|---|
| [fp-repros/](fp-repros/) | FP 最小复现 `.c` 用例（8 个），每个对应一个 FP 根因，改动后做最小回归。 |

## 六、归档（archive/，只读，不再更新）

历史快照 / 已落地设计 / 官方页面翻译。数字已过时，结论已并入上方活跃文档：

| 归档文件 | 性质 | 结论去向 |
|---|---|---|
| fn-root-cause-analysis.md | 「50 FN」根因病理快照（现 FN=0） | 三维分类框架 → theory-mapping §3.2 |
| ikos-race-fp-classification.md | 「227→182 FP」分类快照（现 demote 后 FP=0） | A/B/C 三档 → theory-mapping §3.2 |
| ikos-race-verification-report.md | 1309 行逐用例复现转录 | 可经 `boundary-tests/run.py` 再生成 |
| ikos-vs-goblint-race-comparison.md（+.csv） | IKOS vs goblint 1030 任务对比 | 时点数据 |
| ikos-on-goblint-tests.md（+.csv） | IKOS 跑 goblint 262 回归测试 | 时点数据 |
| ikos-vs-cpachecker-对比报告.md | CPAchecker 交叉验证（12 样本试点） | 已被 witness 验证取代 |
| ikos-condvar-modeling-design.md | 条件变量建模方案（已落地） | soundness 结论 → theory-mapping §2.1.14 |
| ikos-5b-mutex-hb-soundness.md | mutex-HB 不可 sound 落地结论 | → theory-mapping §3.2 注意4 |
| SVCOMP2027-规则/提交页/基准页翻译、SVCOMP-FAQ翻译 | 官方页面翻译快照 | 核验清单已蒸馏可执行结论 |

---

## 维护约定

- 归档 = `git mv` 进 `archive/`，**不改内容**（快照的价值就在「冻结在那一刻」）。
- 合并 = 结论并入活跃文档后 `git rm`（原始内容由 git 历史保留）。
- 引用具体 commit/数字时，写清 commit，避免「数字从哪来」说不清。
- 删除/移动前确认没有别的文档/记忆/脚本引用它（`grep -rn 文件名` 全库查）。
