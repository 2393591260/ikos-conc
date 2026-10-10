# helper目标搜索实验：配对回退后撤回

本次固定Dartagnan重新确认qrcu-1、qrcu-2、gcd-2原生竞争，尝试减少helper
展开的重复工作：保守静态调用图若排除目标所在AR函数，跳过helper内寻找
该目标的遍历，仍枚举真实Return并保留效果；间接调用未知时保留原搜索。
没有搬入参考工具的全程序模型、提高预算或改变值／地址／同步公式。

build/install/cmp、20条双架构P0观察、28条atomic-target及14条helper断言
通过。nested／callback目标保留，mutex与共享守卫对照不确认竞争，未知效果
保持UNKNOWN。六个正例／对照双架构固定参考分别FAIL／PASS。三个目标
benchmark仍UNKNOWN，未形成恢复它们的精度证据。

1029项全量总数仍TP80/TN697/UNKNOWN252、FP/FN/error0，但逐文件为07_rand
UNKNOWN→FALSE，ticket_lock_hc_backoff_vs FALSE→UNKNOWN。仅看TP总数会
掩盖旧竞争回退，所以本实验源码已撤回。局部七分支helper的三轮计时样本小，
也不足以证明稳定全局提速；不为尚无收益证据的优化留下额外架构。

[全部配对](artifacts/bmc-helper-search-experiment-2026-10-10/paired.json)、
[哈希、参考、回归及计时](artifacts/bmc-helper-search-experiment-2026-10-10/summary.json)。
小helper／callback回归作为以后改变调用展开的对照保留，原日志在临时目录。
该实验不代表这些UNKNOWN不可恢复；完整独立验证仍未完成。
