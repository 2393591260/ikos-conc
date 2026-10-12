# SV-COMP 2027 参赛指南

本文统一维护注册、归档、runner 与 witness 验收。**官网日期及部分 GitLab 状态于 2026-10-11 复核**；
2026-10-12 已按 BenchExec PR 审查修改本地接口并验收新候选；远端合并/上传状态仍须单独核对。
旧 ZIP 和历史 validator 保留各自日期，未当作今日验收。
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
| [BenchExec #1324](https://github.com/sosy-lab/benchexec/pull/1324) | 10/12 API：open，head `2393591260/benchexec:main`，`f9e49429`；收到接口/可移植性审查 | 本地修改与新 ZIP 已准备，须更新现有 fork 分支、Zenodo 文件并重新检查 CI/review；本地通过不等于远端接受 |
| [schema !1112](https://gitlab.com/sosy-lab/benchmarking/fm-tools/-/merge_requests/1112) | 已于 UTC 10/10 20:46 合并（上海 10/11 04:46） | 用合并后的 schema 校验本地和远端；不再等待该 MR |
| category-structure !677 | 当时未取得其状态 | 已有 MR 是否需调整由组织者确认；它不再是当时页面必需的新提交 |

本地配置入口：

- [fm-tools-entry.yml](../svcomp/fm-tools-entry.yml)：当前有 `track: Verification`、
  `categories: C.no-data-race.Concurrency`，描述 AI + 有界 Z3 确认，不宣称普遍 FP/FN 为零。
  字段能写入本地不证明远端 schema/CI 已接受。
- [ikos-conc.py](../benchexec/tools/ikos-conc.py)：从任务传递数据模型，运行 bundled
  `bin/svcomp_witness.py`，由 `executable()` 指定实际入口，直接按 shebang 执行。
  10/12 审查修复：版本查询使用 `_version_from_tool`；数据模型使用标准 task utility，
  没有任务模型时 adapter 不添加 ILP32；属性交给包装器校验；非零退出/信号为 ERROR，
  timeout 由 BenchExec 框架统一处理；判定按完整输出行匹配，明确竞争为 `false(no-data-race)`。
  去掉 standalone/fallback/正则和误导注册注释；搬运 bin/lib/libexec/llvm/include 保持。
- [打包脚本](../svcomp/package.py)、[见证包装器](../svcomp_witness.py)、
  [本地接口回归](../svcomp/bench-exec-sim.py)。它实际调用 Tool.cmdline/determine_result，逐任务隔离输出并区分 UNKNOWN、超时和非零退出；没有 cgroup 资源计量，不能替代官方 runner。

`required_ubuntu_packages` 是系统包安装声明；`program_files()`/`REQUIRED_PATHS` 是工具搬运清单。
不需要把整个 `/usr/lib` 放进归档。不同现存 adapter 的清单不同：CPAchecker 明列 bin/JAR/native/config，
Goblint/ESBMC 当时没有额外清单。IKOS 只返回入口在按 program_files 搬运时可能不足；
完整 FM 容器内运行是否失败则须实际检查。自带 APRON、LLVM/Z3 与系统库要保持 ABI/版本一致。

**10/12 当前接口验收：** 8 组 adapter 与 18 组 witness 回归通过；
Ruff 0.16.10 使用 BenchExec main 的配置检查与格式检查通过，BenchExec 3.35 的
`test_tool_info` 以相对包路径运行，通过版本/命令/搬运清单检查。
打包入口自动定位同目录 IKOS，有 executable bit、`--version` 与 `--property`；
包装器仍拒绝非 no-data-race 属性和非 C `.c/.i` 输入，不在 adapter 复制这套语义。
新入口的 1029 项全量与当前 M2 二进制逐任务一致：TP=109、TN=687、FP/FN=0、
UNKNOWN=232、timeout=1；全部 109 个标签竞争结果生成 witness，但这不是独立源见证验收。
双架构包入口 P0 10 条与已接受 BMC 观察一致，离线/只读 Ubuntu 24.04 与含空格迁移路径
race/safe smoke 通过。记录见
[PR 接口验收](artifacts/svcomp-pr1324-2026-10-12/summary.json)与
[逐任务配对](artifacts/svcomp-pr1324-2026-10-12/packaged-paired.json)。
全量后仅调整 adapter 的 import 排序与 Ruff 格式；独立 import 集与其余 AST 一致，
保留运行时 adapter 源码指纹，最终版本的单测及官方接口测试重新检查。

以下保留 **10/11 M1 历史候选** 的验收，不能作为 10/12 新归档的身份记录。
本地 adapter 的真实 BenchExec 接口测试 5 组通过，`test_tool_info` 确认包根定位与搬运路径；
工具源码/安装 Python 一致，构建与 install 同步后两个模式的冻结集结果逐项复现既有基线。
依据见 [M1 直接分析配对](artifacts/svcomp-m1-2026-10-11/direct-baseline.json)、
[双架构 P0 观察](artifacts/svcomp-m1-2026-10-11/p0-observations.json)。
固定的新验证包也已通过真实 Tool API 的 1029 项回归，与直接分析逐项一致：TP=109、TN=678、UNKNOWN=241、1 项超时，FP/FN=0。
依据：[包入口全量](artifacts/svcomp-m1-2026-10-11/packaged-baseline.json)、[逐任务](artifacts/svcomp-m1-2026-10-11/packaged-paired.json)。
验证包支持含空格路径和项目外 cwd；仅声明依赖的新 Ubuntu 24.04 断网/只读容器通过双架构 smoke/P0。
最终交付 ZIP 的源码身份和同包检查见 §3；源 witness 与官方 CI 仍分别待验收。

新 jury member 必须具有贡献身份并向组织者报备。用户已邮件回复 GitLab 用户名。
Jan Strejček 的 10/9 回复明确：**10/12 前所需注册动作已完成；Dirk 将处理 CI；下一节点是 10/20 前 MR 合并。**
不重复注册，也不把当前无 matching runner 直接归因用户设置。
官方 CI、合并与资格分别记录；仅加入 GitLab 项目不等于拥有所有 CI runner 权限。

<a id="package"></a>
## 3. 工具归档：当前候选与历史实物

**10/11 M1 历史候选保留不覆盖。** 10/12 PR 修复新候选的归档指纹在本节新增记录中；
用户之前的上传不代替新包校验，也没有在本轮自动修改远端。
归档：[ikos-conc-m1-20261011.zip](/home/ruan/ikos-conc-m1-20261011.zip)，大小 `160292014` bytes，
SHA256 `9bbef705b69a65d287d336fbe4699e3a3c498316a0a68dd95de3633dc18864a4`，源码提交 `7e5ed7a`。
BUILD-INFO 记录打包时干净的已跟踪源码、384 个运行文件及条目/adapter/package 指纹；
用户其他未跟踪文件不属于该实物，不据此声称工作区完全无文件。
与1029项实际验证包相比，运行文件及权限逐项相同；差异限于身份记录与补齐的Pygments许可。
Pygments 2.19.2 的338个自带 Python 文件与官方wheel逐字节一致，许可来自该wheel。
最终 ZIP 重新解压后通过项目外含空格路径、断网/只读 Ubuntu 24.04 的双架构 race/safe smoke 和20条P0观察；
13个动态二进制/库无未解析依赖。真实BenchExec容器收取了`witness.yml`，但本机无CPU/内存cgroup权限，
不能称官方资源验收通过。
证据：[归档与BUILD-INFO](artifacts/svcomp-m1-2026-10-11/final-package.json)、
[运行/依赖/隔离检查](artifacts/svcomp-m1-2026-10-11/runtime-acceptance.json)、
[同包P0观察](artifacts/svcomp-m1-2026-10-11/final-p0-observations.json)。

旧的项目内ZIP未覆盖。上传时使用上述新文件，同步现有MR/PR及公开源码，
再核对Zenodo公开hash和CI实际取用的包。严格源级witness核验未通过的原因见§5，不能被本地标签结果替代。

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

网页上传中断时，可以在原记录的文件编辑草稿使用
[API 上传脚本](../svcomp/zenodo_upload.py)。2026-10-12 核对的官方说明仍要求
在发布后 30 天内开启 Edit published files，编辑草稿在原发布后 45 天内重新发布，DOI 不变。
脚本依据 [InvenioRDM draft/files API](https://inveniordm.docs.cern.ch/reference/rest_api_drafts_records/)，
先核对原记录/草稿 ID 与 DOI，再初始化文件、PUT 内容、commit 文件并核对远端大小/校验和；
不新建版本、不删除已有完成文件、不自动发布。
在 [Zenodo Applications](https://zenodo.org/account/settings/applications/) 创建带 `deposit:write`
权限的 personal access token，在本机终端执行：

```bash
python3 /home/ruan/ikos-conc/svcomp/zenodo_upload.py \
  --record 23122650 \
  --file /home/ruan/ikos-conc-m1-20261011.zip \
  --filename ikos-conc.zip
```

token 在脚本的隐藏输入提示中粘贴，不写入源码、命令或聊天。
显示远端 checksum 验证通过后，在原上传页面检查并重新发布；公开实物仍需复核。
脚本遇到临时连接/429/5xx 最多尝试三次，每次重传整文件，不承诺断点续传。
发送时显示百分比、平均速度和估计剩余秒数；100% 表示 HTTP 客户端读取了文件内容，
仍需服务端接受、commit 与远端校验才能认定上传完成。连接超时 20 秒、读响应超时 300 秒，
这些不是整个上传的总时限。已启动的旧进程不会自动获得新的进度显示。
当前七项离线协议/边界测试通过；尚无已完成认证 Zenodo 上传的实测，403/404 或草稿权限问题
需要依据实际返回排查，不能据此改用新版本或另建记录。

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
10/11 修复生成器：从 `info.counterexample` 的动态 trace、`creator_trace/creator_step` 和时钟顺序
建立注册与目标实例；包括此前已执行的非候选线程创建，不按入口函数名或源文件顺序猜 ID。
`thread_id` 位于 **waypoint 层**，按固定 release 中第 k 个注册 waypoint 编号；创建 waypoint 属于创建者。
复用实际 IKOS 的 Clang 配置/数据模型，用 AST 提取创建调用右括号和目标 full expression 的精确位置，
将 debug 的 presumed 位置映射回输入文件物理行/列；`.i` 不改换为同名 `.c`。
无法证明的宏端点、聚合初始化、复杂 linemarker 或实例绑定保守返回 UNKNOWN，非零退出。
witness 原子写入成功后才转发 FALSE；清除旧 witness，报告/AST/写入失败不会泄露 FALSE。

18 组定向测试通过；固定代码重新导出既有 **109/109** 确认报告，全部通过固定 2.2 schema。
实际程序的主/子线程、同入口双实例和重复 helper 创建在两个 ABI 上生成 witness；互斥对照 SAFE，
预处理输入的目标落在物理行 1463。依据：[映射检查](artifacts/svcomp-m1-2026-10-11/witness-mapping.json)、
[逐任务实例](artifacts/svcomp-m1-2026-10-11/witness-instances.json)。
这些证明导出和格式行为；源码级 validator 校准与执行验证分别记录，不能由 schema 通过推断。

| 层次 | 能证明什么 | 已保存的限制 |
| --- | --- | --- |
| WitnessLint 格式检查 | schema/结构与部分源位置约束 | 历史 smoke 默认模式通过；strict 模式报 `Unknown waypoint type target`，正式 release strict AST 路径缺 target 分支。不能称 strict 通过。 |
| IKOS 独立 AR 执行检查 | 在导出 AR、明确 API 合同下重新执行动态实例、值源、字节地址和同步，检验所选竞争 | 当前执行记录见 [BMC 手册](ikos-bmc-complete.md#validation)；它不直接验证最终 witness/source 映射。 |
| 独立源级 witness validator | 按输入源码、witness 及实际配置重建被指定实例/约束限定的竞争 | 旧GraphML校准失败；本轮现代CPA开发版已构建运行，创建列号不兼容且接受非竞争目标，严格校准失败。109份原包witness均因创建位置解析受阻，源级接受数为0。 |

[verification training](https://sv-comp.sosy-lab.org/2027/results/results-verified/)、
[validation training](https://sv-comp.sosy-lab.org/2027/results/results-validated/)、
[当时 race 验证表](https://sv-comp.sosy-lab.org/2027/results/results-validated/validation-of-violation-witnesses-v1.C.no-data-race.Concurrency.table.html)
是 preliminary 运行。当时表实际使用 GraphML v1、CPAchecker 4.2.2/Dartagnan 4.3.1/UAutomizer 0.3.1；
不能据此确定 2027 最终 2.2 validator。

本轮已构建并运行CPAchecker开发源码 `bd91e2fd933ac2704dc240089774db4e5ec877da`，
Java21.0.12.1、MathSAT5.6.11。版本打印`4.2.2-unknown`来自无Git元数据的源码归档，
不能把它当成官方发布的4.2.2。4549个原始src/config文件与上游归档核对一致，未修改验证算法。
构建所用编译器launcher/依赖与命令保存在校准证据；这也不是最终官方指定配置。

| 实际校准 | validator结果 | 结论 |
| --- | --- | --- |
| 正式规范的创建右括号列号 | 无匹配CFAEdge，解析失败 | 固定规范要求`)`，此源码`AstCfaRelation.getColumnOfFunctionCallParenthesis`却计算`(` |
| 唯一创建调用所在行省略可选列号 | FALSE | 此最小正例能匹配；仅为校准副本，没有修改原包witness |
| 目标线程ID99 / 创建者ID99 / 矛盾约束`0` / 两个目标均属主线程 | TRUE | 这些损坏不能匹配见证；这里TRUE表示未找到见证约束的违例，不是IKOS的SAFE结论 |
| 将子线程目标从`shared=1`移到随后`return 0` | FALSE | 错目标仍接受，不能证明指定的两个访问构成竞争 |

最后一项的源码依据：`DataRaceTransferRelation`保留此前发现的`hasDataRace`；
`AutomatonWitnessViolationV2Parser.handleIntermediateTarget/handleTarget`匹配位置与线程，
最终断言检查违例状态，没有把竞争访问身份严格绑定到两个目标。
固定2.2指南的no-data-race节要求最终两个target标记竞争访问，因此这项校准确实必要。
这不说明最小程序无竞争，也不直接说明IKOS的所选执行错误；它说明该validator不足以提供所需独立证明。

109份不可变验证包的**原始witness**均已实际尝试：109项`UNSUPPORTED_CREATE_LOCATION`，
严格源级接受0项；保留逐任务原因，不把解析失败记成见证被语义反驳。
没有为通过检查一律删除创建列号：默认列指第一个匹配位置，多调用/同一行/多行时可能错误绑定。
证据：[7项正负校准、最小源码和完整见证](artifacts/svcomp-m1-2026-10-11/validator-calibration.json)、
[109项实际源验证](artifacts/svcomp-m1-2026-10-11/validator-source-results.json)。
实际命令使用`--violation-witness-validation --spec <no-data-race.prp> --witness <witness.yml> --32`，
由根配置路由到`violation-witness-validation-datarace.properties`，不只指定后者而遗漏路由选项。

下一外部条件是组织者确认最终2.2 validator/版本/配置，并修复上述位置与目标绑定限制；
新版本先复测校准，再验证真实输出。必要时另评估独立源级检查，不能通过降低校准标准制造通过。
Dartagnan另找一个race也不能替代对IKOS具体witness的核验。

<a id="checklist"></a>
## 6. 下一次发布检查表

以下针对当前M1候选。勾选本地工程项不表示官方接受。

- [x] 合并后的FM schema校验本地条目；MR/PR与失败原因实际核对，注册动作已完成。
- [x] 当前构建/install/包中二进制hash一致；AI与AI+BMC基线和包入口全量逐项配对，既有AR执行证据保留。
- [x] 新ZIP/BUILD-INFO/组件许可/入口/P0/双架构/重定位/断网依赖检查完成。
- [x] 10/12 本地 adapter 入口、搬运、任务模型、完整行判定与 ERROR 映射检查完成；属性由包装器检查，超时由框架处理。
- [x] 109份witness通过固定2.2 schema；动态实例/源位置定向检查和现代validator校准有证据。
- [ ] 严格绑定指定访问对的独立源级witness验收：当前validator校准失败且原包创建列号解析受阻。
- [ ] 用户同步现有FM MR、BenchExec PR及公开源码，替换Zenodo ZIP后核对公开hash和CI重取实物。
- [ ] 官方runner实际成功、MR/PR合并及参赛资格确认；本地资源隔离不能替代官方验收。

本地候选已达到M1的可控工程冻结出口；**M1外部/源级验收出口仍未关闭**。
不为等待官方支持而宣称通过，也不在本轮加入M2/M3精度机制。
远端状态证据见[MR/PR与jobs](artifacts/svcomp-m1-2026-10-11/remote-status.json)。

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
- [M1包入口配对、格式/实例、现代validator校准与最终实物](artifacts/svcomp-m1-2026-10-11/)。
- [历史类别声明说明](../svcomp/category-structure-mr.md)及 [reference 导航](../svcomp/reference/README.md)
  仅用于解释既有材料；当前注册要求从本指南和当版官网核对。

旧指南、四篇注册快照及四篇官方全文翻译已合并必要信息后删除，原文可从 Git 历史获取。
不把全文翻译持续复制成规则依据；需要新的性质/赛道时直接核对原文。
