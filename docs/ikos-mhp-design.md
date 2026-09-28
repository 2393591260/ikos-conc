# IKOS MHP 对标 goblint 设计文档

> 目的：梳理 goblint 的 MHP（may-happen-in-parallel）抽象，对照 IKOS 现状，给出一个结合 IKOS 实际的、
> 可落地的 MHP 设计方案，作为后续「实例级 create/join HB + 祖先关系」专项的工作依据。
>
> 参考源码：
> - goblint `src/cdomains/mHP.ml`（`may_happen_in_parallel` 本体）
> - goblint `src/analyses/mHPAnalysis.ml`（每访问点快照 `{tid, created, must_joined}`）
> - goblint `src/cdomain/value/cdomains/threadIdDomain.ml`（结构化 TID 树路径 + `must/may_be_ancestor`）
> - IKOS `analyzer/src/checker/data_race.cpp`（成对判定 + `build_thread_creators`）
> - IKOS `core/.../lockset/lockset_domain.hpp`（spawned/joined digest）
> - IKOS `core/.../concurrent_global_env.hpp`（site_id 注册表 / `_func_instances` / `_join_summary`）

---

## 1. 结论先行

- **goblint 的 MHP 是「一等抽象域 + 独立谓词」**：每个程序点携带 `{tid, created, must_joined}`，race 判定时作为
  `may_race` 的**一个合取项**（与锁集/region/symb_locks/mallocFresh 并列）。
- **IKOS 没有独立 MHP**：create/join 的 HB 被烤进 `data_race.cpp` 的成对循环，用
  `LocksetDomain` 的 `spawned_threads/spawned_instances/joined_threads` + `ConcurrentGlobalEnv` 静态扫出的
  `_thread_creators/_thread_ancestors`。
- **本质差距 = 线程身份的粒度**：goblint 的 TID 是**树路径结构**（`(前缀列表, 非唯一集合)`），同一函数在
  「第 1 个 create 点」和「第 2 个 create 点」产生**两个不同的 TID**，所以 create/join HB 天然按实例算；
  IKOS 的线程身份是**函数名字符串**，把所有实例塌成 `"t_fun"`，于是 HB 被迫按名字算——13/15/53 的 FP 都源于此。
- 本方案把 IKOS 的线程身份升级到 **site_id 实例级**，并抽出一个对标 `may_happen_in_parallel` 的独立判定，
  在不改动「每函数只分析一次」的 thread-modular 骨架前提下，追平 goblint 的实例级精度。

---

## 2. 三态对比总览

| 维度 | goblint | IKOS 现状（截至 2026-09-26） | IKOS 设计目标 |
|---|---|---|---|
| MHP 形态 | 独立域 + 独立谓词 | 烤进 checker 成对循环 | 抽成独立函数 `may_happen_in_parallel(a,b)` |
| 线程身份 | `tid` = 树路径 `(prefix, set)`，`must/may_be_ancestor` 直接可查 | `thread_id` = 函数名字符串 + 旁路 `site_id` 注册表 | `instance_sid`（create site 的 sid）为主，名字仅作兜底 |
| created（spawned） | `created`：MAY 集合，元素是结构化 TID | `spawned_threads`（MAY 名字）+ `spawned_instances`（MAY sid，本轮新加） | 只保留 sid 版 `spawned_instances` |
| must_joined | `must_joined`：MUST 集合，结构化 TID | `joined_threads`：MUST sid 集合 | 不变（已是 sid） |
| 祖先关系 | TID 前缀判断（树结构内置） | `_thread_creators` 名字 → 名字，`_thread_ancestors` 传递闭包 | `_site_ancestors`：sid → 祖先 sid 集合 |
| 唯一性 | `is_unique` = set 空（TID 结构内置） | `is_thread_unique(name)` = spawn count ≤ 1（+ 本轮顺序 spawn 检测） | `is_site_unique(sid)`：create site 是否在循环 + 是否顺序 spawn |
| 分析次数 | 上下文敏感，每 create site 一份 | 每函数一份（thread-modular） | **保持每函数一份**（见 §4 关键取舍） |

---

## 3. goblint 组件逐条 → IKOS 落地

### 3.1 线程身份 TID → `instance_sid`

goblint `History` 模块把 TID 建成 `(prefix, set)`：
- `prefix`：从 main 到当前线程的创建链（逆序存，main 在末尾），每个节点 = `(函数 varinfo, create 节点, 唯一计数)`；
- `set`：非唯一线程的「可能身份集合」（`is_unique = set 为空`）；
- `must_be_ancestor(a,b)`：`a` 唯一 且 `a.prefix` 是 `b.prefix` 的前缀。

**IKOS 落地**：site_id 注册表（`_site_ids: CallBase* → sid`）已经是「create 节点」的类比物。设计：

