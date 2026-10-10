# 有限全局 points-to 集合用于候选选择，实际路径证明对象

`06-symbeq_10-equ_rc.c` 的 main 根据 nondet 选择 A 或 B，锁住选中对象的
mutex 后写其 datum；worker 锁 A.mutex 却写 B.datum。当 x=0 时 main 选 B，
两线程锁不同、写同一 B.datum，存在竞争。旧门因 main 的 points-to 非 singleton
拒绝 BMC，返回 UNKNOWN。固定 Dartagnan 本轮重查源程序 FAIL/exit13。

本次允许有限、非空、全部为 global 的 points-to 集合进入 BMC。AI may-alias
只产生候选，不充当实际地址证据。展开后的 pointer_on_path 仍逐定义追踪选中
对象；双方实际 base 必须相同，值守卫、对象边界、访问宽度、同步及动态创建
约束全部保留。另一条 main 选择 A 的路径因实际 base 不同被拒绝。heap、TOP
和不受支持的共享 pointer load 没有因此放开。TLS 仍在对象范围查询处拒绝。
这直接复用 IKOS 的有限别名候选与 AR 路径结构，未增加全程序 SMT 编码。

八个最小例，双架构共16条断言：竞争、helper、同入口多实例和不同宽度重叠
恢复 FALSE；不同对象和公共锁对照 SAFE；互斥守卫不确认 FALSE；共享 pointer
load 仍 UNKNOWN。20条 P0 观察与20条分配锁身份回归逐项相同。build、install
同步/cmp 成功，无前端改动。

外部参考：竞争／helper／多实例及共享 pointer 例原生 FAIL；三个安全对照
原生 PASS。**重叠宽度例的固定 Dartagnan 却 PASS**：该版本竞争公式以起始
地址 sameAddress 判冲突，不能覆盖不同起点的字节重叠。该例依据 C 的重叠
访问与本项目宽度回归，不写成参考确认。真实基准两目标的地址、写值和相对
顺序固定后参考 SAT；错误对象／offset／值被拒绝，见
[间接地址验证](ikos-bmc-indirect-address-validation-2026-10-10.md)。仍为部分
模型检查，没有增加完整独立反例验证数量。

全量1029项 ILP32 配对有运行波动，完整保留两轮，不能只报净增长：

- 第一轮：TP77/FP0/TN697/FN0/UNKNOWN255/error0。对象选择 UNKNOWN→FALSE，
  fmaxsymopt 与 lu FALSE→UNKNOWN。
- 同配置重复：TP78/FP0/TN697/FN0/UNKNOWN254/error0。对象选择仍恢复，
  fmaxsymopt 保持 FALSE，lu 仍 UNKNOWN。
- 两项下降分别做两轮隔离旧／新二进制检查，八次都 FALSE。旧快照为105b986，
  分配锁身份改动对这两个仅用 global 锁的程序不适用；这不是精确性能 A/B。
  现有证据不能确定全量变化来自资源竞争、计时预算还是求解启发式。

因此能声称对象选择这一项稳定恢复，不能声称稳定净 +1 或速度改善。
保留 lu 的全量回退为待解决搜索稳定性问题；不将隔离结果替换全量结果。
LP64仅定向测试。标签 TP 不等于完整反例已验证。
[汇总](artifacts/bmc-global-choice-2026-10-10/summary.json)、
[重复全量配对](artifacts/bmc-global-choice-2026-10-10/global-choice-final-full-repeat-paired.json)。

另检查同一 loop malloc site 再次分配后，通过缓存 old pointer 写已发布对象的
最小例：双架构 default 均保留竞争警告、BMC 均 UNKNOWN，没有复现 SAFE 漏报。
未据此修改 freshness，也没有从这一观察推导其全部规则正确。
