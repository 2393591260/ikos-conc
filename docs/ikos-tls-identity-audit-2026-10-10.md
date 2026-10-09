# TLS 声明不能作为具体对象或共同锁身份

两个线程各自写自己的TLS int；一方把地址存入全局pointer，但没人解引用该pointer。
两份TLS对象仍不同，源程序无竞争。旧BMC把同一TLS声明当作共同global base，
ILP32/LP64均错误确认FALSE。默认抽象警告可以保守过报，但BMC不能确认这个假反例。

另一组FN：两个线程用各自的TLS mutex保护同一个普通global data。
锁对象不同，实际写竞争；旧抽象锁集把TLS摘要cell当作共同MUST锁，双架构均SAFE。
数组成员也复现。多选TLS锁虽未产生同一SAFE漏报，旧BMC仍可以利用假对象身份
确认其模型，故覆盖多选集合的保护证据也必须检查。

固定参考 `ThreadCreation.replaceThreadLocalsWithStackalloc` 为每个动态线程生成
独立TLS存储；12条参考查询逐项PASS/exit0或FAIL/exit13，与源程序预测一致。
IKOS当前没有相应owner-instance地址身份。本次不把静态声明冒充它：

- BMC对象范围查询拒绝TLS，覆盖访问和锁的实际前缀，未建模时UNKNOWN。
- 抽象语义不把TLS摘要位置加入MUST/条件锁；多选集合含TLS时也不添加自一致锁。
  这样共享数据不会因为假共同锁被候选过滤，PBR也不再以该假身份私有化。
- 已发布TLS地址仍按may-shared收集访问，没有把真实跨线程TLS竞争删除为SAFE。

六个最小例共24条双架构/default+BMC回归通过：假确认撤回为UNKNOWN，
真实TLS锁竞争不再SAFE；普通global mutex对照仍SAFE；真实escaped-copy竞争仍保留。
原20条P0观察及28条创建回归相同。build、install同步/cmp成功。
1029项ILP32逐文件全部不变：TP78/FP0/TN697/FN0/UNKNOWN254/error0。
LP64仅定向回归，没有全量或性能测量。
[证据](artifacts/tls-identity-2026-10-10/summary.json)、
[配对](artifacts/tls-identity-2026-10-10/paired.json)。

本轮继续审查发现：同入口线程各自分配的堆锁、栈锁也可被一个摘要位置合并，
最小例双架构均错误SAFE，尚待下一项修复。不能把本提交或基准FN0称为全部正确性
问题已消除。全局对象选择的精度补丁已暂存到隔离目录，未随本提交落地。
