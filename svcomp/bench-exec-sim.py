#!/usr/bin/env python3
"""模拟 BenchExec 批量评测：用 tool-info 模块的 standalone 入口跑整个基准集。

svcomp_race_test.py 是直接跑 ikos；本脚本走 benchexec/tools/ikos-conc.py 的
__main__（等价于 BenchExec 调 Tool.cmdline() + determine_result() 的路径：
svcomp_witness.py wrapper + --format=json + witness 生成），更贴近 SV-COMP 里
BenchExec 实际怎么调用我们。目的：确认 tool-info 集成路径和直接跑 ikos 得到
的判决一致（TP/FP/TN/FN 对得上）。
"""
import argparse
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import svcomp_race_test as base  # 复用任务扫描

TOOL_INFO = (Path(__file__).resolve().parent.parent /
             "benchexec" / "tools" / "ikos-conc.py")
IKOS = (Path(__file__).resolve().parent.parent /
        "install" / "bin" / "ikos")

# tool-info standalone 输出的三种（见 ikos-conc.py __main__ 的 print）
VERDICT_MAP = {
    "TRUE": base.VERDICT_SAFE,
    "FALSE(no-data-race)": base.VERDICT_RACE,
    "UNKNOWN": base.VERDICT_UNKNOWN,
}


def run_one(src, data_model):
    cmd = [sys.executable, str(TOOL_INFO), str(src),
           "--data-model", data_model, "--ikos", str(IKOS), "--timeout", "120"]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=150)
        lines = [l for l in proc.stdout.strip().splitlines() if l.strip()]
        last = lines[-1] if lines else ""
        return VERDICT_MAP.get(last, base.VERDICT_UNKNOWN)
    except subprocess.TimeoutExpired:
        return base.VERDICT_UNKNOWN


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("svbench", type=Path, nargs="?",
                    default=Path("/home/ruan/sv-benchmarks-2026"))
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args(argv)

    tasks = []
    for d in base.CONCURRENCY_DIRS:
        dd = args.svbench / "c" / d
        if not dd.is_dir():
            continue
        for yml in sorted(dd.glob("*.yml")):
            t = base.parse_task(yml)
            if t:
                tasks.append((yml, *t))
    tasks.sort(key=lambda t: str(t[0]))
    if args.limit:
        tasks = tasks[: args.limit]
    print(f"Found {len(tasks)} no-data-race tasks", file=sys.stderr)

    def work(t):
        yml, src, expected, dm = t
        return (str(src), expected, dm, run_one(src, dm))

    rows = []
    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = [ex.submit(work, t) for t in tasks]
        for fut in as_completed(futs):
            rows.append(fut.result())

    tp = fp = tn = fn = unk = 0
    for path, expected, dm, v in rows:
        if v == base.VERDICT_RACE and expected == base.VERDICT_RACE:
            tp += 1
        elif v == base.VERDICT_RACE and expected == base.VERDICT_SAFE:
            fp += 1
        elif v == base.VERDICT_SAFE and expected == base.VERDICT_SAFE:
            tn += 1
        elif v == base.VERDICT_SAFE and expected == base.VERDICT_RACE:
            fn += 1
        else:
            unk += 1

    print(f"TP={tp} FP={fp} TN={tn} FN={fn} unknown={unk}")
    print(f"score = TP*1 + TN*2 = {tp + tn * 2}")


if __name__ == "__main__":
    main()
