#!/bin/bash
# 完整模拟考官（在 Docker 里跑完整 no-data-race 基准集）
#
# 做三件事：
#   1) 生成任务清单（复用 svcomp_race_test 的扫描：哪些 .yml 是 no-data-race + 期望判决）
#   2) 起容器，挂载 benchmark 目录，容器内并行跑 bin/svcomp_witness.py（tool-info 路径）
#   3) 统计 TP/FP/TN/FN + 分数，和期望判决对比
#
# 用法：bash svcomp/docker-full-sim.sh [benchmark根目录] [镜像名] [--limit N]
set -euo pipefail

BENCH="${1:-/home/ruan/sv-benchmarks-2026}"
IMAGE="${2:-ikos-conc:svcomp27}"
LIMIT=0
if [ "${3:-}" = "--limit" ]; then LIMIT="${4:-0}"; fi

DIR="$(cd "$(dirname "$0")" && pwd)"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# 1) 生成任务清单
python3 - "$BENCH" "$WORK/tasks.txt" "$LIMIT" <<'PY'
import sys
from pathlib import Path
sv = Path(sys.argv[1]).resolve()
out = Path(sys.argv[2])
limit = int(sys.argv[3])
sys.path.insert(0, "/home/ruan/ikos-conc")
from svcomp_race_test import CONCURRENCY_DIRS, parse_task

rows = []
for d in CONCURRENCY_DIRS:
    dd = sv / "c" / d
    if not dd.is_dir():
        continue
    for yml in sorted(dd.glob("*.yml")):
        t = parse_task(yml)
        if t:
            src, expected, dm = t
            rows.append(f"{src.relative_to(sv)}|{expected}|{dm}")
rows.sort()
if limit:
    rows = rows[:limit]
out.write_text("\n".join(rows) + "\n")
print(f"任务数: {len(rows)}", file=sys.stderr)
PY

# 2) 容器内并行跑
docker run --rm \
  -v "$BENCH":/bench:ro \
  -v "$WORK/tasks.txt":/tasks.txt:ro \
  -v "$DIR/docker-bench-runner.py":/runner.py:ro \
  "$IMAGE" python3 /runner.py > "$WORK/results.tsv"

# 3) 统计
python3 - "$WORK" <<'PY'
import sys
from pathlib import Path
w = Path(sys.argv[1])
expected = {}
for l in (w / "tasks.txt").read_text().splitlines():
    rel, e, dm = l.split("|")
    expected[rel] = e


def verdict_kind(line):
    if "definitely UNSAFE" in line:
        return "Race"
    if "potentially UNSAFE" in line or "UNKNOWN" in line:
        return "Unknown"
    if "SAFE" in line:
        return "Safe"
    return "Unknown"


tp = fp = tn = fn = unk = 0
bad = []
for l in (w / "results.tsv").read_text().splitlines():
    rel, verdict = l.split("\t", 1)
    v = verdict_kind(verdict)
    e = expected.get(rel)
    if e == "Race Found" and v == "Race":
        tp += 1
    elif e == "Race Found" and v == "Safe":
        fn += 1
        bad.append((rel, "FN"))
    elif e == "Safe" and v == "Safe":
        tn += 1
    elif e == "Safe" and v == "Race":
        fp += 1
        bad.append((rel, "FP"))
    else:
        unk += 1

print(f"TP={tp} FP={fp} TN={tn} FN={fn} unknown={unk}")
print(f"score = TP*1 + TN*2 = {tp + tn * 2}")
for rel, kind in bad:
    print(f"  !! {kind}: {rel}")
PY
