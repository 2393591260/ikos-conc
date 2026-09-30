# IKOS 开发踩坑清单（dev gotchas）

> 从会话记忆固化而来，改并发/竞态代码前先扫一遍。


IKOS 开发时查代码不易一眼看出的隐性约定,容易反复踩坑。

1. **改完必须同步 install 目录**。前端 `build/analyzer/script/ikos` 实际用的是 install 里的二进制和 Python 模块,不是 build:
   - 后端:`cp build/analyzer/ikos-analyzer install/bin/ikos-analyzer`
   - 前端 py:`cp analyzer/python/ikos/{args.py,analyzer.py} install/libexec/lib/python3.12/site-packages/ikos/`
   不改会"改了却没反应",且极容易被误判成别的问题。

2. **log / AST 遍历两个易错点**:
   - `log::debug/info/warning/error()` 是 `void(StringRef)`,**不能流式 `<<`**;要"流式 + 受 log-level 门控"只能写 `if (log::is_enabled_for(LogLevel::X)) { log::msg() << ...; }`。`log::msg()` 本身又无条件输出。
   - `isa<T>(nullptr)` 触发断言崩溃;`unique_def()` 对形参/phi 返回 nullptr,所以 `isa<T>(unique_def(x))` 必须先判非空,或用 `dyn_cast_or_null`。

3. **锁集域 digest 有 MAY/MUST 两种格极性,方向相反,别混用**:
   - **MUST**（join=交集、meet=并集、⊤=∅）: `_mutex_locks`、`_read_locks`、`_joined_threads`（已 join 的线程）。
   - **MAY**（join=并集、meet=交集、⊥=∅）: `_spawned_threads`（可能已 spawn 的线程）。
   创建边 HB 的 `!spawned.count(child)` 要的是「确定未 spawn」= MAY 语义;若把 spawn 当 MUST（旧代码用 `"s:"` 前缀塞进 joined 集共享 MUST 格）,条件 create 合并点 MUST 被交集成空 → 误判「父没 spawn 子」→ 把真竞争当 HB 抑制（FN）。

4. **锁集域是 `DomainProduct2` 的第二分量,经 `inv.second()` 直连、不再走 forwarder**(重构 `f5c0fe6`):顶层 `AbstractDomain = DomainProduct2< ExceptionDomain<MemoryDomain>, LocksetDomain >`,`inv.first()`=数据域(`.normal()`/`.caught_exceptions()` 等)、`inv.second()`=锁集域(`held_locks`/`held_read_locks`/`cond_locks`/`get_joined_threads`/`get_spawned_threads`/`spawned_is_top` 等)。加锁集方法只改 `LocksetDomain` 本体 + numerical 侧经 `inv.second().xxx()` 访问;**不要再往 `memory/*` / `scalar/composite` 加 `lockset_*` 转发**(那套 5 层 forwarder 已随重构删除、恢复 ikos-original)。engine 里数据/锁集双态访问用 SFINAE 的 `data()`/`lockset()`/`data_of()`(`has_lockset` trait 兼容指针分析 `ExceptionDomain<DummyDomain>` 实例化)。`post` 初始化要 `post.first().set_normal_flow_to_bottom()` **加** `post.second().set_to_bottom()`(只 bottom 数据域会留锁集 ⊤,`⊤ join {lock}=⊤` 丢 must 锁 → FP)。

5. **写 C 边界测试时,`int x = glob; (void)x;` 会被 clang 当死代码优化掉**(读被消除 → 测不到竞争 → 假 OK)。要强制活读:把值写进另一个全局 `other = glob`,或用于条件分支。boundary-tests/ 里反复踩过。

6. **`.i`(预处理) 与 `.c`(源码) 在同一管线下判定可能不同**;评测的 ground truth 是 `.c`(clean-sv-benchmarks)。用 `.i` 复现的 verdict 不能当回归基准。

7. **判定有调度不确定性**(tbb parallel_reduce 的 join 顺序敏感),borderline 哨兵会 run-to-run 翻转;「一个修复翻了一个文件」要复跑确认,别把单次翻转当结论。

8. **区间 join 前必须 bit-width/sign 规范化,且两个操作数都要规范**:跨宽度 join 会 `assert_compatible` SIGABRT 或漏报。`ConcurrentGlobalEnv::join_into_cell_nolock` 曾是唯一漏规范处。

