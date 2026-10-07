# docs/ 文档索引与学习路线

> 本目录是 IKOS **并发数据竞争检测**（DataRaceChecker）子系统的文档。
> 索引分两半：**上半部分是学习路线**（新手按顺序走），**下半部分是速查表**（会了之后按目标找）。
>
> 维护原则：根目录只留「当前工作实时相关」的文档；历史快照、已落地设计、官方翻译一律进 `archive/`。

---

## 一、学习路线（新手按顺序，标【难度】【前置】【读完会什么】）

> 不用一次读完。前 3 个阶段（约 1~2 小时）读完就能「看懂代码 + 知道每条判定逻辑为什么存在」；
> 阶段 4 是动手前必扫；「参考」按需查。

### 阶段 0 —— 这项目在干嘛（5 分钟）

| 文档 | 难度 | 读什么 |
|---|---|---|
| `../CLAUDE.md` | ⭐ | 全读：红线 FN=0、怎么 build / 跑测试、关键文件表 |

读完你会：知道「宁可多报不漏报」是铁律，知道改完要同步 install。

### 阶段 1 —— 数据竞争怎么被检测出来的（核心，先懂「为什么」）

| 文档 | 难度 | 读什么 | 跳过 |
|---|---|---|---|
| [ikos-race-theory-mapping.md](ikos-race-theory-mapping.md) | ⭐⭐ | 开头「红线」→ §2.0 总览表（16 个语义能力一眼扫）→ §2.4 两层判定架构 → §3.1 能力边界 | §2.1.x 各模块细节、§3.2 缺口、参考文献 |

读完你会：知道 IKOS 靠「抽象过近似（证 SAFE）+ BMC 欠近似（证 FALSE）+ 中间 UNKNOWN 兜底」两层架构同时做到 FN=0 和 FP=0，以及当前分数 1503 怎么来的。

### 阶段 2 —— 代码怎么组织（要动手必读）

| 文档 | 难度 | 读什么 |
|---|---|---|
| [ikos-architecture-survey.md](ikos-architecture-survey.md) | ⭐⭐ | 第一部分「原生流水线」+ 第二部分「并发改动清单」（哪些是我们加的、侵入内核没） |
| [concurrency-architecture.md](concurrency-architecture.md) | ⭐⭐⭐ | §1 五层分层 → §2 写/读数据流 → §4 判定 10 闸门（改代码时回来查文件:行号） |

读完你会：能把「一个访问从 Load 语句 → AccessRecord → 成对判定 → 报 RACE/UNKNOWN」这条链路在代码里指出来。

### 阶段 3 —— BMC 怎么找回竞争（进阶，可选）

| 文档 | 难度 | 读什么 |
|---|---|---|
| [ikos-bmc-design.md](ikos-bmc-design.md) | ⭐⭐ | §1 理论前提（SV-COMP 用 SC）→ §6 helper 内联的 4 类 FP gate → §7 落地结果 → §8 未做项 |

读完你会：知道 demote 成 UNKNOWN 的竞争对是怎么被 Z3 精确确认回 FALSE 的，以及 7 道 gate 各自挡什么。

### 阶段 4 —— 动手改代码前（必扫一遍）

| 文档 | 难度 | 读什么 |
|---|---|---|
| [dev-gotchas.md](dev-gotchas.md) | ⭐ | 22 条全扫（重点：#1 同步 install、#3 锁集 MAY/MUST 极性、#5 全局变量 type/points-to、#20 fixpoint dirty 收敛） |

---

## 二、按目标速查（已经会了，只想查某件事）

| 我要…… | 去读 |
|---|---|
| 改竞态判定逻辑（为什么报/不报） | `concurrency-architecture.md` §4 闸门 + `theory-mapping.md` §2.1.x 对应模块 |
| 加/改一个同步语义（锁/原子/cond/sem） | `theory-mapping.md` §2.1.x 对应小节 + 对应模块的 soundness 核验点 |
| 找回更多 TP（BMC 方向） | `ikos-bmc-design.md` §8「尚未做」 |
| 回收 lost-TN（SAFE 方向） | `theory-mapping.md` §3.2 的 235 UNKNOWN 拆桶 |
| 查某个 FP/FN 的历史根因 | `archive/`（fn-root-cause / fp-classification 的原始快照） |
| 准备 SV-COMP 提交 | `SVCOMP2027-最终核验清单.md`（打勾）+ `SVCOMP2027-干净环境检测流程.md` |
| 区分「我们的改动」vs「上游基线」 | `ikos-upstream-diff.md` |
| 了解 MHP 规划（未落地） | `ikos-mhp-design.md` |

---

## 三、全量索引（按功能，供参考）

### 理论
- [ikos-race-theory-mapping.md](ikos-race-theory-mapping.md) —— 理论总纲：A/B/C 分类、soundness、两层判定架构、235 UNKNOWN 拆桶

### 架构
- [ikos-architecture-survey.md](ikos-architecture-survey.md) —— 原生 vs 并发「宽」入门
- [concurrency-architecture.md](concurrency-architecture.md) —— 并发子系统「深」代码（文件:行号锚点）
- [ikos-upstream-diff.md](ikos-upstream-diff.md) —— 本地 vs 上游源码 diff 客观清单

### 设计
- [ikos-bmc-design.md](ikos-bmc-design.md) —— BMC 设计 + 演进（helper 内联 4 类 gate + 未做项）
- [ikos-bmc-complete.md](ikos-bmc-complete.md) —— **BMC 完整学习文档**（事件模型→展开→SMT→sound gate 全流程；铁证 vs 待验证设计严格分开）
- [dartagnan-svcomp2026-reference.md](dartagnan-svcomp2026-reference.md) —— Dartagnan 权威参照：SV-COMP 2026 官方版 7caef5c/4.3.1 + encodeDataRaces 机制 + 关键 commits→IKOS 缺口映射
- [ikos-vs-dartagnan-gap.md](ikos-vs-dartagnan-gap.md) —— 差距分析：176 TP vs 80 TP、23 独有、119 lost target 分桶 + 可实现性
- [ikos-mhp-design.md](ikos-mhp-design.md) —— ⚠️ MHP 设计（未实施，仅供思路）
- [dev-gotchas.md](dev-gotchas.md) —— 22 条踩坑清单

### SV-COMP 2027 参赛
- [SVCOMP2027-参赛指南.md](SVCOMP2027-参赛指南.md) —— 提交总入口（日期/计分/提交物）
- [SVCOMP2027-最终核验清单.md](SVCOMP2027-最终核验清单.md) —— 提交前打勾（代码实查版）
- [SVCOMP2027-干净环境检测流程.md](SVCOMP2027-干净环境检测流程.md) —— 考官视角免 Docker 流程

### 复现用例
- [fp-repros/](fp-repros/) —— 8 个 FP 最小复现 `.c`，改动后做最小回归

### 归档（archive/，只读）
历史快照（fn-root-cause / fp-classification / verification-report / goblint×2 对比 / cpachecker）、
已落地设计（condvar / 5b-mutex-hb）、官方页面翻译（规则/提交页/基准页/FAQ）。
数字已过时，结论已并入上方活跃文档。

---

## 维护约定

- 归档 = `git mv` 进 `archive/`，**不改内容**（快照的价值就在「冻结在那一刻」）。
- 合并 = 结论并入活跃文档后 `git rm`（原始内容由 git 历史保留）。
- 引用具体 commit/数字时写清 commit，避免「数字从哪来」说不清。
- 删除/移动前 `grep -rn 文件名` 全库查引用。
