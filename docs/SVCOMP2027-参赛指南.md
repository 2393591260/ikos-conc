# SV-COMP 2027 参赛指南

本文统一维护注册、归档、runner 与 witness 验收。**外部事实最后核对于 2026-10-09**；
本次文档整理没有重新访问官网、远端 MR/PR 或 Zenodo。外部要求和状态须在操作前重新确认。
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

2026-10-09 的页面记录：注册 MR 创建截止 **10 月 12 日**，工具提交/CI 完成 **10 月 20 日**，
任务冻结 11 月 3 日，合格通知 11 月 10 日，最终重提交 11 月 17 日；
结果预计 12 月 4 日公布，参赛短文节点 12 月 17 日。旧报告中的 10 月 8 日注册截止已过时。
短文提交要求以组织者后续通知为准，不沿用旧文档“必需/可选”的笼统判断。

当时规则表：no-data-race 不支持 correctness witness；violation witness 为 **2.2 或更高**，
文件名 `witness.yml`。其他性质的 witness 要求不同。见证资源表曾列 2 处理单元/7 GB、
违例验证 90 秒/正确性验证 300 秒；正式运行时从当版规则和 benchmark XML 核对，
不与本地 analyzer 的 30 秒实验预算混用。

<a id="registration"></a>
## 2. 注册和本地集成

需要工具 ZIP/Zenodo DOI、FM-Tools 工具条目与参赛声明、BenchExec tool-info。
当时 submission 页面已不要求另开 category-structure MR；类别仍需声明，拟迁入 FM-Tools schema。

| 提交 | 2026-10-09 的观测 | 下一次须核对 |
| --- | --- | --- |
| [FM-Tools !1147](https://gitlab.com/sosy-lab/benchmarking/fm-tools/-/merge_requests/1147) | opened，最近 pipeline failed，`ci_must_pass` | 失败 job 的实际原因、权限/runner、最新 schema、是否合并 |
| [BenchExec #1324](https://github.com/sosy-lab/benchexec/pull/1324) | open，15 checks passed，无 review | PR 内容是否与本地相同、集成测试、review/merge |
| [schema !1112](https://gitlab.com/sosy-lab/benchmarking/fm-tools/-/merge_requests/1112) | opened | 今日是否合并或有组织者迁移安排；不能把旧状态当今日事实 |
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

新 jury member 必须具有贡献身份并向组织者报备。用户已邮件回复 GitLab 用户名、再次跟进，
当时等待组织者确认；本文不推断现在已获得 runner 权限。
可在 MR 的 pipeline/job 查看是否存在可启动任务、matching runner 与失败日志；
无法操作时通过已加入的 Zulip/邮件询问。仅加入 GitLab 项目不等于拥有所有 CI runner 权限。

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
4. 用声明的 Ubuntu 24.04 包构建干净运行环境，确认断网和依赖/ABI；工具不能回读开发仓库。
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

- [ ] 官方截止、类别/schema、资格和 jury/runner 权限再次核对。
- [ ] 本地 FM 条目与远端一致；MR pipeline 原因明确并通过；BenchExec PR 内容一致且合并。
- [ ] 当前源码构建/install hash 一致；语义回归、AI+BMC 配对、新 FALSE 执行检查有版本化证据。
- [ ] 新 ZIP 指纹、BUILD-INFO、组件许可、真实入口/P0/双架构/重定位/干净依赖检查通过。
- [ ] adapter 的 bin 定位、搬运路径、属性限制和未知/超时/非零退出映射核对。
- [ ] witness 2.2 格式与严格实例绑定的源级验收完成；能力边界和未验证项明确。
- [ ] 用户替换/上传 ZIP 后公开 hash 核对，CI 重取新实物，提交及参赛资格得到确认。

本地标签分可参考 FALSE +1、TRUE +2、错 FALSE −16、错 TRUE −32、UNKNOWN 0 的历史规则；
最终得分还受 witness/类别和当版官方规则影响，不能把 README 标签计数直接叫比赛得分。

<a id="evidence"></a>
## 7. 证据与维护

- [提交复核原始页面/schema/MR/PR/实物指纹](artifacts/svcomp-submission-2026-10-09/)。
- [重封装、容器/重定位 smoke、P0、linter、旧 CPA 异常及现代源码参照](artifacts/svcomp-repackage-2026-10-09/)。
- [历史类别声明说明](../svcomp/category-structure-mr.md)及 [reference 导航](../svcomp/reference/README.md)
  仅用于解释既有材料；当前注册要求从本指南和当版官网核对。

旧指南、四篇注册快照及四篇官方全文翻译已合并必要信息后删除，原文可从 Git 历史获取。
不把全文翻译持续复制成规则依据；需要新的性质/赛道时直接核对原文。
