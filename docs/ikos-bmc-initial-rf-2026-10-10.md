# 严格检查中的隐式初始写来源

参考 `Init.java`：每个初始化字段是内存模型中的实际写事件，拥有对象和字节偏移。
此前检查器固定读值却遗漏了没有前序显式写的rf来源；值相同的另一写仍可能替代。
研究接口现在导出实际Init类型、静态对象名、偏移和整数位宽。
映射只接受同对象/偏移、相同整数位宽、访问字节宽度一致且唯一的初始字段。
未知/未映射写、不同宽度重叠、并列或缺失clock仍由原通信规则拒绝绑定读源。
不添加自由初值，不用字符串打印猜测Init，不把隐式初始写作为竞争目标。

只有实际用作rf来源的Init才加入证书；来源和值的可行性由固定参考的原生程序编码
及rf关系检查。这个增量不绑定完整CFG、同步或所有局部值，仍属部分模型证据。

双架构新增10条参考校准：真实初始读SAT；等值不同对象、错误读值、已被覆盖的
初始源、把Init当竞争访问均UNSAT_AT_BOUND。8条映射边界断言通过。
完整88条既有参考校准通过，19条既有映射、13条nondet、17条通信/顺序、11条
退出协议断言通过。重复创建测试现在故意超过实际IR容量，不假设常量循环
只生成两个克隆；检查的是超界，而不是拒绝一个参考确实支持的第三次创建。
[初值校准](artifacts/bmc-initial-rf-2026-10-10/initial-calibration.json)、
[既有校准](artifacts/bmc-initial-rf-2026-10-10/full-calibration.json)。

真实候选simple和unverif各绑定一条隐式初始源，查询仍SAT、改固定值后UNSAT。
unverif的两个原初始读遗漏已消除，两读绑定同一个Init。
`23_lu-fig2.fixed`当前选定模型也SAT、改值后UNSAT；完整验证状态仍false。
[simple](artifacts/bmc-initial-rf-2026-10-10/simple.json)、
[unverif](artifacts/bmc-initial-rf-2026-10-10/unverif.json)、
[lu](artifacts/bmc-initial-rf-2026-10-10/lu.json)。

本阶段仅修改研究工具和fixture，没有改分析器/前端，未重复1029项分析器行为测试。
最新全量观察TP78/FP0/TN697/FN0/UNKNOWN254/error0引用创建过滤审查阶段；
lu隔离旧/新二进制均FALSE，不能把全量观察中的+1归因于该过滤补丁。
SAFE剩余规则审查及UNKNOWN精度恢复继续进行。
