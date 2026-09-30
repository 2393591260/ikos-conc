# SV-COMP 2027 参赛指南（事实已核查官方站点）

> 本文件所有事实均来自官方站点 `https://sv-comp.sosy-lab.org/2027/`（首页 / `rules.php` / `submission.php`），
> 于 2026-09-28 用 curl 抓取原文核对。取代之前的交接报告与使用说明。

## 1. 基本信息（已核查）

| 项 | 内容 |
|---|---|
| 届数 | 第 16 届 SV-COMP（International Competition on Software Verification） |
| 随会 | TACAS 2027 |
| 地点 | 丹麦哥本哈根（Copenhagen, Denmark） |
| 时间 | 2027 年 4 月（官网原文 `April ??, 2027`，**具体日期待定**） |
| 主页 | https://sv-comp.sosy-lab.org/2027/ |
| 规则 | https://sv-comp.sosy-lab.org/2027/rules.php |
| 提交 | https://sv-comp.sosy-lab.org/2027/submission.php |
| 沟通 | Zulip（邮件联系组织者 Dirk Beyer / Jan Strejček 申请邀请）；邮件列表 sv-comp |

## 2. 关键日期（已核查，逐条与官网一致）

| 日期 | 节点 |
|---|---|
| 2026-07-31 ~ 08-09 | 公布 SV-COMP 2027 与支持的 witness 格式 |
| **2026-10-08** | **工具注册截止**（建 merge request）+ 验证任务提交截止（sv-benchmarks 仓库 MR） |
| **2026-10-20** | **工具提交截止**（所有 CI 检查须通过） |
| 2026-11-03 | 验证任务冻结 |
| 2026-11-10 | 合格工具通知 |
| 2026-11-17 | 最终工具重提交截止（评估阶段开始） |
| 2026-12-01 | 离线基准阶段完成、结果传达给参赛者 |
| 2026-12-04 | 结果公开发布 |
| 2026-12-17 | 参赛短文（competition contribution）提交 |
| 2027-01-14 / 01-21 | 短文通知 / 定稿 |

> 当前距注册截止（10-08）约 10 天。

## 3. 目标类别与 witness 要求（已核查 rules.php 表格）

| 类别 | 正确性 witness | 违反性 witness |
|---|---|---|
| **C.no-data-race（主目标）** | **not supported** | **2.2** |
| C.unreach-call.Concurrency（可选） | 2.1+ | 2.2 |
| C.no-overflow.Concurrency（可选） | 2.1+ | 2.2 |
| C.valid-memsafety.Concurrency（可选） | not supported | 2.2（demo mode，仅 valid-deref/valid-free） |

- witness 为 YAML 格式，文件名固定 `witness.yml`，交给 witness validator 校验有效性。
- **资源上限**：2 处理单元、7GB 内存；违反性 witness 90s（=验证时间的 10%）、正确性 witness 300s（33%）。

## 4. 参赛需提交的 4 部分（已核查 submission.php，自 2026 起简化）

1. **Zenodo 工具归档**（`.zip`）：单一顶层目录（非 tarbomb），含 `LICENSE`、`README`、`smoketest.sh`（对内置示例运行、exit 0）+ 全部可执行文件与依赖；stdout/stderr ≤ 2MB；支持 `--version`；自包含、任意路径可运行、断网可跑；不含 `.git`/源码/`__MACOSX`/多余测试。上传 Zenodo 得 DOI。
2. **FM-Tools 条目** `<tool_id>.yml`：version（Zenodo DOI）、参赛声明、jury member（须是贡献开发者，且名字出现在项目页/论文/提交记录，或经组织者批准）、标签 `meta_tool`（若元验证器）/ `ai`（若用 LLM）、描述。MR 只在 `smoketest.sh` exit 0 时才合并。
3. **BenchExec tool-info 模块**：加入 BenchExec 仓库，负责拼命令行、解析输出判定结果、取版本。
4. **category-structure.yml 的 MR**：声明参加哪些类别（`C.no-data-race`；用 opt-out/opt-in 控制子类别）。

## 5. 其他已核查要点

- **无需 EasyChair**（2026 起取消）：纯工具提交即可；短文在结果公布后另行提交。
- **上届工具自动注册为 inactive**：本工具为全新，无此问题。
- **工具标签**：用 LLM/AI 须标 `ai`；元验证器标 `meta_tool`（本工具预计都不是）。
- **新 jury member 需向组织者报备邮箱**。
- **合格条件**：FM-Tools 里 flags 正确 + 归档含合格 LICENSE + 与其他工具在概念/实现上有差异 + 有贡献 jury member。

## 6. 待办清单（按优先级，逐步操作）

### 已完成（代码侧，2026-09-29 提交）

