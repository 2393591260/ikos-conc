#!/usr/bin/env python3
"""A startup bound hint changes search, never guard values or thread effects."""

import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "scripts"))
import svcomp_race_test as suite
from sweep_bmc_execution import validate_report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ikos", type=Path, default=ROOT / "install/bin/ikos")
    parser.add_argument("--analyzer", type=Path, default=ROOT / "install/bin/ikos-analyzer")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--phase", choices=("baseline", "candidate"), default="candidate")
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    source = Path(__file__).with_name("startup.c").resolve()
    analyzer_hash = hashlib.sha256(args.analyzer.read_bytes()).hexdigest()
    rows = []
    for bits in (32, 64):
        for case, variant in ((0, 0), (1, 0), (2, 0), (3, 0), (0, 1)):
            for mode in ("ai", "bmc"):
                folder = args.output / f"{bits}-{case}-{variant}-{mode}"
                folder.mkdir()
                env = {key: value for key, value in os.environ.items()
                       if not key.startswith(("IKOS_BMC_", "IKOS_AI_"))}
                if mode == "bmc":
                    env.update(IKOS_BMC_RECOVER="1", IKOS_BMC_DIAGNOSTICS="1")
                report_file = folder / "report.json"
                command = [str(args.ikos.resolve()), "--analyses=race", "--concurrency=auto",
                           "--demote-race-to-unknown", "-m", str(bits), f"-DCASE={case}",
                           f"-DVARIANT={variant}", "--format=json", "--report-file=" + str(report_file),
                           "-o", str(folder / "output.db"), str(source)]
                process = subprocess.Popen(command, cwd=folder, env=env, stdout=subprocess.PIPE,
                                           stderr=subprocess.STDOUT, start_new_session=True)
                timed_out = False
                try:
                    output, _ = process.communicate(timeout=30)
                except subprocess.TimeoutExpired:
                    timed_out = True
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    output, _ = process.communicate()
                text = output.decode(errors="replace")
                (folder / "run.log").write_text(text)
                verdict = suite.classify(text, process.returncode, timed_out)
                report = json.loads(report_file.read_text()) if report_file.exists() else {}
                report["reports"] = [r for r in report.get("reports", []) if isinstance(r.get("info"), dict)]
                counts = Counter()
                for group in report["reports"]:
                    counts.update(group["info"].get("bmc", {}).get("counts", {}))
                validations = validate_report(report)
                row = dict(case=case, variant=variant, bits=bits, mode=mode, verdict=verdict,
                           returncode=process.returncode, timed_out=timed_out, counts=dict(counts),
                           validations=validations, command=command,
                           source_expected="Safe" if case == 2 else "Race Found",
                           boundary="Needs the third flag-writing thread to execute before main" if case == 3 else None)
                (folder / "result.json").write_text(json.dumps(row, indent=2) + "\n")
                rows.append(row)
                print(folder.name, verdict, dict(counts), flush=True)
                assert process.returncode == 0 and not timed_out, row
                assert verdict in (suite.VERDICT_SAFE, suite.VERDICT_RACE, suite.VERDICT_UNKNOWN), row
                if case == 2:
                    assert verdict != suite.VERDICT_RACE, row
                else:
                    assert verdict != suite.VERDICT_SAFE, row
                if verdict == suite.VERDICT_RACE:
                    assert validations and all(v["status"] == "ACCEPT" for v in validations), row
                if mode == "bmc":
                    if case == 3:
                        assert verdict == suite.VERDICT_UNKNOWN, row
                    if case == 1:
                        assert verdict == suite.VERDICT_RACE, row
                        assert counts["startup_literal_loop_hint"] == 0, row
                    if case == 0 and args.phase == "candidate":
                        assert verdict == suite.VERDICT_RACE, row
                        assert counts["startup_literal_loop_hint"] > 0, row
    assert hashlib.sha256(args.analyzer.read_bytes()).hexdigest() == analyzer_hash, "Analyzer changed during checks"
    (args.output / "results.json").write_text(json.dumps(rows, indent=2) + "\n")
    summary = dict(runs=len(rows), phase=args.phase, analyzer_sha256=analyzer_hash,
                   source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                   verdicts=dict(Counter(r["verdict"] for r in rows)),
                   selected_executions=sum(len(r["validations"]) for r in rows),
                   scope="Bound-guided startup search, not loop termination or a new source of values; all FALSE selected AR executions ACCEPT")
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