- AccessRecord 增加 `std::uint64_t instance_sid`（默认 `kMainThreadSid`）。
- main 的访问 → `kMainThreadSid`（已存在，`concurrent_global_env.hpp`）。
- spawned 线程的访问 → 其 **create site 的 sid**。thread-modular 分析 t_fun 时，把「当前实例 sid」作为入口上下文注入，
  这样 t_fun 体内每个 access 都能带上自己属于哪个实例。
- 名字 `thread_id` 保留，仅用于诊断输出与「同函数回退」路径。

### 3.2 `created` → `spawned_instances`（本轮已做）

goblint `created` = MAY 集合，元素是 TID。IKOS 本轮已把 `spawned_instances`（MAY sid 集合）加进 LocksetDomain 并
在 `exec_pthread_create` 里 `add_spawned_instance(sid)`。**设计**：名字版 `spawned_threads` 逐步退役，`creates_before`
改走 sid。

### 3.3 `must_joined` → `joined_threads`（已是 sid，不变）

goblint `must_joined` = MUST TID 集合。IKOS `joined_threads` = MUST sid 集合，`joined_names` 是「sid → 名字（全实例
join 才成立）」的归约。设计保留 sid 语义，删掉「全实例 join」这个过强的归约（见 3.4）。

### 3.4 `may_happen_in_parallel` 四个判定 → IKOS 函数

goblint 原逻辑（`mHP.ml`）：

```ocaml
let may_happen_in_parallel one two =
  match tid, tid2 with
  | `Lifted tid, `Lifted tid2 ->
    if is_unique tid && equal tid tid2 then false                       (* ① 同一唯一实例 *)
    else if definitely_not_started (tid,created) tid2
         || definitely_not_started (tid2,created2) tid then false       (* ② create 边 *)
    else if must_be_joined tid2 must_joined
         || must_be_joined tid must_joined2 then false                  (* ③ join *)
    else if exists_definitely_not_started_in_joined ... then false      (* ④ 传递 create+join *)
    else true
  | _ -> true
```

其中 `definitely_not_started(current, created, other)` = 「current 是 other 的 must 祖先，且 current 还没 create 出
『other 或 other 的 may 祖先』」。

**IKOS 落地**（`DataRaceChecker` 内抽成一个成员函数，四个条件一一对应）：

| goblint 条件 | IKOS 判定 | 依赖的数据 |
|---|---|---|
| ① 同一唯一实例 | `a.instance_sid == b.instance_sid && is_site_unique(a.instance_sid)` | `instance_sid` + `is_site_unique` |
| ② create 边 | `must_be_ancestor(a.sid, b.sid) && !a.spawned_instances.count(b.sid) && !a.spawned_any_ancestor_of(b.sid)` | `_site_ancestors` + `spawned_instances` |
| ③ join | `b.sid ∈ a.joined_threads`（或反过来） | `joined_threads`（sid，已有） |
| ④ 传递 | `∃ t ∈ a.joined_threads : must_be_ancestor(t, b.sid) && t 没 spawn b` | `_join_summary` + `_site_ancestors` |

**关键语义（务必写清）**：IKOS 每函数只分析一次，所以一条 AccessRecord 代表「该函数体在**任意一个实例**里的执行」，
而非 goblint 那种「某个具体实例」。因此 IKOS 的 `may_happen_in_parallel(a, b)` 的②③④必须写成**全称**：

> 对 b 所在函数的**每一个实例 sid**：要么 a 已 join 该实例，要么 a 还没 spawn 该实例（且 a 是该实例的祖先）。

这正是 13 号修复里 `joined_names`「joined OR not-yet-spawned」的推广；把它推广到 `must_be_ancestor` + `spawned_instances`
就得到完整的实例级判定。

---

## 4. 关键取舍：每函数一份 vs 上下文敏感

goblint 上下文敏感（每 create site 一份分析），所以它的 `may_happen_in_parallel` 是「两个具体实例」比较。
IKOS thread-modular 每函数只分析一次（这是性能/收敛的核心设计，**不打算改**）。后果是：

- IKOS 的 `may_happen_in_parallel(a,b)` 里，`b` 的 `instance_sid` 是「**可能实例集合的代表**」，判定要退化为
  「对每个可能实例都成立才 suppress」。
- 这牺牲了 goblint「按具体实例单点判定」的精确性，但**保持 sound**（FN=0 红线），且把当前「名字级」的粗粒度
  提升到「实例级」。

要真正拿到 goblint 逐实例的精度（例如 53 号 `b(NULL)` vs `b(&g)` 这种「同函数不同实参导致不同行为」），需要
进一步做「按 create site 上下文敏感地分析线程函数」——那是第二阶段，不在本方案范围内。

---

