# 原始整数全局对象的字节级 SC 读源（2026-10-10）

## 能力及来源前提

此前宽度干扰修复把部分整数写发布为目标类型TOP，恢复了抽象解释被删掉的
真路径；BMC仍因读写位宽不同无法确认。此阶段在同一完整前缀/同步/值来源
检查之上，恢复这些路径的实际读值，连接AI候选覆盖和BMC具体执行证明。
不是把TOP、may摘要或无初始化malloc字节直接变为自由符号。

支持实际constant offset、原始整数global、位宽为8的倍数且不超过64位，
load/store位宽必须等于实际访问字节数×8。使用AR data_layout的字节序。
原有同宽整值/符号数组编码保持原路径；struct/array混合宽度、symbolic
部分写、pointer字节、padding/undef、heap及更大对象仍拒绝，不依赖文件名。
constructor及库控制/输出值边界先已修复，不能用过时静态初始化补读源。

## SC 构造

每个读字节由已证明的整数初值位图或保留前缀中的最近覆盖写提供。
startup实际写按main顺序更新被覆盖字节，其他字节保留有来源的初值。
A/B写用同一SC全序与读时刻选最近的覆盖写；store值来自已有encode_value，
按源位宽取bit pattern，再按目标布局抽取字节。每个字节可有不同覆盖写，
但整次load共享唯一clock，不得各自选择读取时刻来造撕裂值。
最终concat并按load类型解释有符号/无符号整数。来源未证明的字节绝不自由，
仅当实际覆盖写在读前可提供该字节时允许缺初值。

所有新来源仍受po、create、mutex、atomic、范围、trap/poison和实际动态
定义约束；值cache及符号按动态路径分开，startup nondet则共享同一真实来源。
统一位宽旧编码保留，避免让fib等重复整值计数器付出逐字节公式代价。

## 正反例与参考边界

26条双架构新断言：9种正例UNKNOWN→FALSE，包括低/高字节、改名/helper、
双来源拼接、signed高字节扩展、从word读byte、main部分初始化、unsigned
byte及64位最高byte。互斥selector、不可能的byte组合、整word一致性对照
不得FALSE；aggregate混合宽度保持UNKNOWN。

固定Dartagnan再次双架构确认低字节SOURCE_RACE。parent部分初始化、两个
byte源拼接及64位高byte正例则双架构SOURCE_PASS；互斥对照SOURCE_PASS。
前轮已核实固定word模型的rf/sameAddress没有实现一般混合宽度的字节拼接。
不把这些PASS当作推翻C表示的依据，也不能把这种参考分歧算作独立验证通过。
原生gcc双架构byte表示断言已确认低255、高256、合并511；64位与有符号
表示及整word一致性对照均由新增native assert在gcc -m32/-m64确认。
它们只验证标量表示，不是并发竞争validator；大端分支未在真实大端机器测试。

[修复前](artifacts/bmc-byte-rf-2026-10-10/before.json)、
[最小回归](artifacts/bmc-byte-rf-2026-10-10/fixtures.json)、
[固定参考](artifacts/bmc-byte-rf-2026-10-10/reference.json)。
所选18个模型按架构保留：[ILP32](artifacts/bmc-byte-rf-2026-10-10/selected-pairs-32.json)、
[LP64](artifacts/bmc-byte-rf-2026-10-10/selected-pairs-64.json)，每个文件<1MB。
原始日志/数据库留`/tmp/ikos-recovery-2026-10-10/sc-byte-rf/`。
完整独立TP验证及全部SAFE证明仍未完成。

build/install/cmp、26条新断言、12条干扰宽度、16条库值来源、10条
constructor、46条SC、34条atomic、14条锁干扰断言通过；P0双架构20条
观察与前一阶段逐项相同。原SC mixed-width-unsupported.c真实竞争已恢复
FALSE，更名mixed-width-race.c，移除过时unsupported要求，没有放宽对照。
冻结集1029项ILP32逐项不变：TP98/FP0/TN696/FN0/UNKNOWN235/error0。
[全量配对](artifacts/bmc-byte-rf-2026-10-10/paired.json)、
[统计与hash](artifacts/bmc-byte-rf-2026-10-10/summary.json)。

低字节ILP32/LP64各一个实际所选模型的严格参考绑定得到部分约束SAT，
同一模型改值各得UNSAT_AT_BOUND，见[部分检查](artifacts/bmc-byte-rf-2026-10-10/partial-validation.json)。
没有新增完整独立验证计数；其余混合宽度正例的固定参考模型分歧仍保留。
未做全局性能收益声称，LP64只做定向验证，尚无全量LP64运行。

对已有未提交 `scripts/replay_bmc.py` 做了只读补充检查（未修改/提交脚本）：
18个所选模型中，9个ILP32在独立于Z3的Python逐事件AR解释下ACCEPT，
确切读值、物理字节、创建实例、同步vector clock及目标竞争均通过。
各改一个实际load值、各改一次creator绑定，18个损坏模型均REJECT。
[结果和脚本hash](artifacts/bmc-byte-rf-2026-10-10/conditional-replay-probes.json)。

9个LP64则因重放器默认mutex初始化检查UNSUPPORTED：零初始化的NULL
链表指针被内部表示为pointer-byte tuple，不等于整数0，现有检查因此
误认nondefault。不是已证的假见证；没有改脚本或绕过该检查。
[全部结果](artifacts/bmc-byte-rf-2026-10-10/conditional-replay.json)。
这些ACCEPT以同一IKOS导出的AR program snapshot及glibc合同为前提，
没有独立C frontend import，不等同于SV-COMP witness-2.2验证成功。
脚本仍未进入git，证据由对应本地hash约束，不能声称全新checkout可直接复现。
