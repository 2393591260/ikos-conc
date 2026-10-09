# Dartagnan 指定候选约束接口：校准阶段

2026-10-09。遵循用户确认的方案，先复用 Dartagnan 自身的程序/内存模型编码，
不继续扩展 `scripts/replay_bmc.py` 草稿。此阶段没有改动 IKOS 分析器或前端。

## 已实现与校准

`scripts/dartagnan_strict/calibrate.py` 核对固定提交
`7caef5c850dc2dbf81c5554dcf41e402007f7a19` 的 AssumeSolver 源码 hash，
在临时目录生成并编译 hook，通过 classpath 使用原 JAR/依赖，不覆盖原工具包。
完整程序、CAT 内存模型及性质编码仍由 Dartagnan 提供。

hook 把查询限制到证书指定的动态事件/线程、具体创建事件、所选竞争对，以及
可选内存值、地址和 rf/co/hb 关系。处理后 IR 指纹与事件描述必须匹配。
未识别字段、无法表示的值及错误身份在求解之前终止，不忽略后继续接受。
证书模式关闭线程对称性破缺：指定实例不能因与另一个实例对称而被任意排除。
竞争对公式参考原 PropertyEncoder，归属与许可见 `DAT3M-LICENSE`。

ILP32/LP64 共 **26 项断言通过**：合法同入口两实例证书 SAT；错误线程、
创建事件、IR、描述和越界值被拒绝处理；错误值/地址、同实例候选、非冲突候选、
循环 CAT hb 约束和公共锁候选得到 UNSAT_AT_BOUND。
非冲突候选所在程序另有真实竞争，查询仍然 UNSAT，避免替代所选访问对。

命令：

```bash
python3 -B scripts/dartagnan_strict/calibrate.py --output /tmp/ikos-dartagnan-strict-NEW
```

默认源/JAR/JDK 路径均可通过参数覆盖。输出目录必须新建。
[精简校准证据](artifacts/dartagnan-strict-2026-10-09/calibration.json) 保存结果、指纹和命令；
原始日志/编译类在 `/tmp/ikos-dartagnan-strict-2026-10-09-d/`，不入 git。

## 结论边界

这是 **处理后 IR 约束接口**，尚未实现任意 IKOS AR 证据到 Dartagnan IR 的完整映射，
不是官方 YAML 2.2 validator，也没有使当前 58 个标签 TP 成为独立验证 TP。
此阶段未重跑 IKOS build/P0/全量，因为 IKOS 行为没有修改；之前已复现的基线仍为
TP=58、FP=0、TN=698、FN=0、UNKNOWN=273，不能算作本阶段新测结果。

SAT 只证明本次全部已传入约束在参考编码中可满足；遗漏的 IKOS 证据不能宣称被验证。
UNSAT_AT_BOUND 只表示当前展开界限内无法满足证书，不是无界 SAFE，
也不能直接判定 IKOS 反例错误：先检查映射、参考语义和展开覆盖。
CAT hb 不是 POSIX hb，也不是 IKOS 的总调度 clock；不能直接把 clock 顺序转成 CAT hb。
原版本同址竞争查询不自动覆盖所有混合宽度重叠情况。

下一步：从真实 IKOS 记录建立可审计的线程/创建/事件映射，逐项报告绑定和遗漏内容；
固定值、相对对象地址和同步历史齐全后才把结果提升为对应反例的独立验证。

## 第二个增量：相对对象地址与真实 IKOS 记录

地址证书新增静态对象名、byte offset、width，参考端检查唯一静态对象、已知大小、
有效范围和实际访问宽度，再约束地址等于对象 base + offset。不同工具的绝对地址不作比较。
新增错误对象/宽度/不存在对象对照，双架构校准扩展至 **32 项全部通过**。

`map_ikos.py` 接入真实 `04-mutex_01-simple_rc` 的已保存 AR 记录。只在源码、
动态创建事件和访问映射唯一时生成约束；歧义/重复执行映射失败不选择一个猜测结果。
固定 main load、worker load/store 三个事件的对象/偏移/宽度，worker load=0、store=1，
参考查询 SAT；改变 store 为2且保留 load=0 得到 UNSAT_AT_BOUND。
六项映射断言检查正常映射、损坏创建、无效目标位置、源位置损坏、歧义和动态事件塌缩。
[真实 IKOS 实验](artifacts/dartagnan-strict-2026-10-09/ikos-simple-fixed-memory.json) 和
[32 项校准](artifacts/dartagnan-strict-2026-10-09/relative-address-calibration.json) 已保留。

结果仍标记 `PARTIAL_FIXED_MEMORY_MODEL`：局部值、CFG/调用实例、同步历史以及 IKOS
完整调度/rf 选择未全部映射。Dartagnan 自身继续编码完整程序及同步，但允许它选择
未绑定部分，因此不宣称严格验证了整个原反例，也不增加独立验证 TP 计数。

映射负例曾发现同源码行的普通调用被误匹配为创建事件；现已要求 AR 语句确为
直接 pthread_create、参数数目正确且入口参数匹配子线程，再进行参考创建匹配。
修复后六项映射断言及真实用例正反实验重新通过，原始复测在
`/tmp/ikos-dartagnan-map-2026-10-09-b/`。

## 全部保存候选的部分约束遍历

对既有58任务/71候选逐项检查输入 hash，再按同一接口遍历。bound=2、每进程 JVM
-Xmx2g、墙钟60秒、并发2；这些是研究设置，不是官方比赛配置。
38候选得到部分约束 SAT，31候选 UNMAPPED，2候选 UNSUPPORTED。
31项映射失败包括17项动态创建匹配、12项目标匹配、2项动态执行塌缩；
2项不支持是参考对象范围/访问宽度不满足契约，不能绕过宽度校验。
没有把任何记录升级为完整独立验证；也没有根据这些结果修改 IKOS verdict。

[逐项结果](artifacts/dartagnan-strict-2026-10-09/fixed-memory-sweep.json) 保存每个候选的
绑定、遗漏和参考结果。原始日志在 `/tmp/ikos-dartagnan-fixed-memory-batch-2026-10-09/`。
复用入口：

```bash
python3 -B scripts/dartagnan_strict/sweep_ikos.py \
  --reports /tmp/ikos-p0-replay-2026-10-09/confirmed-reports \
  --output /tmp/ikos-dartagnan-fixed-memory-NEW
```

输入摘要通过 `--export-summary` 指定；参考包、源码和JDK路径可覆盖。
映射失败/有界不满足均需进一步分析，不能据此撤回任务标签或判断程序安全。
