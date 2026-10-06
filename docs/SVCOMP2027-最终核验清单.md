# SV-COMP 2027 最终核验清单（代码实查版）

> 生成日期：2026-10-03
> 依据：官方四份翻译（`docs/archive/SVCOMP-FAQ中文翻译.md`、`SVCOMP2027-规则全文翻译与核验.md`、`SVCOMP2027-提交页翻译与核验.md`、`SVCOMP2027-基准页翻译.md`，已归档），逐条对照**代码实查**（非文档/记忆）。线上核实：FM-Tools 格式（goblint.yml）、bench-defs category-structure.yml。
> 状态图例：✅ 已具备 / ⚠️ 部分/需补 / ❌ 缺失 / 🚧 未验证（外部动作）

---

## 〇、当前状态（2026-10-05 更新，覆盖下文旧表格）

**代码/得分**（本会话）：helper 内联 + cast 修复落地后，冻结集 **TP=85 / FP=0 / TN=709 / FN=0 / err=0 = 1503 分**（demote+BMC 基线 1476 → 1503，红线 FP=0/FN=0 全程守住；commit fa3c4aa / 319dac6 / 4daf3d0）。

**外部提交状态**：

| 提交物 | 状态 |
|---|---|
| Zenodo 归档 + DOI `10.5281/zenodo.23122650` | ✅ 已上传；**代码本次有更新，10-20 前需重传新版归档** |
| fm-tools-entry.yml（B 节） | ✅ 字段已补齐（competition_participations / techniques / frameworks_solvers / literature / jury member / DOI） |
| fm-tools MR | 🚧 已提交；之前 CI 卡「no matching runners」，字段补齐后待确认变绿 |
| bench-defs category-structure MR（D 节） | 🚧 已提交，待合并 |
| BenchExec tool-info PR（C 节） | 🚧 已提交，待合并 |
| jury member 邮箱（E4） | ✅ 已发 + 已回复（GitLab 用户名 `ruanronghui` + 告知两个 MR 情况） |
| Zulip | 🚧 Dirk 已发邀请，待加入 |
| 见证 2.2 + CPAchecker 复核（G4） | ✅ FALSE 见证已被 CPAchecker 确认 |

---

## 一、归档（Zenodo .zip）—— 代码 `svcomp/package.py`

| # | 官方要求（提交页 §1） | 代码证据 | 状态 |
|---|---|---|---|
| A1 | 恰好一个顶层目录（非 tarbomb） | `package.py` zip 段：`arc = os.path.join("ikos-conc", relpath(...))`，所有条目挂在 `ikos-conc/` 下 | ✅ |
| A2 | 含 LICENSE（允许任何人复现/评估、**不得限制日志/见证输出**） | `package.py`：`shutil.copy("LICENSE.txt", install/LICENSE.txt)`；许可证 `spdx: NASA-1.3`（NASA 开源，无输出限制条款） | ✅ |
| A3 | 含 README 描述归档内容 | `package.py`：内嵌 `README` 字符串写入 `install/README.md` | ✅ |
| A4 | 含 smoketest.sh 跑内置示例、exit 0（**MR 合并门禁**） | `package.py` 内嵌 `SMOKETEST`（含 `--version` + `-m32` 竞态 + demote+BMC 断言 `definitely UNSAFE`）；实测 `install/smoketest.sh` exit 0 | ✅ |
| A5 | 自包含：特殊库全打包，不依赖机器装特殊软件 | `package.py`：打包 APRON（`libapron/libboxMPQ/liboctMPQ/libpolkaMPQ/libap_ppl/libap_pkgrid.so`）+ clang/opt + `libLLVM-14.so.1` + `libclang-cpp.so.14`；标准包走 FM-Tools `required_ubuntu_packages` | ✅ |
| A6 | stdout/stderr ≤ 2MB | 工具路径 `benchexec/tools/ikos-conc.py::cmdline` → `svcomp_witness.py`，ikos 以 `--format=json --report-file=` 跑，stdout 只有 summary 几行（JSON 落盘、不进 stdout） | ✅ |
| A7 | 有报告自身版本的选项（`--version`） | `analyzer/python/ikos/analyzer.py:95` `add_argument('--version', action=args.VersionAction)`；`settings.py.in:57` `VERSION = PACKAGE_VERSION (+git)` | ✅ |
| A8 | 不含 .git/源码/`__MACOSX`/大量测试文件 | zip 只打包 `install/`（make install 产物）；实查 `install/` 无 `.git`/`*.cpp`/`__MACOSX` | ⚠️ 见下 A8' |
| A9 | 任意路径可执行、不依赖工作目录 | `package.py::patch_settings`：`PREFIX = os.path.abspath(join(dirname(__file__), *([os.pardir]*6)))`；wrapper 用 `dirname "$0"` 定位 | ✅ |
| A10 | 断网可跑 | 依赖全在归档内（APRON/LLVM/headers），wrapper 只调系统 `python3` | ✅ |

