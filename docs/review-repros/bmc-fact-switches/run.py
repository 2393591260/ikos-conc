#!/usr/bin/env python3
"""Exercise independent BMC fact switches and check every selected AR execution."""

import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[3]
REPROS = ROOT / "docs/review-repros"
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "scripts"))
import svcomp_race_test as suite
from sweep_bmc_execution import validate_report

FLAGS = {
    "bounds": "IKOS_BMC_DISABLE_AI_BOUNDS",
    "no_wrap": "IKOS_BMC_DISABLE_AI_NO_WRAP",
    "single_execution": "IKOS_BMC_DISABLE_AI_SINGLE_EXECUTION",
    "master": "IKOS_BMC_DISABLE_AI_FACTS",
}
PROFILES = {
    "default": {},
    "bounds-off": {"bounds": "1"},
    "no-wrap-off": {"no_wrap": "1"},
    "numerical-off": {"bounds": "1", "no_wrap": "1"},
    "lifetime-off": {"single_execution": "1"},
    "legacy-off": {"master": "1"},
    # All four switches retain the legacy presence convention: "0" disables.
    "bounds-zero": {"bounds": "0"},
    "no-wrap-zero": {"no_wrap": "0"},
    "lifetime-zero": {"single_execution": "0"},
    "legacy-zero": {"master": "0"},
}
CONTROL_PROFILES = ("default", "numerical-off", "lifetime-off", "legacy-off")
CASES = (
    ("local-no-wrap-race", "ai-facts/unused-local.c", None, "Race Found"),
    ("helper-publication-race", "bmc-pointer-publication/publication.c", 0, "Race Found"),
    ("exclusive-guard", "bmc-pointer-publication/publication.c", 13, "Safe"),
    ("repeated-helper", "bmc-pointer-publication/publication.c", 2, "Race Found"),
    ("unsigned-wrap-race", "bmc-fresh-solver/unsigned-overflow-race.c", None, "Race Found"),
    ("unsigned-wrap-control", "bmc-fresh-solver/unsigned-overflow-control.c", None, "Safe"),
    ("signed-overflow-prefix", "sc-same-path/unused-overflow-control.c", None, None),
)


def enabled(settings, fact):
    return "master" not in settings and fact not in settings


