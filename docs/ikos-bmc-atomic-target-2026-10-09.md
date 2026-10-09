# atomic 内部目标、完成路径和预算保护

以 `b843a60`（TP67/TN697/UNKNOWN265）为前态，冻结集1029项ILP32最终配对：
**TP77/TN697/UNKNOWN255，FP/FN/error0**，原67项TP和697项TN逐项保留。
TP是标签统计，仍**未全部独立验证**。证据与逐项转移：
[summary.json](artifacts/bmc-atomic-target-2026-10-09/summary.json)。
本轮没有修改frontend、抽象解释判定或资源预算常数。

## 为什么不只删 open gate

展开器现在显式标记选中的动态访问；目标不必是路径最后一条事件。
访问位于atomic段内时，沿真实CFG继续到End或实际helper Return加合成End，
中途的guard、算术、读写、调用仍必须编码。未知调用、未闭合或超预算不制造结束。
地址宽度、同址查询和结构化导出的targets均改用选中索引。

只关闭一侧并放宽相邻查询，最小对照即暴露错误：helper先写state=1再写2，
另一线程读state后assume(state==1)。若在read处截断，漏掉后续假guard，
会相对参考工具PASS错误确认FALSE。该中间版本没有提交。
现在凡涉及atomic目标，**两侧都要求真实的入口函数Return路径**，并保留全路径假设。
反面、未知尾部、不可完成路径仍UNKNOWN。这是保守的有限能力；不禁止未来以完整
动态控制依赖闭包替代整函数完成路径。普通目标仍采用已有候选前缀。

## 调度与竞争查询

完整AR事件调度、SC读来源、互斥/atomic区间、地址及定义性约束全部保留。
两个atomic内访问不确认竞争：固定参考的EndAtomic把体内事件标为RMW，
PropertyEncoder排除两侧都RMW的候选。该结论只用于BMC不确认，不用于抽象解释SAFE。

单侧atomic目标的查询要求两个访问按一个方向排序，中间无其他内存事件、真实锁、
创建或join；纯局部事件、assume和atomic边界不作为相邻障碍，但其执行和值约束
并未删除。普通目标仍用原来的scheduler clock差1查询。
这比参考CAT竞争关系更受限，不是一般hb等价证明；例如atomic体内目标的前后
都有其他内存事件时，当前查询可能丢失真实竞争（已保留middle-memory边界例）。

## 搜索预算与动态实例

新增完成路径搜索曾挤占旧普通候选：30_Function_Pointer3的1024次atomic/atomic
检查耗尽预算，原TP掉为UNKNOWN。现将**所有已展开路径中两目标均在atomic内**的
候选在完成搜索之前排除；不额外提高预算、不按文件名选候选。
该例现在只记录14次atomic_pair过滤，恢复原res访问竞争；一次定向观察从8.95秒
UNKNOWN变为0.87秒FALSE，负载未控制，不能称稳定全局提速。

另外，普通helper的Return枚举不能提前固定某次动态访问，否则循环中后一次调用
消失。目标标记只在实际目标搜索、atomic闭包或完成搜索保留；两个funloop旧TP
和改名/缩小数组的ordinary-helper-loop变体均保留。中间回退版本未提交。

## 验证与参考结果

build、install同步及cmp通过；新14例双架构28项断言、命名helper18项、指针返回10项、
P0双架构各10项，合计76项。四个新正例与普通helper循环参考双架构FAIL；
intermediate、opposite-assume、both-atomic、mutex、ordered-state对照参考PASS。
开放区间、不可达尾部、未知尾部、内部中间内存边界均保守UNKNOWN。

十项新TP：airline-5/10/15/20/25、01_inc、13_unverif、39_rand_lock_p0_vs-b、
fkp2014、qw2004_variant。八项有本次无附加约束native FAIL；
airline-20首轮60秒超时，固定两个参考访问及值后native竞争查询SAT。
airline-25在60/180秒及单对约束检查均超时，固定更多值/co的检查另记录；
**不得把超时写成参考FAIL或把该标签TP称为独立确认的原IKOS反例**。

named-target最小例固定两个IKOS内存事件后参考SAT，修改存值后UNSAT_AT_BOUND。
fkp2014、13_unverif因循环创建点无法唯一映射到参考实例，严格映射器返回UNMAPPED，
没有选择另一个实例代替。具体同步、局部值、CFG、总调度和所有TP的完整验证仍未闭环。
全量是ILP32；LP64仅定向回归。原始日志/试验/DB均在
`/tmp/ikos-atomic-target-2026-10-09/`。

## 下一步与尚未完成的目标

剩余255项逐任务诊断继续刷新，保留参考FAIL/PASS/TIMEOUT/ERROR及标签分歧。
本轮证明候选预算的前置排除会影响可恢复精度；IKOS提供的对象布局、实际创建、
may候选、offset和no-wrap信息仍参与BMC，没有移植一般CAT解释器或第二套C前端。
后续需优先完善严格动态创建/调用映射及具体执行验证，再推进对象/指针来源、
必要第三线程、join身份、更完整atomic竞争关系和公平搜索。
SAFE规则审查也尚未完成；不能称正确性已经全部证明或可恢复精度已经耗尽。
