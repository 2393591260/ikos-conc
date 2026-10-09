# 完整命名 atomic helper 前缀

本轮仅修改 BMC，没有修改 frontend 或抽象解释 SAFE 判定。以 `4fa4fa6` 为前态，
1029 项冻结集 ILP32 配对结果：TP61→67、TN697→697、UNKNOWN271→265，FP/FN/error0。
旧61项TP和697项TN全部保留。TP仍指**标签确认、未全部独立验证**。
逐项转移、二进制哈希和参考结果保留于
[summary.json](artifacts/bmc-named-atomic-2026-10-09/summary.json)。

## 契约与迁移依据

[SV-COMP 2027 官方规则](https://sv-comp.sosy-lab.org/2027/rules.php)明确：
`__VERIFIER_atomic_*` 已弃用但仍用于现存任务；以 `__VERIFIER_atomic_` 开头的
函数必须作为不可被其他线程打断的执行处理。begin/end不得嵌套或交错。
本轮核对网页原文，而非依据名字中含有 atomic 的猜测。

固定 Dartagnan `7caef5c850dc2dbf81c5554dcf41e402007f7a19` 的
`program/processing/Inlining.java:120–125` 在命名函数展开两端插入 BeginAtomic/
EndAtomic；后续仍复制动态局部寄存器、实参赋值和实际返回。IKOS 采用其边界保留
原则，复用自己的调用前缀、动态定义、机器整数、地址和按需 SC 读来源编码。
IKOS 的函数识别遵守官方带下划线前缀，不照搬该版 Dartagnan 更宽的 startsWith。

## 实现与前提

有实际函数体、确切动态 callee 的命名 helper 保留一次真实调用，在函数体开始前加
合成 Lock，沿实际 CFG/guard 展开，只有具体 Return 前缀之后才加 Unlock。
现有全局 atomic 区间约束排除**另一线程的所有事件**，不局限于另一个atomic段。
返回值和参数仍按同一次调用解析，不引入自由load。

合成事件显式带 `synthetic_atomic`：调用栈重建、到达定义和源操作数定义性检查
跳过这些标记，防止把同一源call误当三次调用或重复定义。结构化前缀导出区分
`synthetic_event=atomic_begin/atomic_end`；validation_status继续为UNVALIDATED。

目标位于未闭合atomic体内仍由 `atomic_prefix_open` 拒绝；嵌套由relock门拒绝；
atomic线程根入口无普通call可包装，显式返回 `atomic_root`。未知外部helper、
GCC硬件原子、原生atomic Load/Store及其他未建模API仍拒绝。搜索预算不变。
这些是当前能力边界，不能据此证明任务无竞争。

## 验证

build通过，install同步后cmp一致；P0三反例及控制在32/64位各10项逐条断言；
返回指针回归10项通过。新增9个最小例双架构共18项断言：

- 完整发布helper后读最终值、重复返回helper：UNKNOWN→FALSE。
- 读helper中间值、错误返回值、同flag互斥：均未确认FALSE；参考工具PASS。
- 未闭合目标、嵌套、bodyless、atomic根入口：UNKNOWN。
  根入口变体旧版FALSE，本轮显式回退；它是有竞争的边界例，不宣称旧反例已证明虚假。

参考工具对两个正例和内部目标边界均FAIL（exit13），三个安全对照PASS，覆盖双架构。
完整发布正例的3个内存事件以及read_write_lock两个候选的17/11个内存事件
固定映射到参考模型后SAT，修改存值后UNSAT_AT_BOUND；仍未绑定完整CFG、局部值、
同步历史和IKOS总调度，**不是完整独立反例验证**。

六个新TP的当前具体候选：

| 文件 | IKOS确认的访问 | 参考native结果 |
| --- | --- | --- |
| read_write_lock-2b.i | 两个reader，r的非原子减计数访问；另有y访问 | FAIL |
| 30_Function_Pointer3_vs.i | thr1读res / thr2写res | FAIL |
| 32_pthread5_vs.i | thr1写lock / thr3写lock | FAIL |
| 41_FreeBSD_abd_kbd_sliced.i | 两个thr1实例，对COND的清零/读取 | FAIL |
| 43_NetBSD_sysmon_power_sliced_2.i | 两个thr1实例，对COND的清零/读取 | 60秒超时；180秒重试FAIL（113.8秒） |
| 47_ticket_lock_hc_backoff_vs.i | main中的thr1读取s / 创建的thr1递增s | FAIL |

六项均重新跑native DATARACEFREEDOM、bound2、ILP32、固定参考版本：五项FAIL，
NetBSD项首轮60秒超时，180秒上限重试在113.8秒得到FAIL；首轮超时记录保留。源哈希及命令
在证据中。全量配对没有只比较计数，也没有用名字指定某项能否确认。

## 剩余问题

先选的参考竞争任务 `01_inc.i` 仍UNKNOWN：helper可展开之后，候选被atomic内部
目标与搜索预算阻塞。下一步不能只移除open gate：需要目标与路径终点分开、
真实尾部闭包、不可分割性和竞争关系一致建模。Dartagnan把atomic体事件标记为RMW，
它的CAT竞争关系与IKOS全部事件scheduler clock并不等价；这一点须先做最小验证。
其它对象/指针来源、必要第三线程、join身份、完整独立验证和SAFE规则审查仍待完成。
原始日志、DB、参考IR和编译产物均在 `/tmp/ikos-named-atomic-2026-10-09/`。
