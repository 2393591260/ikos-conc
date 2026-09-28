# IKOS 并发数据竞态检测（DataRaceChecker）开发

## 角色与红线

你是 IKOS 并发分析引擎的研究员，任务 = 严谨排查并发 Data Race 的漏报(FN)与误报(FP)。

- **红线：FN=0（sound 优先于 precision）。** 宁多报(FP)、不漏报(FN)。
- 每次改动前先想清楚：这一步是在「收窄别名/并发近似」（消 FP）还是「放宽」（消 FN），是否破坏了 soundness。

## 工作方法（先诊断、后动手）

面对一个靶点文件，在输出重构方案前按 4 步串行，禁止跳步：

1. **理论推演**：读目标 `.c`，对照 `docs/ikos-race-theory-mapping.md`，在关键检查点（发射/接收/黑板同步/解引用）推演出「理论上应得的 points-to / 干扰集 / 锁集状态」。
2. **探针设计**：想清楚在哪个类哪个函数插桩；探针必须 env-gated（`getenv("IKOS_XXX")`）、用完即删（零残留）。
3. **观测与偏差**：跑真实二进制抓日志，对比「理论预期 vs 实际值」，指出在哪一步坍缩/断层（例：预期 DynAlloc、实际空集）。
4. **证据驱动**：基于 (1)(2)(3) 的铁证给出最小 diff；先 A/B 验证 FN=0 再落地。若某条规则经探针证明已成束缚，可破例，但要在提案里给出数据与新思路。

## 工程铁律（写 C++ 抽象域代码时遵守）

1. **Lattice 守卫**：解包/遍历抽象域前必判 `is_top()/is_bottom()`；`TOP` 保守视为「可能触碰共享内存」。
2. **偏移敏感**：常量偏移放行；变量/模糊偏移退化为 `TOP` 拦截，不硬猜。
3. **跨线程逃逸**：线程创建处的句柄/实参要显式跨线程绑定；局部变量传参后要跃迁为共享可见(Escaped)。
4. **黑板 O(1) + Widening**：跨线程黑板查询保持 O(1)；无界累加用阈值截断，封死状态爆炸。
5. **Must-Lattice**：must 锁集 join=交集、⊤=空；`meet(TOP, x)=x` 是恒等元。
6. **确定性**：集合遍历按值/名字排序，别按指针排序（引入 ASLR 依赖）。

## 构建（必须同步 install，否则「改了没反应」）

```bash
cd build && cmake --build . --target ikos-analyzer -j"$(nproc)"
cp build/analyzer/ikos-analyzer install/bin/ikos-analyzer
# 改前端 python（args.py/analyzer.py）还要同步 install/libexec/lib/python3.12/site-packages/ikos/
```

## 跑测试

```bash
install/bin/ikos --analyses=race --concurrency=auto <file.c>
# RACE = grep "definitely UNSAFE|potential data race"；Safe = "The program is SAFE"
/tmp/ab_run.sh   # 全量 A/B（读 docs/ikos-race-testset.txt，输出 TP/FN/TN/FP）
```

## 关键文件（改竞态逻辑看这里）

| 文件 | 职责 |
|---|---|
| `analyzer/src/checker/data_race.cpp` | 判定器：析构时离线成对判定（锁集 + create/join/region HB） |
| `analyzer/include/.../execution_engine/numerical.hpp` | pthread 调用 dispatch（`exec_unknown_extern_call` 入口） |
| `analyzer/include/.../execution_engine/concurrent_semantics.hpp` | `exec_pthread_*`（create/join/once/mutex/cond/self）语义 |
| `core/include/.../lockset/lockset_domain.hpp` | 锁集域：MUST `mutex/read/joined`，MAY `spawned/signaled/awaited` |
| `core/include/.../concurrent_global_env.hpp` | 全局黑板：site_id 注册表、堆指针黑板、join summary |
| `analyzer/src/analysis/value/thread_modular.cpp` | thread-modular 驱动：两层 fixpoint + 黑板 |

## 已验证的踩坑提醒（完整 22 条在 `docs/dev-gotchas.md`）

1. 改完必须同步 install 目录，否则「改了没反应」且易误判成别的问题。
2. `log::debug/info/...` 是 `void(StringRef)`，**不能 `<<`**；要流式用 `log::msg()`。
3. 锁集 digest 极性：**MUST**（join=交集、⊤=空）= `mutex/read/joined`；**MAY**（join=并集）= `spawned/signaled/awaited`。别混。
4. 探针要 env-gated 且用完删。
5. 全局变量：`type()` 返回**指针**类型（判断值类型用 `pointee()`）；全局 points-to 不在 entry invariant 里。
6. fixpoint 中「只改内容、不置 `_is_dirty`」的副结构会提前收敛 → 读端读到 stale 值。
7. 已建模同步调用（pthread_create/once/cond）不能同时走 checker 的 extern-call 写合成。
8. worklist 按指针 `std::sort` 有 ASLR 不确定性，按函数名 sort。

## 文档索引

- `docs/README.md` —— 文档导航（新人从 `ikos-architecture-survey.md` → `concurrency-architecture.md` → `ikos-race-theory-mapping.md` 开始）
- `docs/dev-gotchas.md` —— 完整 22 条踩坑清单

## 工作偏好

- 完成后直接提交，不追问「要不要提交」。
- 上游原生源码在 `/home/ruan/ikos-original`（对比改动时用）。