**A8' 已修复**：`package.py` 现已显式排除 `.db`/`__pycache__`/`.pyc`/源码头文件（打包段加排除守卫），`install/output.db` 不会进 zip。

---

## 二、FM-Tools 条目 —— 代码 `svcomp/fm-tools-entry.yml`

| # | 官方要求（提交页 §2 + FM-Tools 格式 2.0，据 goblint.yml 核实） | 代码证据 | 状态 |
|---|---|---|---|
| B1 | 唯一 `id`、`name` | `id: ikos-conc`、`name: IKOS-ConC` | ✅ |
| B2 | `version` 字段填 **Zenodo DOI**（非版本号字符串） | `versions[].version: "svcomp27"`、`doi: 10.5281/zenodo.23122650` | ✅ 已回填 DOI |
| B3 | **参赛声明（participation declaration）** | `competition_participations[]` 已填（`competition: "SV-COMP 2027"` / `track: no-data-race` / `tool_version: svcomp27` / `label`） | ✅ |
| B4/E4 | jury member（贡献开发者；**新成员须报备邮箱**） | `competition_participations[].jury_member` 已填 Ronghui Ruan（ORCID 0009-0007-5722-0468 / NUDT / China）+ `participants[]` | ✅ |
| B5 | 标签：元验证器标 `meta_tool`、用 LLM/AI 标 `ai` | 我们两者皆非，不加（当前 yml 无这两字段，正确） | ✅ |
| B6 | 描述（原理 + 组件） | `description:` 已写 thread-modular + lockset/HB + BMC 重确认 | ✅ |
| B7 | Ubuntu 标准包清单（`required_ubuntu_packages`） | 已列 21 个包 + `libz3-4`，容器 `ubuntu:24.04` | 🚧 待容器实测（=G2） |

**B 节已解决**：`fm-tools-entry.yml` 的 4 个缺口字段（`competition_participations[]` / `techniques`（Abstract Interpretation, Concurrency Support, Bounded Model Checking, Numeric Interval Analysis）/ `frameworks_solvers`（Apron, Z3）/ `literature`（IKOS SEFM 2014, DOI 10.1007/978-3-319-10431-7_20））均已补齐，jury member 也放到了正确位置 `competition_participations[].jury_member`。

---

## 三、tool-info 模块 —— 代码 `benchexec/tools/ikos-conc.py`

| # | 官方要求（FAQ Q3 / 规则 §7.3 / 提交页 §3） | 代码证据 | 状态 |
|---|---|---|---|
| C1 | `executable()` 定位可执行文件 | `executable()` 返回 `tool_locator.find_executable("ikos")` | ✅ |
| C2 | `version()` 取版本字符串 | `version()` 解析 `--version` 首行末词（"3.5"），避开 License 尾行 | ✅ |
| C3 | `cmdline()` 拼命令行（属性固定 no-data-race，不透传 propertyfile） | `cmdline()` 调 `svcomp_witness.py --ikos <exe> --data-model <ILP32>`；注释明示「属性永远 no-data-race，不透传 propertyfile」 | ✅ |
| C4 | `determine_result()` 把输出翻译成 TRUE/FALSE/UNKNOWN | `verdict_from_text()`：`RE_DEF_UNSAFE→FALSE`、`RE_UNKNOWN/RE_POT_UNSAFE→UNKNOWN`、`RE_SAFE→TRUE`、兜底 UNKNOWN | ✅ |
| C5 | 报 FALSE 产出 witness（**no-data-race 违例见证必须 2.2**） | `svcomp_witness.py::FORMAT_VERSION="2.2"`、`SPECIFICATION="G ! data-race"`；`first_race()` 只认 `status==2`（Result::Error）才写 witness.yml | ✅ |

