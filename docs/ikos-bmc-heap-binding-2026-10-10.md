# 严格参考约束中的实际堆分配绑定

本阶段只修改研究适配器，分析行为及TP82/TN697/UNKNOWN250基线不变。
固定Dat3M `7caef5c850dc2dbf81c5554dcf41e402007f7a19` 的`Alloc`编码将返回
寄存器绑定到该事件的`MemoryObject`；`getAllocationSite()`用于核对实际身份。
适配器据此绑定IKOS导出的单次main分配，而不是用全局变量名代替堆对象。

映射要求实际main trace中分配执行一次、malloc声明与参数形状可核实、常量
大小一致，并能唯一对应源码位置上的参考heap Alloc。动态/重复/歧义来源拒绝。
证书要求Alloc事件本身有正确描述、线程/创建身份且执行，访问地址等于该对象
基址加偏移；已知大小、访问类型宽度和范围均核对。引用未绑定Alloc也拒绝。

真实IKOS证书的双架构校准共26条断言通过：原模型SAT，错误值和替换为另一
次真实同大小分配均UNSAT；错误线程/描述/大小/宽度/越界/未绑定分配均拒绝。
旧88条参考校准通过。main-worker、two-workers、field-helper的六项双架构
映射均SAT。benchmark `04-mutex_38-indexing_malloc` 的ILP32实际选中模型由
UNMAPPED变为部分约束SAT，修改其写入值后UNSAT。该benchmark的LP64模型
尚未做这一步；之前的双架构原生竞争确认仍保留。

参考内部pointer存储固定64位，不能把IKOS ILP32的4字节pointer槽声称为同样
的8字节访问；有heap的映射明确列出pointer存储/值绑定遗漏，由原生参考约束
其数据流。局部值、CFG/call对应、同步历史及完整总调度也尚未完全绑定。
这是**部分模型验证**，不是witness2.2 validator或完整反例验证，完整验证
计数不增加。37仍未映射：选中整数load所在行另有pointer load，当前按源码行
匹配不能区分两个静态load站点；不得用其它访问替代。

[校准](artifacts/bmc-heap-binding-2026-10-10/summary.json)、
[六项映射](artifacts/bmc-heap-binding-2026-10-10/fixtures.json)、
[benchmark模型](artifacts/bmc-heap-binding-2026-10-10/benchmark38.json)。
复现命令为`python3 scripts/dartagnan_strict/calibrate_heap.py --output <临时目录>`。
未修改参考安装、frontend或analyzer，未重复不相关的全量测试。