9. **weak-unlock 的「unknown target」路径绝不能 `lockset_set_to_top()`**:那会清掉 joined/spawn 两个 HB digest,让创建边误判、掩盖真 race。应逐个 `lockset_remove_lock`/`remove_read_lock` 具体锁。

10. **形参不是语法绑定**:内联后形参靠抽象域 `match_down` 绑定到实参,`unique_def(formal)` 返回 nullptr(不是 nullptr 就是「未知」)。要解析须沿 `cc->call()->argument(i)` 走 call-context(`FunctionPointerConstant::function()`==fun 时),否则 region/symbolic-index 追踪全空。

11. **取证插桩探针要 env-gated 且用完删**:临时 `cerr` 探针用 `getenv("IKOS_XXX")` 门控,取证后 grep `getenv` 在 data_race.cpp / numerical.hpp / concurrent_global_env.hpp 里清干净。

12. **两个「全局变量类型/points-to」隐性陷阱**(materialize_globals 曾因此整体 no-op):
   - `ar::GlobalVariable::type()` 返回**指针**类型(全局变量在 LLVM 里是 `i32*`),判断值类型必须 `gv->type()->pointee()`;写 `gv->type()->is_integer()` 恒 false → 所有整型全局被静默跳过。
   - **全局变量的 points-to 不在 entry invariant 里**(exec engine `init_global_operand` 注释明说"globals are not stored in the initial invariant")。直接在 entry invariant 上对 `get_global(gv)` 做 `mem_read/mem_write` 会因 `pointer_to_points_to` = ⊤ 触发 `mem_forget_all()` 把整个 invariant 炸成 ⊤(表象是别处某个 pointer load 的 points-to 变 ⊤ → 全线 FP)。要先 `pointer_assign(ptr_var, loc, Nullity::non_null())` pin 住,再读写。
   - 同理整型全局的**内容**是 memory cell,不是 scalar;`int_to_interval(get_global(gv))` 会断言(`ScalarVariableTrait::is_int` 显式排除 CellVariable)。读写内容要走 `mem_read`/`mem_write` 回环。

13. **「未知外部调用」破坏 soundness 有两条独立路径,诊断 FN 时别只盯锁集**:
   - **路径一·锁集塌缩**:`exec_unknown_extern_call` 的 `may_throw_exc=true` → `throw_unknown_exceptions()`(= `caught_exceptions().join_with(normal())`) 把 must 锁集搅成 ⊤ → 丢 must 锁 → FN。
   - **路径二·可达性收缩(更隐蔽)**:若未建模调用被 stub 成 no-op,且它在 `while(cond)` 循环里、循环体不改变 `cond`,则抽象不动点里循环**永不退出** → 循环后的代码在正常流判 `normal=⊥` → 其上的访问(如无锁读)漏记录 → FN。控制变量法能定位"是哪个变量触发",但**控制变量法剥掉 cond_var 时往往顺带剥掉了整个循环**,会误判成"cond_var 的锁集问题"。thread-join-counter FN 的真根因就是路径二。
   - **修法**:cond_wait 类"可能阻塞等待、期间环境可能改全局"的调用,stub 必须 `mem_forget_all()`(不是 `mem_forget_reachable`),让循环能退出;同时不带 `may_throw_exc`。两条路径各自独立,要**分别**堵。