**判定口径（report.py 与 tool-info 对接）**——`analyzer/python/ikos/report.py`：
- `summary.error!=0` → `The program is definitely UNSAFE`（→FALSE，BMC 确认的竞争 / 未 demote 的竞争）
- `summary.unknown!=0` → `The program is UNKNOWN`（Warning 且 info.verdict=="unknown"，即 points-to ⊤ 模型边界，由 `_is_unknown_check()` 判）
- `summary.warning!=0` → `The program is potentially UNSAFE`（demote 后未被 BMC 确认的竞争）
- 否则 → `The program is SAFE`（→TRUE）

对应 `data_race.cpp` 析构：`group_is_unknown`（⊤）→ `Result::Warning` + `info.verdict="unknown"`；`demote && !bmc_confirmed` → `Result::Warning`（无 verdict 标记）；`bmc_confirmed` → `Result::Error`。

---

## 四、category-structure MR —— 代码 `svcomp/category-structure-mr.md`

| # | 官方要求（提交页 §4，比规则页更准） | 代码证据 | 状态 |
|---|---|---|---|
| D1 | 声明参加哪些类别（benchmark definition 经 MR 进 bench-defs） | 顶层 `opt_in:` 表插入 `ikos-conc`，MR 已提交 | 🚧 待合并 |
| D2 | opt-in / opt-out 语义正确 | 已核实：**opt-in**（只进 `opt_in`、不进 `C.Concurrency.verifiers`）只参加 `C.no-data-race.Concurrency` + `C.no-data-race.Huawei-Concurrency-Challenges` | ✅ 语义正确 |

**D 节线上核实（bench-defs category-structure.yml 实查）**：
- 两个 no-data-race 基础类别名**确实存在**：`C.no-data-race.Concurrency`、`C.no-data-race.Huawei-Concurrency-Challenges`（后者在 `demo_categories` 里）。
- 无 `C.ConcurrencySafety` 这个名字（mr 文档已正确指出）。
- 元类别含 no-data-race 的：`C.Concurrency`、`C.Overall`、`C.FalseOverall`、`C.TrueOverall`、`C.Huawei-Concurrency-Challenges`。
- opt-in（不进 verifiers）是「只想跑 no-data-race、不想被 no-overflow/unreach-call/valid-memsafety 及 C.Overall 汇总」的正确机制——与提交页 §4 讲解一致，opt-out 不能规避罚分。

---

## 五、合格性（Qualification）—— 提交页 §三

| # | 条件 | 状态 |
|---|---|---|
| E1 | FM-Tools yml flags 正确 | ✅ competition_participations / techniques / frameworks_solvers / literature / jury member 已补齐 |
| E2 | 归档含合格 LICENSE | ✅ NASA-1.3（无输出限制） |
| E3 | 与其他工具概念/实现差异（不能重复参赛） | ✅ 描述已写：IKOS 上游 + 自研 thread-modular + lockset/HB + BMC 重确认 |
| E4 | 贡献 jury member（或组织者批准） | ✅ 已定 Ronghui Ruan（ORCID/NUDT/China），邮箱已发 + 已回复报备（GitLab `ruanronghui`） |

---

## 六、关键日期（规则页日期）

