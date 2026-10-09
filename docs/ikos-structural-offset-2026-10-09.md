# 未知基址：类型和相对字段偏移不是地址不重叠证明

原 structural_field_of 对 offset=TOP 的访问剥 PointerShift，取相对字段偏移和根指针
pointee类型；两类型一致、偏移不同即删竞争候选，并把相对偏移填成BMC对象绝对区间。
缺少的前提是共同对象原点：基址差未知时，相对4与相对0可能指向同一字节。
宽度检查也不能补足基址前提。

具体有效的地址布局是 union Storage：一个成员是 Pair left，另一个成员是
struct { int pad; Pair right; }。left.y 和 right.x 都位于全局对象的字节4。
两个 getter 分别返回各自嵌入Pair的地址，worker各写这个int，会竞争。
闭合源程序明确给出getter实现，固定Dartagnan bound=2双架构均FAIL；
另一个普通Pair的x/y不同字段对照均PASS。IKOS闭合版本来就保留候选，BMC仍UNKNOWN。

只有同样签名的不透明getter声明时，IKOS无法排除上述有效提供者，旧版却双架构、
两模式均SAFE。此不透明例检查开放外部调用的保守覆盖，没有宣称参考工具验证了其
缺失实现。其合法返回集包含闭合版的重叠地址，因此不能借两个返回值的Pair类型删候选。

现在删除相对偏移的安全过滤、BMC绝对区间替代及相应字段/提取器，共删除89行、增加7行。
已知抽象对象偏移仍按访问宽度排除不重叠范围；offset未知保留候选，BMC须建立实际
地址和来源后才能确认。今后若恢复这类过滤，必须证明对象原点/基址位移及完整字节范围。

三例×双架构×两模式12配对断言：不透明例SAFE→默认抽象竞争/BMC UNKNOWN；
闭合竞争和不重叠安全对照结果保持。build/install/cmp、双架构P0 20观察逐项断言通过。
1029冻结集ILP32逐项不变：**TP61→61（标签统计、未全部独立验证）、TN697、
UNKNOWN271、FP/FN/error0**。[精简证据](artifacts/structural-offset-2026-10-09/summary.json)。
原日志在 `/tmp/ikos-structural-audit-2026-10-09/`，不提交。

```bash
python3 -B docs/review-repros/structural-offset/run.py --output /tmp/ikos-structural-offset-NEW
```

闭合竞争仍因返回指针来源未展开而UNKNOWN，是下一项可定位的通用能力缺口，
不能用旧相对区间猜测填补；其他SAFE过滤和完整反例独立验证仍未完成。
