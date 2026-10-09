# BMC A1 地址编码：实现与验证（2026-10-08）

本轮实现可追溯局部值的单变量地址下标编码。冻结集实测 TP **52 → 53**，
TN=709、FP=0、FN=0、错误=1 不变，UNKNOWN **267 → 266**。
唯一变化是 `goblint-regression/05-lval_ls_14-idxunknown_access.c`：UNKNOWN → FALSE。
交接中的“约 6 TP”未实现，不能用文件分桶代替逐任务验证。

## 实现及边界

修改集中在 `analysis/bmc/encode.hpp` 和 `unroll.hpp`，未改候选过滤规则、循环上限或 benchmark 选择。

- 地址偏移为 `const_off + coeff * encode_value(index)`，与该路径的 guard 使用同一缓存和前缀。
  不同路径保留独立缓存；值符号 `value_a_*` / `value_b_*` 与事件时钟 `a*` / `b*` 分离。
  旧版名称重叠会把独立值错误约束成时钟；两次 nondet 均为零的回归验证了这个问题。
- 支持整数常量、明确识别的整数 nondet 来源、同类型赋值、SExt/ZExt，以及同宽整数 bitcast。
  扩宽保留数值；bitcast 保留位模式，并显式转换有符号/无符号解释。
  依据是 AR 的类型检查与整数操作契约（`ar/src/verify/type.cpp`、`numerical.hpp`），不是丢弃转换。
- nondet 来源采用明确的 `__VERIFIER_nondet_*` 整数 API 集合或 `IkosNonDet` intrinsic，
  不再依据函数名包含 `nondet`。要求零实参及匹配的整数返回位宽；普通用户定义函数不凭名称认作自由来源。
  旧式 C `extern int f()` 在 AR 中可能表现为 vararg，不能因此直接拒绝一个实际零实参的合法调用。
- 常量、字节偏移和系数用精确整数表示，SMT 输入使用十进制字符串，不截断到宿主 `int64_t`。
  地址提取仅接受常量项和至多一个直接 InternalVariable；不使用忽略机器溢出的仿射化来绕过值来源检查。
- 新开放的符号地址要求已知全局对象定义及双方访问宽度，并约束访问位于其分配范围内。
  未知对象范围、未知宽度或不支持的值来源均不确认。
- load、实参、未知调用、phi、收窄和机器算术继续返回未编码；未实现 rf/co。
  `check_race=false` 仍是“未确认”，不能据此得出 SAFE。

本轮扩展不补全整体 BMC 的初始化/线程依赖、同步和内存模型。下面的 FP/FN 零是有限测试的观察，
不构成整个分析器的数学正确性证明，也没有声称已完成独立 witness 验证。

## 验证证据

起点：`51b795c4b09f36170a69be4e5ce0a10345db2de6`；终点：该提交加本轮工作区修改，尚未提交。
数据集：`/home/ruan/sv-benchmarks-2026`，提交 `0a254fdddcfe24949be9edd47f8d609462eb52fb`。
沿用既有脚本的 1029 任务集合；此集合全为 ILP32，不代表已核定 2027 官方任务全集。

构建 `ikos-analyzer` 成功，重新编译了修改所影响的 `data_race.cpp`，并同步安装。
最终 build/install 二进制逐字节一致。二进制和实现 diff 的 SHA-256 见
[实验元数据](artifacts/bmc-a1-address-2026-10-08.json)。

| 指标 | 基线 | 本轮 |
| --- | ---: | ---: |
| TP | 52 | 53 |
| TN | 709 | 709 |
| FP / FN | 0 / 0 | 0 / 0 |
| UNKNOWN | 267 | 266 |
| 错误/超时 | 1 | 1 |

逐任务身份、数据模型、预期标签及前后结果保存在
[1029 任务配对结果](artifacts/bmc-a1-address-2026-10-08.tsv)。只有一个判定变化，没有 TP/TN 回退。
保留的错误是 `ldv-linux-3.14-races/linux-3.14--drivers--net--irda--nsc-ircc.ko.cil.i` 超时。

命令（仓库根目录；A/B 都使用相同 30 秒 wall timeout、8 jobs）：

