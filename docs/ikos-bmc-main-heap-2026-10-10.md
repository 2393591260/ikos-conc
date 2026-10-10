# 实际初始化的不可变指针槽与单次main堆分配

本轮固定Dartagnan双架构重新确认04-mutex_37-indirect_rc、
04-mutex_38-indexing_malloc的真实竞争。前者两个全局指针槽在main发布同一
全局整数地址；后者在main分配一个整数，通过全局槽发布给worker，main与
worker直接写它。旧BMC不能追踪这些load出来的地址，并且只允许global目标。

新增通用地址证明，不凭IKOS摘要位置相同就确认对象相同：

- 全局指针槽的LLVM地址只能用于直接pointer load和唯一store，没有地址逃逸、
  其它callee／后续改写；该store须在实际隐式main前缀中、所有create之前执行。
  沿本次动态定义及实际分支追溯其值，不能取未执行或未来初始化的值。
- malloc须是已核实的库intrinsic／LLVM声明和调用ABI：pointer返回、一个与
  目标指针宽度一致的size_t整数参数，不把错误原型或用户函数体当分配摘要。
  选择一次合法成功分配；实际malloc在隐式main且首次create之前执行一次，
  正整数大小可由已执行纯定义算出并不超过1MiB。递归main、重复站点、动态／
  未知大小需要进一步动态身份支持，当前拒绝。
- 事件独立携带allocation和extent；global的base不被伪装成heap对象。目标须
  解析到同次分配，偏移、宽度、范围重叠及全部值／guard／同步检查仍成立。
  Heap候选只从有限may集合选取，实际身份由上述前缀证明建立。
- 第一版只支持heap写，heap读／pointer rf未建模时拒绝，绝不把malloc的
  未定字节当自由整数。free前缀无摘要，保持UNKNOWN；worker分配、可变指针槽、
  槽地址逃逸等也继续保守。直接heap线程参数的定义性证明尚未接入。

global槽指向global或heap使用同一发布证明，因此恢复37与38两个任务。
同机制使旧shared-pointer-unsupported正例恢复FALSE，已改名shared-pointer-race；
固定参考双架构重新确认FAIL，没有放宽未知共享读或使用may-alias代替地址。
改名、struct字段偏移、helper、两worker、公共锁、不同字段、独立分配、早于
发布的读取、错误ABI、释放／改写／逃逸／重复站点等共26条双架构断言通过。
P0共20条观察与原版一致；20条分配锁、16条global-choice、28条atomic回归
通过。build成功，install同步/cmp一致。没有frontend改动。

最终1029项冻结集ILP32配对：**TP80→82 / FP0 / TN697 / FN0 /
UNKNOWN252→250 / error0**。唯一两项转移是上述37、38，旧TP／TN全保留。
此前未加LLVM原型gate的一轮总数80，两项新竞争恢复但fmax/lu回退；最终全量
和新二进制定向都确认fmax/lu。仍无证据将这些资源敏感变化归因于ABI语义，
不声称稳定全局提速。LP64只有定向回归／参考，未全量。

三种新正例与三种安全对照均有双架构固定参考FAIL／PASS；两个benchmark
再送入现有严格映射器时均UNMAPPED：37的所选间接访问尚不能唯一映射，
38的堆对象还没有严格的参考分配绑定。没有用另一个访问替代，也没有增加
完整独立验证计数。TP依然是标签确认、未全部独立验证；这些缺口必须后续
补齐。09-regions指针列表任务本轮也有参考竞争，但当前需要worker／helper
分配和pointer rf，不能声称已经恢复或无法实现。

[配对](artifacts/bmc-main-heap-2026-10-10/paired.json)、
[汇总和哈希](artifacts/bmc-main-heap-2026-10-10/summary.json)、
[实际所选目标](artifacts/bmc-main-heap-2026-10-10/confirmed-targets.json)。
临时目录保留原始日志，不提交数据库／大输出。