## 5. 数据结构改动清单（具体到文件）

### `core/.../concurrent_global_env.hpp`
| 字段/方法 | 说明 |
|---|---|
| `kMainThreadSid`（已有） | main 的保留实例 id |
| `_site_ids`（已有） | create site → sid |
| `_func_instances`（已有） | 函数名 → sid 集合 |
| **新增 `_site_ancestors: unordered_map<uint64_t, set<uint64_t>>`** | sid → 祖先 sid 集合（静态，注册时构建） |
| **新增 `is_site_unique(uint64_t sid)`** | 该 create site 是否唯一实例（循环/顺序 spawn 检测） |
| `spawns_are_sequential_locked`（已有） | 顺序 spawn 检测，供 `is_site_unique` 复用 |

`_site_ancestors` 构建（在 `register_thread_func` 里，沿 create 边做闭包）：
- main 的 create site `s` → `_site_ancestors[s] = {kMainThreadSid}`；
- 函数 F（其可能实例集合 `I_F = _func_instances[F]`）内的 create site `s` → `_site_ancestors[s] = I_F ∪ (∀ c ∈ I_F: _site_ancestors[c])`。

### `core/.../lockset/lockset_domain.hpp`
| 字段 | 说明 |
|---|---|
| `_spawned_instances`（已有） | MAY sid 集合 = goblint `created` 的 sid 版 |
| `_joined_threads`（已有） | MUST sid 集合 = goblint `must_joined` |
| `_spawned_threads`（名字版） | 保留兼容，逐步退役 |

### `analyzer/include/.../checker/data_race.hpp` + `src/checker/data_race.cpp`
| 改动 | 说明 |
|---|---|
| AccessRecord 加 `uint64_t instance_sid` | 访问所属实例 |
| 抽 `bool may_happen_in_parallel(const AccessRecord&, const AccessRecord&)` | 四个判定（§3.4） |
| `build_thread_creators` 改为（或增补）sid 版 | 产出 `_site_ancestors`，替代名字版 `_thread_creators/_thread_ancestors` |
| `joined_names` 归约删除，改用 sid 级判定 | 「全实例 join」的过强归约不再需要 |

---

## 6. 实现步骤（分阶段，每步独立可验证）

1. **AccessRecord 加 `instance_sid`**：thread-modular 分析入口注入当前 create site 的 sid（main → kMainThreadSid）。
   → 纯数据流，不改判定，先确保字段填充正确。
2. **`_site_ancestors` 构建**：在 `register_thread_func` 里按 sid 构建祖先闭包。
   → 用 probe 验证 main→t_fun→t2_fun 的祖先集合。
3. **`is_site_unique`**：把 `is_thread_unique(name)` 的循环/顺序检测下沉到 sid 级。
4. **抽 `may_happen_in_parallel(a,b)`**：把成对循环里的 ①同实例唯一 / ②create 边 / ③join / ④传递 四条改成调用新函数，
   先保持行为不变（A/B 应 FN=0、FP 不变）。
5. **切 sid 语义**：`creates_before` 改用 `_site_ancestors` + `spawned_instances`，删名字版闭包。
   → 目标：53-races-mhp/04 及未来 MHP 类 FP 下降，FN 保持 0。

---

## 7. 预期收益（对应残留 FP）

| 文件 | 现状 | MHP 设计后 |
|---|---|---|
| `10-synch/53-races-mhp/04-not-created2.c` | FP（`b(NULL)` 与 `b(&g)` 实例折叠，实参不同） | 部分缓解（实例级 created/join）；彻底要「按实参上下文敏感」，属第二阶段 |
| `10-synch/16-join_loop_nr.c` | FP（`id[i]` 句柄 ⊤） | 不直接受益（这是数组句柄内存精度，非 MHP）；另开专项 |
| 未来「同函数多实例交错 create/join」类 | 可能 FP | 直接受益（实例级祖先 + 实例级 created/joined） |

> 说明：MHP 主要治「线程实例分不开」这一类；16 是内存模型精度（变下标数组句柄），不在本设计范围内。

---

## 8. soundness 红线

- 每函数只分析一次 ⇒ `may_happen_in_parallel` 的②③④都是**全称判定**（对所有可能实例成立才 suppress）。
- `spawned_instances` 是 MAY（join=并集），`spawned_instances.count(sid)==0` 才是「肯定没 spawn 该实例」的 MUST 事实；
  用反了（把 MAY 当 MUST）会漏报——沿用本轮 13 号「not-yet-spawned」的门控（`is_creator && !spawned_top`）。
- `_site_ancestors` 是静态扫 `pthread_create` 来的，**间接/条件 create** 会缺席；缺席时祖先判定返回 false（不 suppress），
  保持 sound（宁可报多不可漏报）。
