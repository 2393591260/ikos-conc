# 依据实际循环守卫安排搜索（2026-10-10）

与7198337分析二进制的1029项冻结集配对：TP90→92/FP0/TN697/FN0/
UNKNOWN242→240/error0。只有`fib_safe-7-racy.i`与`fib_unsafe-7-racy.i`
恢复FALSE，所有旧TP/TN保留。本轮起点TP86以来累计新增六项。

先在固定Dartagnan bound20双架构确认两项原生SOURCE_RACE，再读取具体
源码：main的helper循环16次，退出检查需要再访问基本块，固定16上限
无法构造满足实际guard的完成路径。IKOS现在在原bound1查询之后，扫描
根函数及受限直接helper中的循环块整型大小比较；常量15..32提供`n+2`
的搜索提示，默认仍16，最多34次基本块访问。函数扫描至多64个、深度8。
只有自身在CFG循环中的比较参与提示，不按文件名、标签或函数业务名识别。

**它不是循环次数证明或SAFE证书**，也不证明循环可终止。初值非0、步长
不同、其他线程改写或不终止循环可能超出提示。所有保留路径仍须通过实际
CFG/机器guard、值来源、同步和地址编码；不发出截断分支，不将未找到竞争
变成SAFE。原10秒deadline、1024查询和展开资源预算保持；枚举中断组原有
小界限补充搜索也适用于新的界限。变化是通用搜索安排，不放宽确认前提。

新增长循环最小竞争旧ILP32 UNKNOWN，新版双架构FALSE；atomic安全对照
双架构SAFE，改名/抽helper变体双架构FALSE并实际使用提示。32条读源/
守卫/搜索断言、34条atomic断言通过，P0的20条观察相同。build/install/cmp
与全量完成，没有frontend修改。两个基准ILP32定向约7.07/7.19秒FALSE，
unsafe7 LP64也FALSE；不把定向耗时泛化为全局性能收益。

两个最小正例双架构固定参考SOURCE_RACE、四个所选模型部分约束SAT，
安全对照SOURCE_PASS。实际unsafe7 ILP32两条模型各绑定92个内存事件，
在参考bound17部分约束SAT；改变store值均UNSAT_AT_BOUND。完整CFG/call、
局部值及同步历史等仍未全部绑定，**完整独立验证计数不增加**。

[全量配对](artifacts/bmc-loop-hint-2026-10-10/paired.json)、
[统计](artifacts/bmc-loop-hint-2026-10-10/summary.json)、
[原生参考](artifacts/bmc-loop-hint-2026-10-10/bench-native.json)、
[unsafe7所选模型](artifacts/bmc-loop-hint-2026-10-10/fib7-32-partial.json)。