```bash
cmake --build build --target ikos-analyzer -j"$(nproc)"
IKOS_BMC_RECOVER=1 python3 -B svcomp_race_test.py --demote
python3 -B analyzer/test/regression/bmc-address/runtest
python3 -B docs/review-repros/bmc-2026-10-08/run.py --bits 32
python3 -B docs/review-repros/bmc-2026-10-08/run.py --bits 64
```

新增地址/值回归为 14 场景 × 两种位宽，**28 项通过**。覆盖有符号/无符号下标、helper、源文件改名、
guard 分离地址、混合访问宽度、时钟/值符号分离、大整数常量、bitcast、旧式声明，以及 load、未知来源、
收窄、算术、越出对象范围时的保守不确认。越界场景用于检查能力边界，不作为 race-free 输入。
同一测试在保存的旧二进制上失败，最终二进制通过；旧版失败不全部等价于 FP，有些是缺少确认能力。

历史反例共 **20 条模式/位宽观察**已逐项检查，而非仅检查脚本退出码：

- `exclusive-flag` 仍为 UNKNOWN，未确认伪竞争。
- `overlapping-widths` 及其对照在 default/BMC 下均为 FALSE。
- `published-before-init` 及其对照在 default 下为 FALSE；BMC 仍因堆对象边界为 UNKNOWN，没有变为 SAFE。

原始构建、基准和回归日志保留在 `/tmp/ikos-a1-address-io1ltxef/`。
新增 TP 也通过了 `benchexec/tools/ikos-conc.py` 的 standalone 适配路径：ILP32 下返回
`FALSE(no-data-race)` 并生成 `witness.yml`；文件保存在该目录的 `new-tp.witness.yml`。
这是适配与生成检查，未运行独立 witness validator，也不等价于完整 BenchExec 资源隔离评测。
安装复制未完成时启动的一轮检查作废；文中指标来自复制完成且校验一致后的最终运行。
中间版本曾因拒绝旧式 C 声明导致 TP=50；修复后两项原有 TP 均恢复。

## 对后续工作的重新评估

交接中的地址桶不是同一种缺口：`05-lval_ls_01-idx_rc` 的数据访问没有变量下标，
`05-lval_ls_09-idxsense_rc` 使用常量下标；两者还有初始化循环。
`05-lval_ls_13-idxunknown_lock` 的变量在锁地址上，不能直接按本轮的数据地址扩展处理。
`06-symbeq_09-tricky_address4` 还涉及 `i++` 和锁/数据槽关系；`indexer` 依赖实参读取、循环和 CAS helper。
这些文件没有被本轮确认，具体 gate 的归因仍需要单独验证，不能断言单加地址编码就能全部找回。

下一步先量化剩余候选的阻塞原因，再决定算术、锁身份或循环哪个投入更有收益。
若实施 A1 算术，建议统一机器整数的 bit-vector 表示和 U/S 比较、扩宽/收窄/bitcast，
按 AR 的 no-wrap/poison 契约处理溢出；不要把 `i++` 直接编码成无界整数加一。
时钟仍保持整数，地址需要明确从机器位模式到位移的解释。这属于后续实施，不在本轮新增能力中。

rf/co 的最小可行子集可以单独推进，但不能仅在两条候选路径中挑“最近的 store”：

1. 从候选控制流、读值和地址依赖构造有界闭包，纳入全局初值、main 创建前缀及相关线程/写入。
2. 先固定 SC 子集：写入及程序/同步事件放在一致的执行顺序中，读选择兼容该顺序且未被后续写覆盖的来源。
   更弱内存模型需要独立的 rf/co 与相关关系公理，不能沿用自定义 `hb` 定义。
3. 对实际循环迭代和调用实例建立不同动态身份，禁止静态 SSA 指针复用错误相关多个 nondet/读事件。
4. 闭包缺项、未知副作用或相关同步未建模时保持 UNKNOWN；达到展开边界而 UNSAT 也不能判 SAFE。
5. 先用最小 flag 协议及 race-free 对照验证可行执行，再跑逐任务 A/B，并接入模型/执行 witness 验证。

可行性判断：相关基础设施能复用，但当前事件中缺少一般写值/读来源和完整动态执行上下文，
这是跨 unroll、encode、候选依赖及 witness 的独立工程。实际收益须重新评测，暂不认可“约 15 TP”作为承诺。
