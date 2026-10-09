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
