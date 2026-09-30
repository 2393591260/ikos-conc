# category-structure.yml MR — 声明 IKOS-ConC **opt-in** 只参加 C.no-data-race

仓库：`gitlab.com/sosy-lab/sv-comp/bench-defs`
文件：`benchmark-defs/category-structure.yml`

> 已对着 bench-defs 最新 `main`（2026-09-30 拉取）核实：
> 基准类别是 `C.Concurrency`（**没有** `C.ConcurrencySafety` 这个名字）。
> 逐子类参与由**顶层 `opt_in:` / `opt_out:` 两张表**控制，keyed by 工具名。
> `C.Concurrency.verifiers` 里的工具会**默认参加全部 4 个子类**（no-data-race /
> no-overflow / unreach-call / valid-memsafety），所以只想参加 no-data-race 时
> **不能进 verifiers 列表**，只能进 `opt_in`。

## 改动（唯一一处）：在顶层 `opt_in:` 表按字母序插入

参照同类的 no-data-race-only 工具（`cooperace` / `locksmith` / `racerf` /
`sv-sanitizers`），它们都是**只进 `opt_in`、不进 verifiers**：

```yaml
opt_in:
  ...
  hornix:
    - C.unreach-call.Loops
    - C.unreach-call.Recursive
    - C.unreach-call.XCSP
  ikos-conc:                                     # <-- 新增，字母序在 hornix 和 korn 之间
    - C.no-data-race.Concurrency
    - C.no-data-race.Huawei-Concurrency-Challenges
  korn:
    - C.unreach-call.Arrays
    ...
```

**不要**把 `ikos-conc` 加进 `C.Concurrency.verifiers`（那会默认参加 no-overflow /
unreach-call / valid-memsafety 三个子类）。

## MR 描述模板

```
Title: Add ikos-conc (opt-in) for C.no-data-race.Concurrency (SV-COMP 2027)

Add IKOS-ConC, a sound thread-modular data-race detector built on NASA IKOS,
to the top-level opt_in table for C.no-data-race.Concurrency and
C.no-data-race.Huawei-Concurrency-Challenges. It is not added to the
C.Concurrency.verifiers list, so it does not participate in the other
concurrency subcategories.

Tool info: https://github.com/2393591260/ikos-conc
BenchExec tool-info module: benchexec/tools/ikos-conc
```

## 备注

- 该 MR 只需在注册截止（2026-10-08）前提交。
- `opt_in` 表的字母序位置、以及 `C.no-data-race.Huawei-Concurrency-Challenges`
  这个名字，提交前对着 bench-defs 最新 `main` 再核一遍（本文件基于 2026-09-30 快照）。
