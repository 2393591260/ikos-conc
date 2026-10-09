# 启动前缀的逐次数组地址

从参考工具已确认竞争的 `09_fmaxsym-race.i` / `11_fmaxsymopt-race.i` 出发，
main 先循环初始化六个 storage 元素，再重复创建 worker。之前 conditional_live_threads
直接拒绝静态 var_id 非零的访问，执行中的常量循环下标也因此无法建立创建前缀。

现仅在已有有界 AR 路径中，复用 pointer_on_path / constant_integer_on_path，按每次访问之前
的动态 reaching definition 解析地址，并把结果保留在 startup。仍要求常量偏移、已知对象
范围和宽度；未知/共享 load 下标、越界、溢出、未定义来源或不支持的同步仍不确认。
没有删除原 gate，也没有按 benchmark 特征提供初值。

启动前缀选择 main 连续执行、孩子暂不调度的合法子集。worker 的 demanded load 继续使用
同一 startup 的 SC 初始化供值；两线程不能各自猜不同初始化值。SAFE 过滤未改变。

双架构正向循环和倒序 helper 变体 UNKNOWN→FALSE；startup 偏移分别为 0/4/8 和 8/4/0，
两个 worker 读到的最后元素都为 5。初始化 nondet 数组、两个线程分别检查值等于/不等于 0
的安全对照仍 UNKNOWN，Dartagnan PASS；两个正例在参考工具均 FAIL。
指定实例、访问对、静态对象/偏移/宽度和读写值的部分参考约束均 SAT，改值后均 UNSAT_AT_BOUND。
这仍不是全部 CFG、局部值、同步和调度的独立验证。

本次 stock Dartagnan bound=8：fmaxsymopt FAIL，fmaxsym 90 秒超时；后者已有冻结集参考 FAIL
记录（输入 SHA256 相同）。不能用本次超时替代或否定原记录，也没有据此宣称速度优势。
IKOS fmaxsym 在 ILP32/LP64 均 FALSE，fmaxsymopt 仍 UNKNOWN，后续需继续定位其他阻塞。

build、install 同步、双架构 P0 共20观察及全量1029配对通过：
**TP59→60（标签统计、未全部独立验证）/ FP0→0 / TN698→698 / FN0→0 / UNKNOWN272→271 / error0→0**。
唯一转移为 fmaxsym，其他1028判定相同。访问/值模型扩大，但不宣称所有 TP 已独立验证。

[精简证据](artifacts/bmc-startup-address-2026-10-09/summary.json)。回归：

```bash
python3 -B docs/review-repros/bmc-startup-address/run.py --output /tmp/ikos-startup-address-NEW
```

原始日志和前后二进制留在 `/tmp/ikos-startup-address-2026-10-09/`，不进入 git。
