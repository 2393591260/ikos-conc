# 堆／栈锁的分配位置不等于具体锁对象

同一个 worker 创建两次，各自在同一 malloc 或栈分配语句创建自己的 mutex，
然后锁住它并写普通 global data。两把锁互不排斥，存在竞争。旧抽象语义却把
单个 MemoryLocation 摘要当作共同 MUST 锁，ILP32/LP64 均错误 SAFE。
helper 返回 malloc 锁的变体也复现。只检查分配语句／调用上下文是否位于循环
不够：没有循环的线程入口仍可执行多次，栈对象也可以有多个实例。

本次在锁准入处要求位置能够证明只代表一个具体对象：普通非 TLS global
直接满足；heap／local 必须找到实际分配语句，且该语句不在 CFG 循环、所属函数
有从隐式 main 出发的直接、无环、单次调用链证书。复用 ConcurrentGlobalEnv
已有证明，函数地址逃逸、线程入口、递归、多调用点和调用循环均不通过。
多选符号锁的每个可能位置也必须满足这一前提。一个同时存活线程不等于一次
生命周期调用，不能拿线程唯一性替代这里的对象基数证明。

证明不了时不加入 MUST／条件锁，保留 may-warning，BMC 在能力边界内返回
UNKNOWN；不把一个摘要锁强行拆成猜测的动态身份。这会保守扩大候选集合，
不删除真实竞争。main 一次分配、发布给所有线程的 heap／stack 锁对照仍 SAFE。
没有前端改动或新的 BMC 对象模型。

验证完成：build、install 同步/cmp；五个最小例的双架构/default+BMC 共20条
断言；既有20条 P0 观察逐项相同，28条创建及24条 TLS 回归逐项相同。
固定 Dartagnan `7caef5c850dc2dbf81c5554dcf41e402007f7a19`，bound=2，
三竞争例双架构均原生 FAIL/exit13，两对照均原生 PASS/exit0。
这确认源程序竞争／对照结果，不是原 IKOS 模型的完整独立重放。

冻结集1029项 ILP32 输入集合与结论逐项不变：
TP78/FP0/TN697/FN0/UNKNOWN254/error0。LP64仅做定向回归，没有全量或性能测量。
[小证据](artifacts/lock-allocation-identity-2026-10-10/summary.json)、
[逐文件配对](artifacts/lock-allocation-identity-2026-10-10/paired.json)、
[最小回归](review-repros/lock-allocation-identity/run.py)。

这修复已复现的分配身份漏报，不构成全部 SAFE 过滤规则的证明。下一步继续
审查重复分配的 fresh 摘要与发布信息，再恢复暂缓的全局对象选择精度工作。
