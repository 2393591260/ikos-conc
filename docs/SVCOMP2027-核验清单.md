# SV-COMP 2027 核验清单（按比赛规则逐条）

> 提交前逐条打勾。每条含「**目的**」= 规则为什么这么要求，帮助判断「这条到底在防什么」。

---

## A. Zenodo 工具归档（`ikos-conc.zip`）

| # | 要求 | 目的 | 状态 |
|---|---|---|---|
| A1 | **单一顶层目录**（非 tarbomb），如 `ikos-conc/` | 解压后不会把一堆文件散落到当前目录，评审方好定位工具 | ✅ `ikos-conc/` |
| A2 | 含 **LICENSE** | 法律上允许 SV-COMP 运行/再分发你的工具；缺了直接判不合格（§5 合格条件之一） | ✅ NASA-1.3 |
| A3 | 含 **README** | 说明工具是什么、怎么跑 | ✅ |
| A4 | 含 **smoketest.sh**，对内置示例运行、**exit 0** | 评审方自动冒烟测试：证明归档「解压即能跑」。MR 只在它 exit 0 时才合并 | ✅ 已验证 |
| A5 | **全部可执行文件与依赖**（自包含） | 评测机是隔离容器，不能装你的私有依赖（APRON/LLVM14） | ✅ 打包 APRON+LLVM14 |
| A6 | **stdout/stderr ≤ 2MB** | 防止工具刷屏淹没评测日志（跑死循环/狂打印） | ✅ `--format=no` |
| A7 | 支持 **`--version`** | 评审方校验工具版本与 FM-Tools 里声明的 version 一致 | ✅ `ikos 3.5` |
| A8 | **可重定位**（任意路径可跑） | 评测机解压路径未知，不能有硬编码绝对路径 | ✅ settings 相对路径 |
| A9 | **断网可跑** | 评测环境无外网，不能运行时拉依赖/模型 | ✅ |
| A10 | 不含 `.git` / 源码 / `__MACOSX` / 多余测试 | 体积 + 合规（源码夹带会判问题）；避免 Mac 打包残留 | ✅ 已清理 |

---

## B. FM-Tools 条目（`data/ikos-conc.yml`）

| # | 要求 | 目的 | 状态 |
|---|---|---|---|
| B1 | `id` 唯一（`ikos-conc`） | 全局工具标识，与 BenchExec 模块名、归档目录一致 | ✅ |
| B2 | `version`（version 字段填 **Zenodo DOI**） | 把「这个参赛版本」锁定到归档，可追溯、可复现 | ⏳ 待 DOI |
| B3 | **参赛声明 / 描述** | 说明工具是做什么的（评审 + 论文用） | ✅ 模板 |
| B4 | **jury member**（须是贡献开发者，名字出现在项目页/论文/提交记录，或经组织者批准） | 保证有真人负责、且真参与了工具开发（防止「空壳参赛」） | ⏳ 待填身份 |
| B5 | **标签**：`ai`（若用 LLM）/ `meta_tool`（若元验证器） | 公平性：用了 AI 要公开声明，元验证器单独计 | ✅ 本工具两者都不标 |
| B6 | `benchexec_toolinfo_module` 指向 `benchexec.tools.ikos-conc` | 评测时 BenchExec 用它拼命令/判结果 | ✅ |
| B7 | `required_ubuntu_packages` / `base_container_images` | 告诉评测方工具需要哪些系统包，容器里装 | ⏳ 待容器验证 |

---

## C. BenchExec tool-info 模块（`benchexec/tools/ikos-conc.py`）

| # | 要求 | 目的 | 状态 |
|---|---|---|---|
| C1 | `executable()` 返回工具名 | BenchExec 定位归档里的可执行文件 | ✅ |
| C2 | `version()` | 校验版本 | ✅ |
| C3 | `cmdline()` 拼命令行（含 `--propertyfile` 处理） | 评测时用统一方式起工具，属性由 BenchExec 传 | ✅（不透传 propertyfile，固定 no-data-race） |
| C4 | `determine_result()` 把输出映射成 TRUE/FALSE/UNKNOWN | 把工具文本输出翻译成比赛裁决 | ✅ |
| C5 | 报 FALSE 时产出 `witness.yml` | 「FALSE + Witness」规则要求 | ✅ |

---

## D. category-structure.yml 的 MR（bench-defs）

| # | 要求 | 目的 | 状态 |
|---|---|---|---|
| D1 | 把 `ikos-conc` 加入 `C.Concurrency`（含 `C.no-data-race.Concurrency`）的 `verifiers` | 声明参加哪个类别，评测时才会跑到你 | ⏳ 待提交 MR |
| D2 | 用 **opt-out/opt-in** 控制子类别（若只想要 no-data-race） | 不想参加 no-overflow/unreach-call 等并发子类时，显式声明 | ⏳ 默认全参加 |

---

## E. 合格条件（§5，决定「是否算参赛」）

| # | 条件 | 目的 | 状态 |
|---|---|---|---|
| E1 | FM-Tools 里 **flags 正确** | 保证工具按声明的方式跑（不玩花样） | ✅ |
| E2 | 归档含**合格 LICENSE** | 法律合规（=A2） | ✅ |
| E3 | 与其他工具**概念/实现上有差异** | 防止同一工具换个名重复参赛刷分 | ✅（IKOS 为底、自研 thread-modular + lockset/HB） |
| E4 | 有**贡献 jury member** | 有真人负责（=B4） | ⏳ 待填 |

---

## F. 关键日期节点（务必卡点）

| 日期 | 节点 | 需要动作 |
|---|---|---|
| **2026-10-08** | 工具注册 + 验证任务提交截止 | 提交 B、C、D 三个 MR |
| **2026-10-20** | 工具提交截止（CI 全绿） | A 归档 DOI 定稿 + B 里 DOI 填齐 |
| 2026-11-03 | 验证任务冻结 | — |
| 2026-11-10 | 合格工具通知 | 等结果 |
| 2026-11-17 | 最终工具重提交（评测开始） | 如需改 bug，最后一次提交 |
| 2026-12-01 | 离线基准完成 | — |
| 2026-12-04 | 结果公开 | — |
| 2026-12-17 | 参赛短文提交 | 另写短文 |

---

## G. 提交前仍需人工核实（我离线没核到的）

| # | 待核实项 | 风险 |
|---|---|---|
| G1 | 错误答案**罚分**（本文按 −16 算） | 影响 FP 的代价评估，rules.php 复核 |
| G2 | `required_ubuntu_packages` 精确清单 | 在 `ubuntu:24.04` 容器跑 smoketest 验证 |
| G3 | **data_model**（ILP32/LP64） | ✅ 已修（`28286aa`）：tool-info 读 `task.options["data_model"]` → `-m 32/64`，witness 字段对齐；32 位头文件已列进 required_ubuntu_packages |
| G4 | witness validator（非 linter）实际验收 | CPAchecker 4.2.2 尚不支持 2.2 格式（连官方示例都解析不了）→ 语义验证暂做不了，等官方新版 validator |
| G5 | 工具须**公开可访问**（repo 需 public） | 规则原文「participating tools are required to be publicly available on the internet」——确认 `github.com/2393591260/ikos-conc` 已公开 |
