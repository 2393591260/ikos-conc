# stdio 外部访问遗漏：四类最小漏报及保守修复

SAFE 规则审查发现 is_stdio_output 整类跳过 printf/fprintf/sprintf/snprintf/puts 等。
“指针只是输出的值”不是访问契约：snprintf/sprintf 写目标缓冲区，%n 写指针指向的整数，
%s 读字符串；每类都可能与另一线程普通写竞争。
另一个过滤按 `.xxx` 符号名猜常量，C asm 符号别名可使可写数组被错认只读。

四个最小例均在原基线 ILP32/LP64 报 SAFE：snprintf、printf %n、printf %s，以及
helper 调用 sprintf 写 `.writable` 数组。修复后 default 报抽象竞争，BMC模式 UNKNOWN，
不会把保守外部访问候选当成已验证 FALSE。共享锁缓冲区和字面量/整数输出对照仍 SAFE。
六例 × 两架构 × 两模式共24断言通过，另检查双架构P0共20观察。

删除整类stdio豁免，复用已有 constant_output_call，仅认可已证明不可变、终止的
字面量及受支持整数格式的无用户内存写摘要。其他指针参数恢复原通用保守候选记录，
宽度未知保持may-overlap；LLVM isConstant 属性取代符号拼写的只读推断。
未添加文件名、benchmark家族或标签例外。没有实现通用 printf格式/va_list 的精确访问模型。

依据为本地 Linux man-pages printf(3)：sprintf/snprint 写str，%s读取字符数组，%n存储字符数。
本地 Goblint `bb55b70d91290992629ba3d073185c10cdf356e6` 的 libraryFunctions.ml:62–65
区分buffer写/format读/varargs读，可作分层摘要参考；其varargs读摘要不是%n写的证明。
固定 Dartagnan `7caef5c` Intrinsics.java:232 将printf替换为inlineAsZero，%n最小例实测PASS。
故该版本不能验证这类库效果，不能根据它的PASS撤回源程序契约明确的竞争。

build/install一致，全量1029配对：**TP60→60（标签统计、未全部独立验证）/ FP0 / FN0 /
TN698→694 / UNKNOWN271→275 / error0**。全部60项原FALSE保留；仅四项SAFE→UNKNOWN：
04-mutex_36-trylock_nr、reorder_2、reorder_5、twostage_3。
这是精度代价，后续须按真实stdio参数效果/路径恢复，不能重加整类豁免。
[精简证据及逐任务变化](artifacts/stdio-effects-2026-10-09/summary.json)。回归：

```bash
python3 -B docs/review-repros/stdio-effects/run.py --output /tmp/ikos-stdio-effects-NEW
```

SAFE 审查仍未完成：其他库家族过滤（如sem_getvalue输出）、间接调用多目标处理、
结构偏移过滤、动态锁身份及抽象域覆盖均需明确契约与反例检查。
这次修复只关闭已复现的stdio/只读识别缺口，不宣称其他规则全部正确。