14. **未初始化值 → `undef` → BOTTOM → 静默 FN（0 checks）**:
   - **`undef` 的出处是 ikos-pp 的 mem2reg**（`-opt=basic` 管线第一个 pass,`createPromoteMemoryToRegisterPass()`）,不是 clang：mem2reg 把「写之前先读」的 alloca 提升成 SSA 时直接插 `undef`。`undef` → `ar::UndefinedConstant`。
   - 分析器里 ~30 处 `has_undefined_constant_operand() → set_normal_flow_to_bottom()` 和 `uninit_assert_initialized()` 把「未定义/未初始化」当成「路径死(BOTTOM)」,这是 **under-approximation**：未初始化值本该是「任意值(TOP/nondet)」,剪掉路径就漏报（04-mutex_31/63、52-confid、06-symbeq_12、09-regions_33、51-threadjoins_05 都是这类 goblint FN）。
   - **根修法 = ikos-pp 加 `--freeze-undef` pass**：在 mem2reg **之前**给每个 i32/i64 alloca 插 `store @__ikos_nondet_int(64), alloca`,把 undef 变成**单一 nondet 值**（保住「同一 alloca 两次 load 同一值」的相关性）。前端 `library_function.cpp` 已识别 `__ikos_nondet_int/uint/int64/uint64` → `IkosNonDet`。由 `ikos` wrapper 在 `'race' in analyses` 时自动加 `-freeze-undef`。局部修法（dispatch pthread 到 uninit 检查之前）只覆盖 pthread 一类。
   - **未决缺陷**：undef→nondet 后,「create iff join」这类**路径敏感相关性**（10-synch_19/21/22 `if(x) create; if(x) join`）会因分析器路径不敏感而变 FP——goblint 用 `ana.path_sens[+] threadflag` 解决,IKOS 尚无,需补路径敏感才清零这 3 个 FP。
   - **诊断技巧**:「0 checks + SAFE」先怀疑可达性收缩（undef→BOTTOM）,不是锁集问题。看 `Total number of checks` 是「0」还是「>0 但都 safe」。

15. **pthread_t handle 是「取址局部变量」，值在 MEMORY cell 不在 scalar 域**（thread-instance join 改造踩的深坑）:
   - `pthread_t id2`（因被 `&id2` 取址）是 `ar::LocalVariable`，其 `type()` 是**指针**（`i64*`，alloca 产生指针）；`get_local(id2)` 返回的是**地址变量**（pointer-typed），不是内容。
   - 旧代码 `scalar_assign_nondet(get_local(id2))` 实际是给**地址变量**赋 nondet 指针，根本没动内容。要写 pthread_t 的**值**必须走 `mem_write(ptr_var=get_local(id2), machine_int(...), size)`；读走 `mem_read`/`int_to_interval(load结果)`。
   - 判别技巧：探针看 `var->type()->kind()`（4=PointerKind）与 `arg->type()->kind()`（2=IntegerKind，join 的 arg0 是值 i64）——取址局部的「地址」是指针、「值」是 i64，两视图不混。

16. **`exec_unknown_call` 的 may_write_params 只 forget `InternalVariable` 指针实参，跳过 `LocalVariable`（取址局部）指针实参**:
   - `foo(&id2)` 这种「把取址局部传给未知外部函数」的实参是 `ar::LocalVariable`（不是 InternalVariable），旧循环 `!isa<InternalVariable>(arg) → continue` 直接跳过 → **不 forget** → 后续 `pthread_join(id2)` 读到 pre-call 的 handle 值 → 把「可能被改写」的 join 误判成「确定 join」→ FN（51-threadjoins_07）。
   - 修法：循环里补 `LocalVariable` 分支，`ptr = get_local(cast<LocalVariable>(arg))` 后走同一套 forget。这是通用 soundness 补丁，不是 pthread 专用。

17. **goblint 判题 oracle 是逐行 `/RACE/`（无 `!`）**：`update_suite.rb` 里 `obj =~ /RACE/`（`// RACE`、`//RACE`、`// RACE!` 都匹配），`/NORACE/` 优先。按 `// RACE!`（带感叹号）grep 会漏掉 31 个用 `// RACE`（无 `!`）的文件，把它们误判成「期望 SAFE」→ FP 虚高。正确口径 = 逐行「含 RACE 且不含 NORACE」判 race 行，文件级 RACE iff 有任一 race 行，再加 `_rc`/`_nr` 后缀兜底。

18. **前端 `translate_getelementptr` 把 GEP 拆成两类 PointerShift 项，系数就是区分信号**：
   - struct 字段项：系数 **=1**（`1 * offsetof(field)`）；
   - 数组元素项：系数 **=元素大小**（`size * index`，index 是 CONST 或 VAR）。
   符号索引抽取若把两类都当「常量字段偏移」丢掉，`entry[1]`（元素 +1）会被当成 `entry->field`（同元素）→ 锁 {i} 错误保护 {i+1} → FN（06-symbeq_39）。修法 = 累加「系数>1 的常量元素项」进 `SymbolicIndex.const_term`（锁侧 `symbolic_index_of` + 数据侧 `region_collect` 都要做，两侧必须一致）。char 数组元素 size=1 无法区分，保持丢弃（不回归）。

