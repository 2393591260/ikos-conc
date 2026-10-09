# 优先较短的实际前缀

稳定排序完整生成的路径集合，按事件数从短到长查询；排序发生在构造完整性检查之后，
不删路径、不截断调用/atomic尾部、不改SMT或SAFE过滤规则。相同长度保留原次序。
文件共享预算仍可能受查询次序影响，所以采用前做完整逐任务配对。

build成功、install同步/cmp一致，双架构P0的20条观察逐项检查，候选回归30项通过。
1029项ILP32全部前后相同：TP77/FP0/TN697/FN0/UNKNOWN255/error0。
没有前端改动。LP64仅定向回归，未跑LP64全量。
[证据](artifacts/bmc-short-prefix-2026-10-09/summary.json)、
[逐文件配对](artifacts/bmc-short-prefix-2026-10-09/paired.json)。

`thread-join-counter-inner-race`所选main前缀从109步变25步，实际创建从15次变1次；
outer-race也从15次变1次，所选main前缀28步。这是查询不同的真实可行路径，
不是修改已保存模型中的数值。两个新模型在参考bound2下固定具体实例、nondet、
已映射内存值和rf/co，均SAT（约3秒），改固定值均UNSAT_AT_BOUND。
旧15次创建模型及其超时/界限不足观察保留，未宣称已经验证旧模型。
新模型的全部局部/CFG/同步/调度仍未绑定，完整验证数量不增加。

这个实验降低了两个所选反例的验证成本，没有增加本轮标签TP；
全量脚本未保留逐任务耗时，不能宣称整体提速。原日志、二进制和report留在
`/tmp/ikos-short-prefix-2026-10-09/`。后续继续同步/调度及未映射证据，
并处理117个参考竞争UNKNOWN的通用语义和搜索能力。

## 已映射访问的顺序约束

研究适配器增加 `order.before/after`，按实际模型clock保存已映射内存访问的相对
顺序，等clock不指定两者先后。参考端用独立 `ikos_schedule` 整数排序变量，
要求它延伸真实SC图并满足这些相对顺序。只绑定已映射访问，其他事件仍可穿插；
这不等于保存了所有CFG、同步和完整总调度。

读取固定源码发现：WmmEncoder.java:160–215由axiom encode graph决定active sets；
Acyclicity.java:95–123只保留SCC内相关边，且可以移除可推出的must边。
原无环判定合法的削减，不能直接当作完整排序证书的支持条件。
适配器在内存模型预处理/分析之前增加参考原生 `ForceEncodeAxiom(hb)`，
保证实际hb及依赖的所有may边获得编码；在独立排序变量上约束所有实际边前向。
新axiom只要求编码，不增删源执行的内存模型约束。无证书查询维持原模式。
此处CAT hb表示SC图，不声明为POSIX同步关系；相对顺序也不增加CAT hb边。

参考校准84项和补充关系控制4项（均覆盖ILP32/LP64）全部通过，合计88项。
最小写序例现在两次写同为9：去掉顺序、指定第一次写为读源可以SAT，
保留第二次写在读前的实际顺序后，同一读源被拒绝。
独立对象的指定执行顺序SAT，改成要求该方向CAT hb边UNSAT。
这些控制覆盖“相同值不足以绑定读源”和“执行先后不等于hb关系”。
[校准](artifacts/bmc-short-prefix-2026-10-09/schedule-calibration.json)。
49项Python映射检查、11项退出协议检查仍通过。

计数器inner/outer分别固定2/5条访问顺序，保留实例、nondet和rf/co，
均SAT、改值后UNSAT_AT_BOUND。
[选定查询](artifacts/bmc-short-prefix-2026-10-09/schedule-selected.json)。
仍属于PARTIAL_FIXED_MEMORY_MODEL，完整验证数量不增加。
本阶段仅修改研究工具，未改分析器或前端，TP77基线引用上述完整配对。
