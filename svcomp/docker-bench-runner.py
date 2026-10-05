#!/usr/bin/env python3
"""在容器内跑：读 /tasks.txt（relpath|expected|dm），并行跑 svcomp_witness.py。

这是「考官 BenchExec 批量评测」的容器侧等价物：对每个任务走
bin/svcomp_witness.py（即 tool-info 模块 cmdline() 拼出的那条命令），
输出 "relpath<TAB>判决行"。
"""
import subprocess
from concurrent.futures import ThreadPoolExecutor


def load_tasks():
    out = []
    for line in open("/tasks.txt"):
        line = line.strip()
        if line:
            rel, expected, dm = line.split("|")
            out.append((rel, expected, dm))
    return out


def run(t):
    rel, expected, dm = t
    src = "/bench/" + rel
    cmd = ["python3", "/opt/ikos-conc/bin/svcomp_witness.py",
           "--ikos", "/opt/ikos-conc/bin/ikos",
           "--data-model", dm, src]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=180,
                           cwd="/tmp")  # 避免 witness.yml 写入工作目录互相踩
        lines = [x for x in p.stdout.splitlines() if x.strip()]
        verdict = lines[-1] if lines else ""
    except subprocess.TimeoutExpired:
        verdict = ""
    return f"{rel}\t{verdict}"


def main():
    tasks = load_tasks()
    with ThreadPoolExecutor(max_workers=8) as ex:
        for r in ex.map(run, tasks):
            print(r, flush=True)


if __name__ == "__main__":
    main()
