#!/usr/bin/env python3
"""
IKOS 并发分析能力边界 —— 自动验证脚本。

批量运行 test/L1、test/L2 下的用例，提取每个用例的三态判定结果
(Safe / Race / Unknown)，与用例注释里的预期结果比对，输出 CSV 与汇总。

用法:
    python3 run_verification.py                     # 默认 IKOS 命令为 ikos
    python3 run_verification.py --ikos /path/to/ikos
    python3 run_verification.py --timeout 120 --jobs 4

输出:
    verification_result.csv   —— 每用例一行
    stdout                    —— 汇总（通过率 + 失败清单 + 失败日志片段）
"""

import argparse
import concurrent.futures
import csv
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
LEVELS = ["L1", "L2"]

# 判定结果归一化
SAFE = "Safe"
RACE = "Race"
UNKNOWN = "Unknown"
ERR = "Err"  # 超时/编译失败/崩溃

EXPECTED_RE = re.compile(r"预期结果[:：]\s*(Safe|Race|Unknown)", re.IGNORECASE)


def expected_result(path):
    """从用例头部注释解析预期结果（Safe/Race/Unknown）。"""
    with open(path, encoding="utf-8") as f:
        for i, line in enumerate(f):
            if i > 15:
                break
            m = EXPECTED_RE.search(line)
            if m:
                return m.group(1).lower()
    return None


def classify(out):
    """从 IKOS 输出提取三态判定（与 report.py 的判决顺序一致）。"""
    if "The program is UNKNOWN" in out:
        return UNKNOWN
    if "definitely UNSAFE" in out or "potential data race" in out:
        return RACE
    if "The program is SAFE" in out:
        return SAFE
    return ERR


def run_one(args, case):
    level, name, path, _exp = case
    db = tempfile.mktemp(suffix=".db")
    cmd = [args.ikos, "--analyses=race", "--concurrency=auto",
           "-o", db, path]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              timeout=args.timeout)
        out = proc.stdout + proc.stderr
        actual = classify(out)
    except subprocess.TimeoutExpired:
        actual = ERR
        out = "[timeout]\n"
    finally:
        for p in (db, db + "-wal", db + "-shm"):
            if os.path.exists(p):
                try:
                    os.remove(p)
                except OSError:
                    pass
    return {"level": level, "name": name, "expected": case[3],
            "actual": actual, "log": out}


def log_snippet(out, want, n=6):
    """从 IKOS 输出中提取与判定相关的关键日志片段。"""
    keys = []
    if want == RACE:
        keys += ["data race", "UNSAFE"]
    elif want == UNKNOWN:
        keys += ["UNKNOWN", "verdict"]
    elif want == SAFE:
        keys += ["SAFE"]
    keys += ["error:", "fatal:", "abort", "signal"]
    lines = out.splitlines()
    hits = [ln for ln in lines
            if any(k in ln for k in keys) and not ln.startswith("[*] Compiling")]
    return hits[:n]


def main():
    ap = argparse.ArgumentParser(description="IKOS 并发边界自动验证")
    ap.add_argument("--ikos", default="ikos",
                    help="IKOS 命令（默认 ikos；会追加 --analyses=race --concurrency=auto）")
    ap.add_argument("--timeout", type=int, default=120, help="单用例超时秒数")
    ap.add_argument("--jobs", type=int, default=4, help="并行度")
    ap.add_argument("--out", default="verification_result.csv", help="CSV 输出路径")
    args = ap.parse_args()

    # 收集用例
    cases = []
    for level in LEVELS:
        d = os.path.join(HERE, "test", level)
        if not os.path.isdir(d):
            continue
        for f in sorted(os.listdir(d)):
            if not f.endswith(".c"):
                continue
            exp = expected_result(os.path.join(d, f))
            if exp is None:
                exp = "?"
            cases.append((level, f, os.path.join(d, f), exp))

    print(f"发现 {len(cases)} 个用例，IKOS 命令 = {args.ikos}，"
          f"并行度 = {args.jobs}，超时 = {args.timeout}s\n")

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futures = [ex.submit(run_one, args, c) for c in cases]
        for fut in concurrent.futures.as_completed(futures):
            results.append(fut.result())

    # 按层级 + 名称排序，稳定输出
    results.sort(key=lambda r: (r["level"], r["name"]))

    # 写 CSV
    fieldnames = ["用例名", "所属层级", "预期结果", "实际结果", "是否通过"]
    with open(args.out, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(fieldnames)
        for r in results:
            passed = (r["expected"].lower() == r["actual"].lower())
            w.writerow([r["name"], r["level"], r["expected"], r["actual"],
                        "PASS" if passed else "FAIL"])

    # 汇总
    n = len(results)
    passed = [r for r in results
              if r["expected"].lower() == r["actual"].lower()]
    failed = [r for r in results
              if r["expected"].lower() != r["actual"].lower()]

    print(f"{'用例名':32s} {'层级':4s} {'预期':7s} {'实际':7s} {'结果'}")
    for r in results:
        ok = r["expected"].lower() == r["actual"].lower()
        print(f"{r['name']:32s} {r['level']:4s} {r['expected']:7s} "
              f"{r['actual']:7s} {'PASS' if ok else 'FAIL'}")

    print(f"\n===== 汇总 =====")
    print(f"总用例: {n}  通过: {len(passed)}  失败: {len(failed)}  "
          f"通过率: {len(passed) / n * 100:.1f}%" if n else "无用例")
    print(f"CSV 已写出: {args.out}")

    if failed:
        print(f"\n===== 失败用例明细 =====")
        for r in failed:
            print(f"\n--- [{r['level']}] {r['name']} "
                  f"预期={r['expected']} 实际={r['actual']} ---")
            snips = log_snippet(r["log"], r["expected"])
            if snips:
                for s in snips:
                    print(f"    {s.strip()[:160]}")
            else:
                print("    (无匹配日志，可能超时/崩溃)")

    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
