# 创建基数：单次执行证书与单调发现

从三个 join SAFE 回退和新主递归反例审查创建基数。此前只检查 create 所在基本块能否
回到自身，main内非循环语句即计1，所有helper计2。main递归两次执行同一create，两个worker
写同一data，双架构均错报SAFE；helper只调用一次、正确join后的访问则过度保守UNKNOWN。

现按普通闭合AR调用图建立至多一次执行的充分条件：当前函数地址没有逃逸、全程序只有
一个直接调用点、该点不在CFG循环中、调用者递归满足同一条件，最终到没有显式调用/地址
引用的隐式main。递归/多调用点/bitcast或其他地址引用/聚合常量中的函数指针/深度预算
不满足证明时计可能重复。该证书缓存于静态程序、clear时清空；不得当作may执行次数证明。

同时发现函数指针表helper反例：早期迭代发现create，后续抽象回放失去目标后把spawn
计数重置为0，持久thread registry里的worker反被当成唯一实例，双架构SAFE。
现create静态may-sites和计数跨迭代单调保留，按site插入去重；新增site使顺序创建缓存失效。
实际pthread_create传入原创建语句供证书检查，wrapper用于身份的调用点不冒充真实create。
未知extern的函数指针回调不能因extern只调用一次而假定callback仅执行一次，保守计重复。

主递归反例双架构SAFE→FALSE；函数指针表反例SAFE→UNKNOWN，pointer rf/callee仍未支持，
不会仅凭候选升FALSE。重复helper/循环helper原FALSE保持；一次helper+join恢复SAFE；
连续helper各自create/join的对照仍UNKNOWN，没有误确认。六例双架构共12断言通过。
固定Dartagnan bound=3各架构四个正例均FAIL，两安全对照均PASS。它确认源程序存在竞争，
不能替代对IKOS所选完整值/实例/调度模型的独立验证；递归creator映射仍有缺口。

build/install一致；双架构P0、stdio 24断言、sem 12断言通过。完整1029配对：
**TP60→60（标签统计、未全部独立验证）/ TN694→697 / UNKNOWN275→272 / FP/FN/error0**。
唯一变化为race-1_1/1_2/1_3-join恢复SAFE，另外1026项相同。
此前11项TN回退已恢复其中3项，另外8项及stdio新增4项仍跟踪。
[精简证据](artifacts/single-execution-2026-10-09/summary.json)。回归：

```bash
python3 -B docs/review-repros/single-execution/run.py --output /tmp/ikos-single-execution-NEW
```

证明依赖普通AR控制流/调用覆盖；未知extern、汇编和未建模语言/运行时行为的覆盖仍属于
全局SAFE审查范围，本增量不宣称全部源码/库边界已被证明。当前不增加K线程SMT或重写前端。
