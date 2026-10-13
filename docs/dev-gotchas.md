# IKOS-ConC 开发速查

这份文档只收录执行流程和易错约定。语义依据见 [AI 手册](ikos-ai-complete.md)
和 [BMC 手册](ikos-bmc-complete.md)；当前数字只维护在 [阅读入口](README.md#baseline)。

<a id="workflow"></a>
## 1. 一轮修改怎样完成

1. 读一个具体程序，先推演线程、值源、对象范围和同步，再核对实际 AR。
2. 固定参考工具、输入、架构、配置；必要时用临时探针找出预期和实际的差异。
3. 在负责该语义的层实现通用机制，声明前提、近似方向、失败回退。
4. 验证最小正例、对照及改名/调用/宽度/控制流变体。标签 TP 不代替反例真实。
5. 构建并同步真实安装入口，运行相关语义回归和 AI/BMC 配对；新确认执行做独立 AR 检查。
6. 行为阶段通过基线验证后立即单独 commit，消息记录机制与前后配对数字。
   前端改动独立 A/B，检查 SAFE 影响；不要把多轮正确性和精度改动攒成一个大 diff。

保留小型 summary/transitions/执行结论与原理，原始大日志写入被忽略的目录。
不要提交二进制、数据库、全量日志或他人尚未提交的文件。纯文档修改只需核对事实和链接，
不要求无意义的全量重跑。

提交范围限于项目代码、构建/参赛配置、回归用例、维护手册及小型可复核证据。
Office 文档、私人资料、临时文件、原始日志和打包产物不入库；其他本地资料放在
被忽略的 `local/` 或 `docs/local/`。只对本轮相关文件使用显式 `git add <paths>`，
不要使用 `git add .` 或 `git add -A` 把他人文件混入提交；提交前核对
`git diff --cached --name-status`，push 前检查待推送提交的文件清单。
忽略规则只作用于未跟踪文件，已经提交的文件仍属于提交历史。

### 推理强度与难题分工

主会话模型由用户在 VS Code 扩展中选择；
代理不能自行切换主会话的模型或推理强度。
SAFE 保守性/执行保持证明、同步与干扰建模、影响不动点的架构取舍，
以及已有最小复现仍解释不了的矛盾，可作为最高强度攻关任务。
用户于 2026-10-13 选择暂用 Astra ultra，优先完成 M0–M7 的设计审查；
在用户另行通知前，主会话及委派沿用当前模型，不主动切回 Sol。
此前授权的 Sol 常规执行＋难题 ultra 分工留待恢复 Sol 时使用，不能覆盖最新选择。
委派提供具体代码、已有证据和证明义务；主会话核对结论后实施和验证，
不用子代理意见替代源码/反例/回归证据。构建、回归、打包不需要为其专门提高推理强度。

<a id="build"></a>
## 2. 构建与同步（从项目根目录执行）

```bash
cd /home/ruan/ikos-conc
cmake --build build --target ikos-analyzer -j"$(nproc)"
cp build/analyzer/ikos-analyzer install/bin/ikos-analyzer
sha256sum build/analyzer/ikos-analyzer install/bin/ikos-analyzer
```

`install/bin/ikos` 是 wrapper，实际使用安装目录的 analyzer 和 Python 模块。
修改 `analyzer/python/ikos/` 后同步对应文件到 wrapper 实际加载的位置
（本机为 `install/libexec/lib/python3.12/site-packages/ikos/`）；不能假定未来仍是 Python 3.12。
修改 LLVM 预处理 pass 时另构建 `ikos-pp` 并核对实际安装路径/前端管线。

```bash
# may 分析：potentially UNSAFE 是警告，并非已确认的真实竞争
install/bin/ikos --analyses=race --concurrency=auto file.c
# AI 安全能力：关闭 BMC 恢复
env -u IKOS_BMC_RECOVER install/bin/ikos --analyses=race --concurrency=auto --demote-race-to-unknown file.c
# AI + BMC
IKOS_BMC_RECOVER=1 install/bin/ikos --analyses=race --concurrency=auto --demote-race-to-unknown file.c
# 最初三个反例及控制，两个数据模型均须核对
python3 docs/review-repros/bmc-2026-10-08/run.py --bits 32
python3 docs/review-repros/bmc-2026-10-08/run.py --bits 64
# 冻结集；固定两端任务人口、jobs、timeout 和所有模式参数
IKOS_BMC_RECOVER=1 python3 svcomp_race_test.py --demote --jobs 4 --timeout 30
# 滚动集只作补充
IKOS_BMC_RECOVER=1 python3 svcomp_race_test.py /home/ruan/sv-benchmarks --demote --jobs 4 --timeout 30
```

`IKOS_BMC_RECOVER` 检查的是环境变量**是否存在**，设置为 `0` 仍启用；关闭必须 unset。
runner 的 demote 为显式 `--demote` 选项；其他选项须先核对 `--help` 和源码。1029 项冻结集不自动代表官方 2027 全集。
独立执行检查、诊断导出和消融的完整复现命令见 [BMC 验证](ikos-bmc-complete.md#validation)
与 [工作流程](ikos-bmc-complete.md#workflow)。

<a id="traps"></a>
## 3. 代码与诊断的易错约定

| 约定 | 为什么需要 |
| --- | --- |
| 日志 API | `log::debug/info/warning/error(StringRef)` 返回 void，不能 `<<`。流式用 `log::msg()`；它本身不受日志级别门控，须外加条件。 |
| AR 空定义 | `unique_def` 对形参/合流可能为空；`isa<T>(nullptr)` 会断言。先判空或用 `dyn_cast_or_null`。 |
| 双态产品 | 数据/异常域为 `inv.first()`，锁域为 `inv.second()`；post 的两分量都要正确初始化 bottom。指针分析实例化可能没有锁域，使用现有 trait。 |
| MUST/MAY 极性 | 必持锁、必 joined 合流取交；may-created 等取并。逐字段检查 top/bottom，集合名字不能证明同步。未知 unlock 只失效锁事实，不清空创建/完成信息。 |
| 整数布局 | join 前双方规范位宽/符号；地址偏移和访问 width 是不同事实，范围须按字节比较。 |
| 形参绑定 | 形参不靠单一语法定义取值；沿真实 call context/实参解析，BMC 另保留动态调用身份。 |
| 全局内容 | `GlobalVariable::type()` 是指针类型，值类型看 pointee；内容在 memory cell。entry invariant 不自动包含全局 points-to，须经正规物化/绑定。 |
| 句柄存储 | 取址的 `pthread_t` 局部是地址变量；读写内容走 memory，不能给地址 scalar 赋 nondet 来模拟句柄修改。 |
| 未知调用 | 指针实参包括 LocalVariable、InternalVariable 等；遗漏取址局部会保留陈旧句柄/数据。库效果与返回/异常/阻塞分别建模。 |
| 阻塞与干扰 | 未建模等待不能随意 no-op，让循环后访问因陈旧条件而不可达。按已声明合同导入干扰或回退 UNKNOWN，不能凭 API 名称假定成功。 |
| undef 与 freeze | 前端合成初始化用于抽象覆盖，不是源级合法 nondet 证据；BMC 不可凭它确认执行。0 checks + SAFE 可能正常，也可能覆盖遗漏，须查实际可达性。 |
| 初始化关系 | 初始化后只读、首写未发布、受锁访问分别证明；仅看到后续持锁不能推出 private_init。 |
| GEP 与区域锁 | 系数等于 1 不足以区分字段/char 元素；锁和访问的区域关系需布局、宽度、同一次实际下标等证据，不能靠系数猜字段。 |
| 动态身份 | 静态 site、函数、变量或 allocator 返回摘要不是动态对象/线程。once 按 control 身份和寿命建模，模糊 control 不注入确定 join。 |
| nondet API | 核实 intrinsic 或精确 API/类型/来源合同；不得用名字含 `nondet` 的子串认定合法自由值。 |
| BMC 符号 | 同路径值和地址消费共用实际动态定义；不同线程/调用/循环实例不得共用静态变量符号。 |
| fixpoint dirty | 所有共享摘要的语义变化均须通知外层迭代及依赖缓存，不能只看集合长度。最终 checker/replay 的新变化也需覆盖检查。 |
| 确定性 | worklist/序列化按稳定名字或语义键排序，不能按指针地址；不动点应保守覆盖不同调度，排序只是确定性手段。 |
| 库访问分工 | 已建模控制存储不重复合成普通未知库写；仍记录 create 句柄输出、getter 输出、timeout 读取、sem 输出等真实用户访问，不做整类 pthread 豁免。 |
| C 回归有效性 | 死代码读可能被 clang 消掉；检查 AR 确有目标访问，锁初始化、寿命、对齐及 ABI 必须合法。UB 用例只检查能力边界。 |
| 输入与 oracle | `.c`/`.i` 是不同输入，不能为比较私自换源；SV-COMP 以任务 YAML 为准。Goblint 行级 RACE/NORACE 是另一口径，需用其当版测试解析器，不能当比赛标签。 |
| 探针卫生 | 临时探针 env-gated，用完移除；保留的是通用可维护诊断。观察单次翻转后复跑并核对原因，不能把调度噪声当收益。 |

历史回归中有些精度断言会因后来保守边界失效，不能把旧脚本的整体 exit code 当最新全绿证明。
按 [AI 证据索引](ikos-ai-complete.md#evidence) / [BMC 证据索引](ikos-bmc-complete.md#evidence)
核对对应提交、实际判定和范围。
