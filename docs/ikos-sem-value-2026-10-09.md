# sem_getvalue 的共享输出槽

同库访问审查进一步发现 sem_ 整类过滤错误假定 sem_getvalue 的 int* 输出永远局部。
两个线程分别 sem_getvalue(&sem, &output) 和 output=1 的最小例，在原基线双架构均 SAFE。
POSIX契约（本地sem_getvalue(3)）明确会存储当前值到sval；有效初始化的sem不消除输出竞争。

现保留该API的用户输出候选；已解析为确切声明、两实参时，只豁免同步对象arg0。
输出arg1仍按points-to、共享性、范围、共同锁和joined规则处理，未知宽度保持may-overlap。
没有为任意输出创造自由值；一般sem返回/值流未建模，BMC UNKNOWN而非凭候选确认FALSE。
共享输出、局部输出、helper内调用且同锁访问的对照，双架构default/BMC共12断言通过。

build/install、P0共20观察、完整1029配对均通过：
**TP60→60（标签统计、未全部独立验证）/ TN694→694 / UNKNOWN275→275 / FP/FN/error0**，
全部1029判定相同。[精简证据](artifacts/sem-value-2026-10-09/summary.json)。回归：

```bash
python3 -B docs/review-repros/sem-value/run.py --output /tmp/ikos-sem-value-NEW
```

其他sem_*的时间参数/名称字符串，以及其他API家族/多目标间接调用过滤，仍需逐项审查。
本次不把修复一个明确输出槽称为已证明所有库摘要。