- [x] **P0 审计现有包装器**：结论 a半✅（缺属性文件，P3 兜底）b✅ c❌。详见 P1~P4。
- [x] **P1 结果包装器**：`benchexec/tools/ikos-conc.py`（与 P3 合流），固定 `--analyses=race --concurrency=auto`，映射 TRUE/FALSE(no-data-race)/UNKNOWN；`--format=no` 控 stdout 体量。
- [x] **P2 witness 生成器**：`svcomp_witness.py` 把竞态报告转格式 2.2 `witness.yml`（`function_enter` 注册线程 + 2 个 `target` 结尾段）；过官方基础 witnesslint（exit 0）。`data_race.cpp` 侧补了每个访问的 `line/column/file` + `thread_creations`。
- [x] **P3 BenchExec 模块**：`benchexec/tools/ikos-conc.py`（`executable/version/cmdline/determine_result`），tool id `ikos-conc`；`cmdline()` 跑 `svcomp_witness.py`，报 FALSE 自动写 witness。
- [x] **P4 自包含归档**：`svcomp/package.py` 产出 `ikos-conc.zip`（120MB）：打包 APRON+LLVM-14、settings 可重定位、wrapper 用系统 python3、smoketest.sh exit 0。已解压到干净目录验证 `--version` + 竞态分析 + witness 全通。

### 待你手动完成（依赖外部账号/信息，我无法代办）

- [ ] **上传 `ikos-conc.zip` 到 Zenodo → 拿 DOI**。
- [ ] **填 `svcomp/fm-tools-entry.yml` 占位符**：`<doi>`、jury member 的 `orcid/name/institution/country`、`<gitlab-username>`。
- [ ] **提交三个 MR**：fm-tools 的 `data/ikos-conc.yml`、bench-defs 的 `category-structure.yml`（见 `svcomp/category-structure-mr.md`）、BenchExec 的 `benchexec/tools/ikos-conc.py`。
- [ ] **新 jury member 报备邮箱**给组织者（Dirk Beyer / Jan Strejček）。

### 提交前建议复核（我离线没核实的点）

- [ ] 错误答案罚分：本文按 **−16** 计算（rules.php 提交前确认 2027 是否调整）。
- [ ] `required_ubuntu_packages` 清单：建议在 `ubuntu:24.04` 容器里跑一遍 `smoketest.sh` 验证（尤其 libppl/libboost/tbb 版本号）。
- [x] `data_model`：已从任务 `.yml` 读 ILP32/LP64，`-m 32/64` 透传 clang + witness `data_model` 字段对齐（`28286aa` + `8048181`）。

### 当前基线（2026 冻结集全量，1029 任务，**全部 ILP32**）

TP=235 FP=100 TN=694 **FN=0** err=0；precision 70.1%。红线 FN=0 守住。已落地的 sound FP 修复：

- **ILP32 bitcast-callee 修复（`ca1907f`，杠杆最大）**：32 位下 clang 给无原型函数 `__VERIFIER_atomic_begin/end` 的调用包 `bitcast`，`call->called()` 不再是 `FunctionPointerConstant`，原子伪锁识别失效 → 原子段全报 FP。把识别挪到 `exec_extern_call`（用已穿透 bitcast 的 `fun->name()`）。**FP 404→108**（TN 390→686），FN=0 不破。
- **fresh 堆节点修复（`fc8339c`，C 桶）**：`malloc` 结果是线程私有（fresh）直到发布；lock集加 flow-sensitive 的 MUST `fresh` digest，存进全局/已发布堆字段/线程实参时移除。**FP 108→100**（TN 686→694），FN=0 不破（goblint region 域「fresh bullet」）。
- `free()` 排除出 extern-call 写合成（`99b7c88`）——free 不通过指针写数据；
- stdio 输出族（printf/fprintf/…）排除出写合成（`9d7edb6`）——其指针实参是流/格式串/要打印的值。

> ⚠️ 基线更正（2026-09-29 → 2026-09-30）：1029 个 no-data-race 任务**全是 ILP32**（无 LP64 任务）。此前 FP=173 是**默认 LP64（错误 arch）**测的；`-m32`（正确 ILP32）一度 FP=404 —— 根因不是「数值域退化」，而是上面那条 bitcast 让 `__VERIFIER_atomic_*` 识别全线失效。修完后正确 ILP32 的 108 FP **比错误 LP64 的 172 FP 还低**。

剩余 FP 大头（对着 ILP32 重测）：weaver 的 ⊤ points-to、无锁线性化、per-thread 槽位、互斥传递 HB、container_of 等，见 `docs/ikos-race-theory-mapping.md` §3.2。注册/提交不看成绩，见 §5。

## 7. 相关仓库

- FM-Tools：https://gitlab.com/sosy-lab/benchmarking/fm-tools
- BenchExec：https://github.com/sosy-lab/benchexec
- sv-benchmarks：https://gitlab.com/sosy-lab/benchmarking/sv-benchmarks
- bench-defs：https://gitlab.com/sosy-lab/sv-comp/bench-defs
- witness 格式：https://github.com/sosy-lab/sv-witnesses
