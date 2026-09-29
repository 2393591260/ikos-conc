# category-structure.yml MR — 声明 IKOS-ConC 参加 C.no-data-race

仓库：`gitlab.com/sosy-lab/sv-comp/bench-defs`
文件：`benchmark-defs/category-structure.yml`

## 改动（一行）

在 `C.Concurrency:` 的 `verifiers:` 列表里加 `- ikos-conc`：

```yaml
  C.Concurrency:
    properties:
      - unreach-call
      - no-data-race
      - no-overflow
      - valid-memsafety
    categories:
      - C.no-data-race.Concurrency
      - C.no-overflow.Concurrency
      - C.unreach-call.Concurrency
      - C.valid-memsafety.Concurrency
    verifiers:
      ...
      - infer
      - ikos-conc        # <-- 新增
      - lazycseq
      ...
```

`C.Concurrency` 下的 `verifiers` 同时覆盖 `C.no-data-race.Concurrency`（我们主目标）以及 no-overflow / unreach-call / valid-memsafety 三个并发子类。

## 若只想要 no-data-race（opt-out 其他子类）

如果只想参加 `C.no-data-race`、不参加其他并发子类，需要在 MR 里同时把
`ikos-conc` 从其他子类的 verifiers 里 opt-out。SV-COMP 2027 用 `benchmark-defs`
里的 per-category `verifiers` 列表 + `opt_out` 机制控制（确切字段见 bench-defs
的 `category-structure.yml` 顶部注释 / 组织者说明）。默认按「全并发子类都参加」
提交即可，等组织者确认再按需 opt-out。

## MR 描述模板

```
Title: Add ikos-conc as a verifier for C.no-data-race (SV-COMP 2027)

Add IKOS-ConC, a sound thread-modular data-race detector built on NASA IKOS,
to the C.Concurrency verifiers list (target category: C.no-data-race.Concurrency).

Tool info: https://github.com/2393591260/ikos-conc
BenchExec tool-info module: benchexec/tools/ikos-conc
```

## 备注

- 该 MR 只需在注册截止（2026-10-08）前提交；工具本体归档 + FM-Tools 条目
  在工具提交截止（2026-10-20）前完成即可。
- 精确的 opt-out/opt-in 语法以 bench-defs 仓库当前 `category-structure.yml`
  为准，提交前对着最新 main 再核一遍。
