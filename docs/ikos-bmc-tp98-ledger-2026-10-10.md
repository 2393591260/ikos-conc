# TP98之后的逐任务台账（2026-10-10）

`e0e9d3c`冻结集复跑TP98/FP0/TN696/FN0/UNKNOWN235/error0。
剩余235项重新诊断，全部逐项UNKNOWN，错误0。保留300条历史任务并
加入锁入口修复的一项TN回退，共301条，源码哈希逐项核实。
[逐项记录](artifacts/bmc-tp98-ledger-2026-10-10/results.json)及
[统计](artifacts/bmc-tp98-ledger-2026-10-10/summary.json)。

剩余参考FAIL107/TIMEOUT73/ERROR25/PASS29/UNKNOWN1。历史参考仅在
源码哈希一致时沿用，本阶段不是235次新参考分析。fib7/10/11/12
八个恢复记录更新为固定参考原生竞争证据；旧TIMEOUT阶段报告保留。
新nested-lock回退双架构参考均60秒TIMEOUT。剩余107个参考竞争不是
不可实现的证明，也不是预计可直接恢复的收益。

实际观察任务数（可重叠）：thread_activation_unproven 114，
call_summary 91，target_not_reached 79，search_budget 62，
non_global_object 50。阶段/门计数不能换算恢复收益，义务见每行。
原始报告留临时目录，结论JSON小于1MB，没有提交原始日志/数据库。

仍未完成全部TP完整独立验证、SAFE过滤完整证明、历史frontend TN影响
隔离及稳定全局性能比较。新发现partial-width共享guard变体，
Dartagnan双架构SOURCE_RACE，IKOS SAFE，正在独立修复；不能把冻结集
FN0说成没有正确性问题。后续精度扩展继续给这些义务让路。
