# 部分标量写的干扰宽度审查（2026-10-10）

冻结集1029项ILP32配对逐项不变：TP98/FP0/TN696/FN0/UNKNOWN235/error0。
新正例双架构曾错误SAFE，修复后UNKNOWN，完整正确性证明仍未完成。

## 反例及根因

共享`short flag=0`，writer持mutex经`signed char*`把低字节写成-1，
解锁后写data；reader持同一mutex读flag，解锁后在flag==255时写data。
本机x86 ILP32/LP64小端布局，合法char别名写后的short位图0x00ff，
值255。writer写flag/解锁，reader读255/解锁，两条data写没有HB，
存在竞争。flag自身被共同锁保护，避免其他竞争掩盖消失的data路径。

`on_store_privatize`此前只记录RHS区间[-1,-1]，没有写宽度及起始offset。
入口把8位-1数字扩展成16位-1，合并初值0得到[-1,0]，删除flag==255
分支，错误SAFE。改名/helper同样复现。固定Dartagnan bound2先确认
低字节及helper双架构SOURCE_RACE，才修改分析器。

高字节写1实际得到256，其相同条件变体也错误SAFE。原生gcc -m32/-m64
三条assert证明低字节255、高字节256、已有高字节1再写低字节FF得到511。
race论证仍是共同锁保护flag后，data写都在release之后，无跨目标HB；
原生值assert是表示验证，不是完整竞争见证validator。

## 参考分歧必须保留

固定Dartagnan在高字节变体返回SOURCE_PASS（双架构），不是SOURCE_RACE。
其processed事件为`load(&flag):bv16`及`store(&flag+1,bv8(1))`。
`encoding/EncodingContext::sameAddress`比较起始地址相等；
`encoding/WmmEncoder::visitReadFrom`的rf要求sameAddress/sameValue。
这份固定模型/配置没有把高字节写拼进起始地址0的16位读，不能作为这项
混合宽度C语义的通用oracle。保留PASS及实际事件，不隐去参考不一致。
不由此推导所有Dartagnan版本/模式都错误，也不直接搬其word值模型。

## 通用修复

实际ar::Store透传到唯一on_store_privatize调用者。全局整数摘要仅在
RHS同位宽整数、offset精确0时表示覆盖整个标量，按目标全局类型做
同宽符号转换。部分/移位/未知offset或非整数来源改为该标量类型TOP，
覆盖所有可能结果，不能当数值符号扩展。数组/struct保留原aggregate
forget，pointer内容通道未扩展。未来可用字节区间/内存cell精化替代TOP。
无文件名识别、标签、BMC确认门、frontend或普通顺序分析行为变更。

## 验证与边界

build/install/cmp、新12条双架构断言、原46条SC、14条锁干扰、34条
atomic断言通过，P0双架构20条逐项相同。部分宽度三个正例均
SAFE→UNKNOWN，whole-word正例FALSE保留，对照SAFE保留；不可能低字节
条件对照SAFE→UNKNOWN，是保守精度损失，没有确认假竞争。
参考word正例SOURCE_RACE、两个对照SOURCE_PASS；高字节分歧见上。

BMC有值守卫的mixed-width rf仍拒绝：范围重叠不等于知道完整整数读值。
后续需byte-level来源、初始化和拼接，不能放开门或把读值编自由符号。
另测两个持锁条件分支不同整值的竞争变体，参考及IKOS双架构均确认，
没有将flush队列的潜在证明义务当作已复现FN或提交猜测补丁。

[配对](artifacts/bmc-interference-width-2026-10-10/paired.json)、
[统计](artifacts/bmc-interference-width-2026-10-10/summary.json)、
[参考](artifacts/bmc-interference-width-2026-10-10/native.json)、
[最小例](review-repros/interference-width/run.py)。

原生表示测试（仓库根）：

```sh
gcc -m32 -O0 docs/review-repros/interference-width/runtime/byte-semantics.c -o /tmp/byte32
/tmp/byte32
gcc -m64 -O0 docs/review-repros/interference-width/runtime/byte-semantics.c -o /tmp/byte64
/tmp/byte64
```

全部SAFE过滤/转移证明仍未完成，也未增加完整独立TP验证计数。
pointer部分写、Top目标写、一般heap及K线程语义仍有独立义务。
