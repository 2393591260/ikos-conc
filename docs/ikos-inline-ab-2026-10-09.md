# mark_internal_inline 单独 A/B

只撤回 `f.removeFnAttr(Attribute::NoInline)`，其他 pass、库成员、Python 管线及分析器
完全相同。`AlwaysInline` 本来就在上游，不能把本轮新增描述为首次加入该属性。
原源码/构建/安装未修改；在临时目录编译一个 pass 对象，替换复制 archive 的对应成员后链接。
构建和安装的当前 ikos-pp 完全相同。脚本 `scripts/build_inline_ab.py` 可复现隔离构建。

两端按当前冻结集的相同命令完整运行1029任务、jobs=4、BMC+demote，结果 **全部一致**：
TP=58、FP=0、TN=698、FN=0、UNKNOWN=273、error=0。历史11项TN回退在两端均UNKNOWN。
双架构P0的20条观察也逐项检查：exclusive不确认FALSE，race输入不返回SAFE。

原因由调用链确认：mark_internal_inline 位于 `ikos_pp.cpp` 的 `if (InlineAll)` 中，
Python `--inline-all` 默认False，当前全量命令未打开。因此移除NoInline不是这11项回退的原因。
这不归因其他前端改动（包括freeze_uninit），也不证明11项回退已修复。

活动控制另外验证构建确实不同：带取地址noinline helper的LLVM最小程序，不开inline-all
两端均通过；打开时当前版本通过，保留NoInline版本因noinline/alwaysinline冲突被LLVM拒绝。
普通直接helper在两端均可内联。当前修复应保留，不为了追回TN撤回此行。

[配对证据](artifacts/ikos-inline-ab-2026-10-09/summary.json) 保存二进制/源码指纹、命令和
11项逐任务状态；激活控制另存小JSON。原日志/对象/可执行文件在
`/tmp/ikos-inline-ab-2026-10-09/`，不入git。本轮用于判定归因，未测量可宣称的性能收益。

下一步继续追踪候选覆盖/共享aggregate干扰及其他语义变化，恢复TN需保留已修复FN的变体。