19. **worklist / 函数集合按指针 `std::sort` 会引入「路径/ASLR 依赖」的确定性 bug**：`thread_modular.cpp` 曾 `std::sort(worklist)` 对 `ar::Function*` 按**指针地址**排序，指针地址随 ASLR 和「源文件相对/绝对路径」的 ModuleID 字符串长度变化 → 分析顺序变化 → MemoryLocation stable_id 分配顺序变化 → 堆指针黑板 fixpoint 收敛到**不同不动点** → 同一文件相对路径判 RACE、绝对路径判 Safe（09-regions_33）。修法：按函数名（确定性键）排序。凡「borderline 哨兵 run-to-run 翻转」先怀疑是否还有按指针/hash(指针) 的排序或遍历。

20. **fixpoint 中「只改内容、不置 dirty」的副结构会提前收敛 → 读端读到 stale 值**：`ConcurrentGlobalEnv` 的 `join_global_pointer`/`join_heap_pointer` 曾只把 points-to 集合并进黑板、**不置 `_is_dirty`**，外层 thread-modular fixpoint 在黑板「新 entry / 点集扩张」时**不重跑** → 恢复端 `on_load_pointer_restore` 读到的黑板值停在上一轮（D1->next n=2 而非 n=3）→ 遍历丢节点 → FN（09-regions_33）。修法：新 entry 和「set.size() 增长」两处都置 `_is_dirty=true`。**通用教训**：任何跨线程/跨迭代共享、在 fixpoint 中被「累积」的副结构（黑板/缓存/summary），其「内容变化」必须映射回 fixpoint 的 dirty 信号，否则「内容收敛了、但外层不知道」→ 静默欠收敛（这类 FN 表象是「某副结构少了个元素」，真因在「外层少迭代了一轮」）。

21. **已建模的同步调用不能同时再走 checker 的「未知库调用写合成」**：`DataRaceChecker::check_extern_call_effects` 对每个指针实参合成一个 Write（「库可能通过它写」）。但 `pthread_create`/`pthread_once` 已被引擎精确建模（`exec_pthread_create`/`exec_pthread_once` 跳过 `exec_unknown_call`），其指针实参**不是**「库要写的数据」——`pthread_create` 的 `void*` 是「传给子线程」（读非写）、`pthread_once` 的 once-control 是同步机内部状态。不排除就 fabricate「main 写了它递给线程的共享对象」「两个线程各自写 once-control」的假竞争（06-symbeq_26/27/33、87-once FP）。修法 = 在 exclusion 名单里和锁原语一起 `return`。**原则**：checker 的写合成是「未建模库调用」的 fallback；任何被引擎精确建模的同步/线程调用都该排除，否则双计。

22. **pthread_once 回调的实例要按「once-control 对象」键控，不按函数名/调用点**：once 回调「每个不同 control 跑一次」，两个 once 调用用同一个 control → 回调只跑一次（HB 后都 join）；用两个不同 control → 回调跑两次（自竞态，87-once_07/10 必须保持 RACE）。曾按函数名键控 → 两个 control 塌成一个实例 → 把真竞态当 HB 抑制（FN 回归）。且 control 键要**静态解析**（剥 PointerShift/Bitcast 到 GlobalVariable），不能用运行时 points-to——首轮分析全局未物化时 points-to 是 ⊤，会把「同一个 control」拆成两个实例（87-once_02 FP 复现）。`register_once_thread(func, key, joinable)` 里 `joinable=false`（control 不透明，如 `optr` 全局指针可能变）时返回 0、绝不 join（sound）。

**Why:** 这些都是"顺着直觉写会错、但查代码也不一定立刻警觉"的隐性陷阱——有编译/构建层面（1）、日志/API 误用（2）、抽象域格语义（3、9）、双态状态承载（4）、测试写法（5、6、7）、数值不变式（8）、别名/绑定语义（10）、取证卫生（11）、全局变量陷阱（12）、未知调用双路径（13）、undef→BOTTOM 静默 FN（14）、pthread_t 取址局部语义（15、16）。本会话及之前已各踩数次。

**How to apply:** 改完代码先同步 install（1）再验证；写日志/AST（2）、加锁集方法（4）、写测试（5）时逐条对照。背景见 [[ikos-race-detection-work]]（现状/架构）、[[ikos-concurrency-infrastructure]]（文件路径）。