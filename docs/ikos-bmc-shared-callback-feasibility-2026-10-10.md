# 共享回调值流的下一阶段依据（2026-10-10）

这是可行性分析与参考探针，尚未增加此能力。当前实现与冻结集仍是
TP104/FP0/TN696/FN0/UNKNOWN229/error0，不能把下一阶段方案计为恢复。

## 一个真实目标及可复核的预测

`04-mutex_27-base_rc.c` 初始化 f=good，main 创建 worker 后持 fm 写 f=bad；
worker 持 fm 读取 f，随后间接调用 g()。main 在 gm 中读 global。
worker 读到 bad 后不持 gm 写 global，与 main 访问形成真实竞争前缀；
若读到 good，其 global 写仍持 gm，与 main 受共同锁保护。
IKOS 当前卡在调用 g 的 call_summary，不能从 may-callee 集证明实际调用 bad。

固定参考 7caef5c、bound2 双架构新跑 SOURCE_RACE。原生 processed-IR
探针固定 main 的 global 读、worker 的候选写、f 读及实际 rf：

| 所选值源与目标 | ILP32 | LP64 |
| --- | --- | --- |
| f 读从 main 的 f=bad 取值，进入 bad 写 | SAT | SAT |
| f 读从初始 good 取值，却进入 bad 写 | UNSAT_AT_BOUND | UNSAT_AT_BOUND |
| f 读从初始 good 取值，选择 good 的受锁写 | UNSAT_AT_BOUND | UNSAT_AT_BOUND |

这是参考原生 rf/callee 探针，不是 IKOS 所选模型验证。严格引用完整
动态 event/creator/descriptor，未把单纯标签 FAIL 当作移植充分条件。

## 参考代码中的机制与不可照搬部分

`NaiveDevirtualisation.java:89` 按 addressed functions 和 call type 构造
`funcPtr == target` 条件分支；`Inlining.java` 再复制调用体及每次调用的
register scope。共享 pointer 的真实读值仍由 WmmEncoder 的 rf/co 决定。
只移植函数枚举而丢掉 guard/rf，会制造不可达 bad 调用。

同一 devirtualisation 文件还主动删除对当前函数的递归候选，并打印
“unsoundly assumes”告警；该限制不迁移。未知/递归上下文需要有界实际
前缀或保守 UNKNOWN。上述具体用例没有递归，该通用风险不使本例探针失效。

## 结合 IKOS 的最小实施范围

1. 从实际候选及 IKOS may-callee 信息安排目标和搜索顺序；may 信息只用于
   候选覆盖/排序，调用存在性必须由具体值约束建立。先保留旧查询，再试扩展。
2. 初版只支持同一 global 固定偏移的 function-pointer slot，真实静态
   function/null 初值、所选 startup/A/B 实际完整宽度的函数地址 store，
   沿合法 cast/assignment 传播。TLS、普通数据指针转换、partial bytes、
   未知初值、未编码的 source 或 ABI 不匹配保持 UNKNOWN。
3. 复用既有 SC 时钟/最新写源约束，函数值用稳定语义身份表示；每个调用
   必须满足“读到的函数身份 == 本次展开函数”，并核对原 LLVM signature、
   calling convention、参数和返回类型。枚举到某体不是调用已发生的证据。
4. 保留每次动态调用的定义/参数/return 上下文；两个线程或两次调用不能
   因共享 AR InternalVariable 而共享局部符号。支持范围先限定非递归调用。
5. 导出调用函数身份与 pointer rf，区别于机器整数/实际地址。不能把内部
   token 数字当成 C 函数的实际地址塞给 validator；同步更新 mapper 或
   明确标注尚未绑定部分。原始指针 ABI 的已知参考边界继续保留。
6. 正例、只读初始 good 对照、不可执行 bad-store 对照、签名不兼容、
   未写 pointer、改名/helper、同入口多实例分别检查；然后 build/install、
   P0、相关套件及1029逐文件配对，通过后独立commit。

该阶段可行，但须联动 unroll、pointer 值编码、动态调用及证据导出；
不把它压成一个放行 call_summary 的补丁。预计收益只能在实现后配对衡量，
既有一项参考TP不是普遍可恢复数量，也不构成已经获得加速的证据。

[参考结论](artifacts/bmc-shared-callback-feasibility-2026-10-10/reference.json)、
[六项探针](artifacts/bmc-shared-callback-feasibility-2026-10-10/probes.json)。
原始材料在 `/tmp/ikos-recovery-2026-10-10/shared-callback-feasibility/`，不进Git。
