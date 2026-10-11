# SV-COMP 2027 参赛指南

本文统一维护注册、归档、runner 与 witness 验收。**官网日期及部分 GitLab 状态于 2026-10-11 复核**；
BenchExec PR、旧 ZIP 和历史 validator 记录保留各自实际核验日期，未当作今日验收。
外部要求和状态须在操作前重新确认。
本地分析能力与最新基线见 [阅读入口](README.md#baseline)，不能用旧 ZIP 的成功检查替代新版本验收。

<a id="requirements"></a>
## 1. 目标与官方入口

当前目标是 `C.no-data-race.Concurrency`，不是自动参加全部并发性质。
数据模型来自任务 YAML，不能一律采用宿主位宽。2027 任务集合未固定前，使用本地 2026 冻结集回归。
程序 → YAML（输入、属性、期望结果、数据模型）→ 类别任务集合，是评估口径；
不得自行把 `.i` 换成同名 `.c` 后仍声称测试了原任务。

| 官方入口 | 用途 |
| --- | --- |
| [主页与日期](https://sv-comp.sosy-lab.org/2027/) | 最新期限和公告 |
| [submission](https://sv-comp.sosy-lab.org/2027/submission.php) | 当前注册/归档/集成流程 |
| [rules](https://sv-comp.sosy-lab.org/2027/rules.php) | 性质、资源、判定和评分规则 |
| [benchmarks](https://sv-comp.sosy-lab.org/2027/benchmarks.php) | 任务类别、训练集合与约定 |
| [BenchExec 集成说明](https://github.com/sosy-lab/benchexec/blob/main/doc/tool-integration.md) | tool-info 与本地测试 |
| [FAQ](https://sv-comp.sosy-lab.org/faq.txt) | 输出、版本、复现及见证核验 |

2026-10-11 官网确认：注册 MR 创建截止 **10 月 12 日**，工具提交/CI 完成 **10 月 20 日**，
任务冻结 11 月 3 日，合格通知 11 月 10 日，最终重提交 11 月 17 日；
12 月 1 日向参与者通知结果、12 月 4 日公开，参赛贡献论文节点 12 月 17 日。
旧报告中的 10 月 8 日注册截止已过时；网页未在该日期表中明确截止时区，操作前须核对。
短文提交要求以组织者后续通知为准，不沿用旧文档“必需/可选”的笼统判断。

本轮规则页核对：no-data-race 不支持 correctness witness；violation witness 为 **2.2 或更高**，
文件名 `witness.yml`。其他性质的 witness 要求不同。见证资源表曾列 2 处理单元/7 GB、
违例验证 90 秒/正确性验证 300 秒；正式运行时从当版规则和 benchmark XML 核对，
不与本地 analyzer 的 30 秒实验预算混用。

<a id="registration"></a>
## 2. 注册和本地集成

需要工具 ZIP/Zenodo DOI、FM-Tools 工具条目与参赛声明、BenchExec tool-info。
submission 页面已不要求另开 category-structure MR；类别仍需在 FM-Tools 的 CompetitionTrack 中声明。

| 提交 | 最近实际观测 | 下一次须核对 |
| --- | --- | --- |
| [FM-Tools !1147](https://gitlab.com/sosy-lab/benchmarking/fm-tools/-/merge_requests/1147) | 10/11：opened；失败 jobs 为 `stuck_pending_no_matching_runners`，smoke 跳过 | runner 恢复后的实际日志、是否合并；当前不能判为工具失败或通过 |
| [BenchExec #1324](https://github.com/sosy-lab/benchexec/pull/1324) | 10/9：open，15 checks passed，无 review | PR 内容是否与本地相同、集成测试、review/merge |
| [schema !1112](https://gitlab.com/sosy-lab/benchmarking/fm-tools/-/merge_requests/1112) | 已于 UTC 10/10 20:46 合并（上海 10/11 04:46） | 用合并后的 schema 校验本地和远端；不再等待该 MR |
| category-structure !677 | 当时未取得其状态 | 已有 MR 是否需调整由组织者确认；它不再是当时页面必需的新提交 |

本地配置入口：

- [fm-tools-entry.yml](../svcomp/fm-tools-entry.yml)：当前有 `track: Verification`、
  `categories: C.no-data-race.Concurrency`，描述 AI + 有界 Z3 确认，不宣称普遍 FP/FN 为零。
  字段能写入本地不证明远端 schema/CI 已接受。
- [ikos-conc.py](../benchexec/tools/ikos-conc.py)：从任务传递数据模型，运行 bundled
  `svcomp_witness.py`，映射 SAFE/definitely UNSAFE/UNKNOWN，并拒绝非零退出或超时作为正常结果。
  当前 `find_executable("ikos")` 未显式指定 `bin`；归档根定位须实测。
  当前未明确拒绝其他属性；只注册 no-data-race 不等于适配器具备通用属性支持。
- [打包脚本](../svcomp/package.py)、[见证包装器](../svcomp_witness.py)、
  [本地运行模拟](../svcomp/bench-exec-sim.py)。模拟不能替代官方 runner。

`required_ubuntu_packages` 是系统包安装声明；`program_files()`/`REQUIRED_PATHS` 是工具搬运清单。
不需要把整个 `/usr/lib` 放进归档。不同现存 adapter 的清单不同：CPAchecker 明列 bin/JAR/native/config，
Goblint/ESBMC 当时没有额外清单。IKOS 只返回入口在按 program_files 搬运时可能不足；
完整 FM 容器内运行是否失败则须实际检查。自带 APRON、LLVM/Z3 与系统库要保持 ABI/版本一致。

新 jury member 必须具有贡献身份并向组织者报备。用户已邮件回复 GitLab 用户名。
Jan Strejček 的 10/9 回复明确：**10/12 前所需注册动作已完成；Dirk 将处理 CI；下一节点是 10/20 前 MR 合并。**
不重复注册，也不把当前无 matching runner 直接归因用户设置。
官方 CI、合并与资格分别记录；仅加入 GitLab 项目不等于拥有所有 CI runner 权限。

<a id="package"></a>
## 3. 工具归档：历史实物与新版本边界

Zenodo 记录：[23122650](https://zenodo.org/records/23122650)，concept DOI 为 23122649。
以下都是 **2026-10-09 保存的实物证据**，不是当前分析器发布凭证。

| 实物 | 指纹与结论 |
| --- | --- |
| 当时公开旧 ZIP | 95,356,370 bytes，MD5 `232bc0f5ab3d0b710c2752e3d858c428`；解压后仍复现 exclusive-flag FP、published-before-init FN、overlapping-widths FN（双架构）。 |
| 本地重封装 ZIP | 149,398,460 bytes，SHA256 `b06a6b850bed78562626d21f5c8d008b5ea35409308bf6b82830a3d7a2856b34`，MD5 `87a17d9490aab3490f10a7951ecd516b`。 |
| 重封装 analyzer | SHA256 `3043999b0e36620f977aeb8d397afeb3921bc4d445d78df5945eb80ccbd8424c`；不是 README 当前二进制。 |

当时重封装核对：单顶层目录、无检查到的 git/cache/db/cpp 残留；项目外换 cwd/路径运行；
正式 wrapper 的 race/safe × ILP32/LP64 smoke；全新 Ubuntu 24.04 仅声明包与 unzip；
P0 双架构 20 条观察与修复基线一致。退出码非零不能靠输出 unsafe 字符串通过 smoke。
包内补了 BUILD-INFO、修改说明和自带组件许可；源码补充 tar.gz 不是 FM 的执行 ZIP。
该轮没有重新跑全量，沿用同 analyzer hash 的先前实验，不能说完成了最新 109 TP 版本的包装验收。

Zenodo [manage-files](https://help.zenodo.org/docs/deposit/manage-files/#modify)
当时允许发布后 30 天内通过 Edit files 做 minor corrections，保留同一记录 DOI；新建版本产生新版本 DOI。
不要自动创建/上传版本。替换后核对公开文件 hash，并要求 CI 使用新实物；同 DOI 可能还有旧缓存。
本轮没有发布或修改远端。

新发布归档应满足：单一顶层目录；合格 LICENSE、README、smoketest；支持 --version；
必要工具组件/许可完整；任意路径、断网可运行；stdout/stderr 控制在官方上限内（当时 2 MB）；
排除数据库、源码补丁、缓存和无关大测试。BUILD-INFO 记录源码、二进制、配置和组件指纹。

<a id="runner"></a>
## 4. 干净环境与官方运行闭环

1. 固定候选源码提交，构建并同步 install；先完成语义回归/冻结集配对/所选执行检查。
2. 打包到新的暂存目录，记录 ZIP/analyzer/前端 hash；不要用旧包覆盖验收记录。
3. 在项目外解压，不同 cwd、不同绝对路径运行正式入口 smoke/P0/双架构见证生成。
4. 用声明的基础镜像/系统包构建干净运行环境，确认断网和依赖/ABI；工具不能回读开发仓库。
   当前条目为 Ubuntu 24.04；会议纪要计划官方运行主机为 26.04。核实 host/container 的实际组合，
   测试兼容性，不能仅替换版本字段就宣布支持。
5. 安装对应 BenchExec 与此 adapter，从任务 YAML 生成固定 manifest/XML，传入属性及数据模型，
   保存 command line、资源/退出码/判定及 witness；缺失属性文件须来自权威版本，不能自造评分任务。
6. 在可用的正式 Linux cgroup/container 隔离下运行所选任务及全量，核对资源、文件白名单和输出限制。
7. 对 violation witness 做格式和独立源级语义核验；最后提交远端并检查实际 CI 实物和日志。

Ubuntu VM 只能验证安装/执行环境，**不自动等于官方隔离与资源 enforcement**。
`--no-container` 或本地模拟适合诊断，须在结论里标明；不要据此宣布官方 runner 通过。
XML 使用 BenchExec 当前 schema 与任务文件列表；不要沿用旧报告里未经核验的容器豁免。
官方 benchmark-defs、训练结果输出包含版本、运行命令、任务和 validator 信息，
这些用于定位“用了哪个包/参数/见证”，比只看总分可靠。

<a id="witness"></a>
## 5. Witness 2.2 与三层验证

正式格式：[SV-Witnesses 2.2](https://zenodo.org/records/21993990)，2026-08-18 发布；
当时下载 ZIP MD5 `a1339c68eade4f5a194f67dc2342a1fd`。
本地 2.2 生成器在多线程创建及多 target 上仍须严格核对实际实例与源位置。
当前 `build_witness` 按入口函数名及源顺序创建列表分配 ID，同入口取 `created[:2]`；
嵌套/重复创建的实际动态实例需与 BMC 导出证据核对，不能直接用函数名猜测。
旧包装修正省略有争议的 function_enter 列号，不等于多行调用、宏、`.i`→`.c` 映射都正确。

| 层次 | 能证明什么 | 已保存的限制 |
| --- | --- | --- |
| WitnessLint 格式检查 | schema/结构与部分源位置约束 | 历史 smoke 默认模式通过；strict 模式报 `Unknown waypoint type target`，正式 release strict AST 路径缺 target 分支。不能称 strict 通过。 |
| IKOS 独立 AR 执行检查 | 在导出 AR、明确 API 合同下重新执行动态实例、值源、字节地址和同步，检验所选竞争 | 当前执行记录见 [BMC 手册](ikos-bmc-complete.md#validation)；它不直接验证最终 witness/source 映射。 |
| 独立源级 witness validator | 按输入源码、witness 及实际配置重建被指定实例/约束限定的竞争 | 旧 GraphML validator 接受损坏线程 ID 的校准失败；本地 CPAchecker 4.2.2 对 2.2 解析异常。不能称最终官方验收已通过。 |

[verification training](https://sv-comp.sosy-lab.org/2027/results/results-verified/)、
[validation training](https://sv-comp.sosy-lab.org/2027/results/results-validated/)、
[当时 race 验证表](https://sv-comp.sosy-lab.org/2027/results/results-validated/validation-of-violation-witnesses-v1.C.no-data-race.Concurrency.table.html)
是 preliminary 运行。当时表实际使用 GraphML v1、CPAchecker 4.2.2/Dartagnan 4.3.1/UAutomizer 0.3.1；
不能据此确定 2027 最终 2.2 validator。

CPAchecker 开发源码固定 `bd91e2fd933ac2704dc240089774db4e5ec877da` 可见 2.2 版本识别、
创建 waypoint/thread ID/multi-target 解析、`validate_data_race_concurrency` 集成测试及
`violation-witness-validation-datarace.properties` 配置。这是源码证据，当时没有构建运行该开发版。
下一次验收固定一个实际支持功能的可执行版本，先做线程 ID/目标/值约束损坏的负校准，
再测真实输出，并与最终官方配置对齐。Dartagnan 另找一个 race 不自动验证 IKOS 的具体 witness。

<a id="checklist"></a>
## 6. 下一次发布检查表

以下未勾选项针对**下一次发布**，不否认历史局部检查。自动化成功和组织者接受分别记录。

- [ ] 合并后的 schema、本地/远端类别、官方 CI 和资格状态分别核对；注册动作已完成。
- [ ] 本地 FM 条目与远端一致；MR pipeline 原因明确并通过；BenchExec PR 内容一致且合并。
- [ ] 当前源码构建/install hash 一致；语义回归、AI+BMC 配对、新 FALSE 执行检查有版本化证据。
- [ ] 新 ZIP 指纹、BUILD-INFO、组件许可、真实入口/P0/双架构/重定位/干净依赖检查通过。
- [ ] adapter 的 bin 定位、搬运路径、属性限制和未知/超时/非零退出映射核对。
- [ ] witness 2.2 格式与严格实例绑定的源级验收完成；能力边界和未验证项明确。
- [ ] 用户替换/上传 ZIP 后公开 hash 核对，CI 重取新实物，提交及参赛资格得到确认。

本地标签分可参考 FALSE +1、TRUE +2、错 FALSE −16、错 TRUE −32、UNKNOWN 0 的历史规则；
最终得分还受 witness/类别和当版官方规则影响，不能把 README 标签计数直接叫比赛得分。

<a id="schedule"></a>
## 7. 参赛日程与发布出口

整体路线的 M0–M7 见 [README](README.md#roadmap)。以下内部日期是建议，官方日期来自 10/11 官网。
本地退出与官方退出分开：外部 runner/validator 未就绪不等于通过，也不阻塞可以独立完成的工程工作。

| 时间 | 工作/产物 | 退出条件 |
| --- | --- | --- |
| 10/11–15（内部） | 新 schema 与远端差异、adapter 定位/搬运/属性/退出映射、当前二进制的新包 | 实际入口、依赖/ABI、重定位与断网 smoke 有记录；不是旧包验收的沿用 |
| 10/12–17（内部） | 固定可运行的 2.2 validator；实例/源位置映射检查和正负校准 | 正例接受；影响指定目标的坏实例、目标或约束被拒；真实输出逐项记录 ACCEPT/REJECT/TIMEOUT/unsupported，不混淆 AR 与源验证 |
| **10/18（内部冻结）** | 固定首个候选源码、install、ZIP、BUILD-INFO 和证据 | 可控验收完成；源 validator 不可用则保留明确未验收项，已知错误映射修复或保守降级；随后仅纳入经验证的阻断修复 |
| 10/18–19（内部缓冲） | 供用户上传的实物和登记内容；上传后公开 hash/CI 获取包核对 | 登记和公开实物一致，同 DOI 的旧缓存得到排除；内部 freeze 不是官方接受 |
| **10/20（官方 M1）** | 工具提交，CI checks 全部成功；按 Jan 邮件完成 MR 合并 | 查看实际日志和 merged 状态；runner 未恢复则如实列为外部未决，不以本地模拟替代 |
| 10/21–11/2 | 官方预跑问题归因；择入完整验收的 M2/M3 机制 | 先处理错误判定、见证和环境，再处理 TN/TP；每批单机制提交，保留可用候选包 |
| **11/3（官方）** | 任务冻结；固定 2027 revision/manifest/YAML/属性/数据模型 | 建立正式目标基线，2026 回归与 2027 目标分别记录；这是任务冻结，不是工具最终冻结 |
| **11/10（官方）** | 合格工具通知 | 资格以组织者通知为准，本地最终候选准备继续进行 |
| **11/15（内部冻结）** | M4 最终候选包及同包验收 | 固定源码、前端/分析器/ZIP hash；只处理经验证的最终提交阻断问题 |
| **11/17（官方 M4）** | 最终重提交，正式评估开始 | 公开实物、登记记录、官方收取实物一致；不再改已提交 ZIP |
| 12/1、12/4、**12/17（官方）** | 参与者结果、公开结果、参赛贡献论文 | 按官方验证结果汇报；研究论文另按新颖性/证据出口选场所和期限 |

会议纪要关于 hash 注册、malloc 失败和语言元数据的事项，按实现后的官方 schema/规则检查；
已获取页面未确认独立 archive-hash YAML 字段，不凭纪要发明配置键。
远端上传、邮件和 MR/PR 写入由用户操作或另行明确授权；本地先准备可审查实物。

<a id="evidence"></a>
## 8. 证据与维护

- [提交复核原始页面/schema/MR/PR/实物指纹](artifacts/svcomp-submission-2026-10-09/)。
- [重封装、容器/重定位 smoke、P0、linter、旧 CPA 异常及现代源码参照](artifacts/svcomp-repackage-2026-10-09/)。
- [历史类别声明说明](../svcomp/category-structure-mr.md)及 [reference 导航](../svcomp/reference/README.md)
  仅用于解释既有材料；当前注册要求从本指南和当版官网核对。

旧指南、四篇注册快照及四篇官方全文翻译已合并必要信息后删除，原文可从 Git 历史获取。
不把全文翻译持续复制成规则依据；需要新的性质/赛道时直接核对原文。