def run(args, case, bits, profile):
    name, relative, define, expected = case
    source = REPROS / relative
    folder = args.output / f"{bits}-{name}-{profile}"
    folder.mkdir()
    settings = PROFILES[profile]
    env = dict(os.environ, IKOS_BMC_RECOVER="1", IKOS_BMC_DIAGNOSTICS="1")
    for flag in FLAGS.values():
        env.pop(flag, None)
    env.update({FLAGS[key]: value for key, value in settings.items()})
    command = [str(args.ikos.resolve()), "--analyses=race", "--concurrency=auto",
               "--demote-race-to-unknown", "-m", str(bits), "--format=json",
               "--report-file=" + str(folder / "report.json"),
               "-o", str(folder / "output.db")]
    if define is not None:
        command.append(f"-DCASE={define}")
    command.append(str(source))
    started = time.monotonic()
    proc = subprocess.Popen(command, cwd=folder, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, start_new_session=True)
    timed_out = False
    try:
        output, _ = proc.communicate(timeout=args.timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        output, _ = proc.communicate()
    text = output.decode(errors="replace")
    (folder / "run.log").write_text(text)
    verdict = suite.classify(text, proc.returncode, timed_out)
    row = dict(case=name, input=str(source), source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
               define=define, expected=expected, bits=bits, profile=profile,
               environment={FLAGS[key]: value for key, value in settings.items()},
               command=command, verdict=verdict, returncode=proc.returncode,
               timed_out=timed_out, seconds=time.monotonic() - started)
    report_file = folder / "report.json"
    report = json.loads(report_file.read_text()) if report_file.exists() else {}
    report["reports"] = [r for r in report.get("reports", []) if isinstance(r.get("info"), dict)]
    metrics, diagnostics = Counter(), Counter()
    candidate_pairs = 0
    for group in report["reports"]:
        info = group["info"]
        metrics.update(info.get("bmc_result", {}).get("metrics", {}))
        diagnostics.update(info.get("bmc", {}).get("counts", {}))
        candidate_pairs += len(info.get("conflicting_accesses", []))
    row.update(metrics=dict(metrics), diagnostics=dict(diagnostics),
               candidate_pairs=candidate_pairs, validations=validate_report(report))
    (folder / "result.json").write_text(json.dumps(row, indent=2) + "\n")
    print(bits, name, profile, verdict, dict(metrics), flush=True)
    assert proc.returncode == 0 and not timed_out, row
    assert verdict in (suite.VERDICT_SAFE, suite.VERDICT_RACE, suite.VERDICT_UNKNOWN), row
    if verdict == suite.VERDICT_RACE:
        assert row["validations"] and all(v["status"] == "ACCEPT" for v in row["validations"]), row
    if expected == "Safe":
        assert verdict != suite.VERDICT_RACE, row
    elif expected == "Race Found":
        assert verdict != suite.VERDICT_SAFE, row
    for fact in ("bounds", "no_wrap", "single_execution"):
        if not enabled(settings, fact):
            assert metrics[f"ai_{fact}"] == 0, row
    assert diagnostics["ai_single_execution"] == metrics["ai_single_execution"], row
    if name == "local-no-wrap-race":
        assert "ai_single_execution" in row["metrics"], "requires new metrics schema"
        assert verdict == suite.VERDICT_RACE, row
        assert metrics["ai_no_wrap"] == int(enabled(settings, "no_wrap")), row
        assert (metrics["ai_bounds"] > 0) == enabled(settings, "bounds"), row
        assert metrics["path_queries"] == 1 and metrics["solver_queries"] >= 1, row
    elif name == "helper-publication-race":
        lifetime = enabled(settings, "single_execution")
        assert verdict == (suite.VERDICT_RACE if lifetime else suite.VERDICT_UNKNOWN), row
        assert (metrics["ai_single_execution"] > 0) == lifetime, row
    elif name == "repeated-helper":
        assert verdict == suite.VERDICT_UNKNOWN, row
    elif name == "unsigned-wrap-race":
        assert verdict == suite.VERDICT_RACE, row
    elif name == "signed-overflow-prefix":
        # An undefined prefix cannot certify the later target race.
        assert verdict == suite.VERDICT_UNKNOWN and metrics["ai_no_wrap"] == 0, row
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--ikos", type=Path, default=ROOT / "install/bin/ikos")
    parser.add_argument("--analyzer", type=Path, default=ROOT / "install/bin/ikos-analyzer")
    parser.add_argument("--timeout", type=int, default=30)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    rows = []
    for bits in (32, 64):
        for case in CASES:
            profiles = PROFILES if case[0] in ("local-no-wrap-race", "helper-publication-race") else CONTROL_PROFILES
            samples = [run(args, case, bits, profile) for profile in profiles]
            rows.extend(samples)
            # The switches only change BMC fact provision, never AI candidates.
            assert len({r["candidate_pairs"] for r in samples}) == 1, samples
            first = samples[0]
            for row in samples:
                if row["profile"] in ("bounds-off", "no-wrap-off", "numerical-off", "bounds-zero", "no-wrap-zero"):
                    assert row["verdict"] == first["verdict"], row
            if len(samples) == len(PROFILES):
                by_profile = {r["profile"]: r for r in samples}
                for one, zero in (("bounds-off", "bounds-zero"), ("no-wrap-off", "no-wrap-zero"),
                                  ("lifetime-off", "lifetime-zero"), ("legacy-off", "legacy-zero")):
                    a, b = by_profile[one], by_profile[zero]
                    assert a["verdict"] == b["verdict"], (a, b)
                    for key in ("ai_bounds", "ai_no_wrap", "ai_single_execution", "path_queries", "solver_queries"):
                        assert a["metrics"].get(key, 0) == b["metrics"].get(key, 0), (a, b)
    (args.output / "results.json").write_text(json.dumps(rows, indent=2) + "\n")
    summary = dict(runs=len(rows), data_models=[32, 64], profiles=PROFILES,
                   analyzer_sha256=hashlib.sha256(args.analyzer.read_bytes()).hexdigest(),
                   verdicts=dict(Counter(r["verdict"] for r in rows)),
                   selected_executions=sum(len(r["validations"]) for r in rows),
                   validation="All reported FALSE selected executions independently AR ACCEPT; not source witness validation",
                   metric_scope="ai_single_execution counts successful per-event certificate annotations, not unique functions or required publication uses",
                   timing_scope="Regression observations only; no speedup claim")
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