| 日期 | 节点 | 关联 |
|---|---|---|
| 2026-10-08 | 注册 + 验证任务提交（B/C/D 三 MR） | ✅ 三个 MR/PR 均已提交，待合并确认 |
| 2026-10-20 | 工具提交截止（CI 全绿，DOI 定稿） | 🚧 **需重传新版 Zenodo 归档**（代码本次更新）+ 确认 fm-tools CI 绿 |
| 2026-11-03 | 验证任务冻结 | 类别名/任务数/ILP32-LP64 以此为最终 |
| 2026-11-10 / 11-17 | 合格通知 / 最终重提交 | |
| 2026-12-01 / 12-04 | 离线评测完成 / 结果公开 | |
| 2026-12-17 | 参赛短文（可选，赛后） | |

---

## 七、风险与未验证项（G）

| # | 风险 | 代码/证据 | 状态 |
|---|---|---|---|
| G1 | 计分：FALSE 错=−16、TRUE 错=−32、UNKNOWN=0 | 规则 §9 明确；`--demote-race-to-unknown` + BMC 重确认；**实测冻结集 TP=85/FP=0/TN=709/FN=0 = 1503 分** | ✅ |
| G2 | ubuntu:24.04 干净容器跑通 smoketest | `required_ubuntu_packages` 已列（含 libz3-4），但未在干净容器实测 | 🚧 |
| G3 | data_model ILP32/LP64 匹配 | `svcomp_witness.py`：`--data-model` → `-m 32/64`，witness 头部写 `data_model`；实测全量 ILP32 | ✅ |
| G4 | witness 见证被 validator 验收 | 见证 2.2 已用 CPAchecker 复核：FALSE 见证被确认（正确调用见 `ikos-bmc-design.md` §7/记忆） | ✅ |
| G5 | 归档/仓库公开可访问 | Zenodo 已上传（DOI `10.5281/zenodo.23122650`）；GitHub `github.com/2393591260/ikos-conc` | ⚠️ 归档需重传新版 |

---

## 八、本次代码实查的**新增结论**（四份翻译文档里没有的，原文见 `docs/archive/`）

1. **`fm-tools-entry.yml` 缺 `competition_participations`（参赛声明+jury member）**，且 jury member 位置错（应在 `competition_participations[].jury_member`，非仅 `maintainers`）。这是 B3/B4/E4 的硬缺口，也是 E1「flags 正确」的隐性失败点。
2. **`fm-tools-entry.yml` 缺 `techniques` / `frameworks_solvers` / `literature`** 三个格式 2.0 字段（goblint.yml 有）。
3. **`install/output.db` 残留会被 `package.py` 打进 zip**，违反「不含不必要数据」；package.py 无显式排除守卫。
4. **判定是三条线不是两条**：`definitely UNSAFE`(FALSE) / `UNKNOWN`(⊤ 模型边界) / `potentially UNSAFE`(demote 未确认) 三者区分由 `report.py::_is_unknown_check` 靠 info.verdict 标记实现；tool-info 把后两者都映射 UNKNOWN——这是「demote 不误报、也不漏报」的代码机制。
5. **`C.no-data-race.Huawei-Concurrency-Challenges` 在 demo_categories 里**（线上核实），opt-in 它是否计分需在 MR 前再确认（demo 类别可能不正式计分）。

---

## 九、待办排序（按截止日，2026-10-05 更新）

1. **确认三个 MR/PR 合并**（10-08 注册截止）：fm-tools MR（之前 CI 卡 no-matching-runners，字段补齐后确认变绿）+ bench-defs category-structure MR + BenchExec tool-info PR。均已提交，等维护者合并。
2. **加入 Zulip**：Dirk 已发邀请，jury 通讯/后续协调在这里。
3. **重传 Zenodo 归档**（10-20 前）：本次代码更新了 helper 内联 + cast 修复（1476→1503），需 `python3 svcomp/package.py` 重打包并上传到 DOI `10.5281/zenodo.23122650`（同 DOI 更新版本）。
4. **干净容器 smoketest**（G2，10-20 前）：ubuntu:24.04 容器实测 `install/smoketest.sh`。
5. **持续**：仓库保持 public；`C.no-data-race.Huawei-Concurrency-Challenges` 是否计分在 MR 合并前确认。
