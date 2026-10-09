# SV-COMP 提交参考文件（对照模板）

> 2026-10-09 复核发现：字段与参考工具一致不足以证明提交合规；schema 类别迁移尚未完成，adapter 的定位/依赖清单也有实查缺口。下文是历史核对，最新依据见 [提交复核](../../docs/SVCOMP2027-提交复核-2026-10-09.md)。

本目录存放我们从官方仓库拉下来的「格式参考」，用来逐字段核对我们的提交物。
**这些都是上游官方文件，不是我们的代码**，仅供本地 diff 用。

| 文件 | 上游来源（2026-10-05 拉取 main 分支） | 用来核对 |
|---|---|---|
| `goblint.yml` | `gitlab.com/sosy-lab/benchmarking/fm-tools` → `data/goblint.yml` | `../fm-tools-entry.yml`（FM-Tools 格式 2.0） |
| `template.py` | `github.com/sosy-lab/benchexec` → `benchexec/tools/template.py`（BaseTool2） | `../../benchexec/tools/ikos-conc.py`（tool-info 模块） |

## 核对结论（2026-10-05）

- `fm-tools-entry.yml`：16 个顶层字段与 goblint.yml 完全一致；`benchexec_toolinfo_options: []`
  空列表正确（我们无需额外选项）；无 `label` = 默认 active（参赛），goblint 的 2027 条目
  是 `label: [inactive]`（hors-concours 陪跑），两者都对。
- `ikos-conc.py`：覆写 BaseTool2 的 5 个方法（name/executable/version/cmdline/determine_result），
  签名逐字一致；`project_url/environment/program_files/working_directory/get_value_from_output/close`
  6 个 OPTIONAL 方法用默认，一个不多一个不少。

> 上游文件会更新，若以后要重新核对，重新拉取覆盖即可。
