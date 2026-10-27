#!/usr/bin/env python3
"""Closed private JOINABLE attributes: dual-ABI effects, failures and refusal controls."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

from ikos_runtime import runtime_command, runtime_identity, verify_runtime

ROOT = Path(__file__).resolve().parents[1]
CONTRACT = "closed-private-default-pthread-attributes-v1"
# Expected distinct attribute effect sites, final SAFE, and a retained race pair.
POSITIVES = {
    0: (3, True, False), 1: (2, True, False), 2: (6, True, False),
    3: (3, False, True), 4: (3, True, False), 5: (2, True, False),
    6: (3, False, True), 7: (3, False, True), 8: (3, True, False),
    9: (3, False, True), 35: (3, False, False), 42: (3, False, True),
}
REFUSALS = tuple(range(10, 35)) + (38, 39, 40, 41)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ikos", type=Path, default=ROOT / "install/bin/ikos")
    parser.add_argument("--bits", choices=(32, 64), type=int, default=32)
    args = parser.parse_args()
    command = runtime_command(args.ikos.resolve())
    identity = runtime_identity(args.ikos.resolve())
    env = {k: v for k, v in os.environ.items()
           if not k.startswith("IKOS_") and k not in {"PYTHONPATH", "PYTHONHOME"}}
    fixture = (ROOT / "analyzer/test/regression/bmc-values/pthread-attributes.c").read_text()
    count = 0
    with tempfile.TemporaryDirectory(prefix="ikos-pthread-attributes-") as folder:
        work = Path(folder)

        def check(name, body, proof_count, safe=None, pairs=False, disabled=False):
            nonlocal count
            source = work / "control.c"
            source.write_text(body)
            database, report = work / "result.db", work / "result.json"
            for output in (database, report):
                output.unlink(missing_ok=True)
            run_env = dict(env)
            if disabled:
                run_env["IKOS_DISABLE_PTHREAD_ATTRIBUTES"] = "1"
            proc = subprocess.run(command + ["--analyses=race", "--concurrency=auto",
                "--partitioning=guards", "--demote-race-to-unknown", "--bmc-deadline-ms=0",
                "--status-filter=*",
                f"-m{args.bits}", "--format=json", "--report-file=" + str(report),
                "-o", str(database), str(source)], env=run_env, capture_output=True,
                text=True, timeout=30)
            assert proc.returncode == 0, (name, proc.stdout, proc.stderr)
            data = json.loads(report.read_text())
            result = data["race_result"]
            proofs = {row["statement_id"] for row in data["reports"]
                      if (row.get("info") or {}).get("pthread_attributes_contract") == CONTRACT}
            assert len(proofs) == proof_count, (name, proofs, data, proc.stdout)
            if safe is not None:
                assert (result["verdict"] == "SAFE") == safe, (name, result)
            if pairs:
                assert result["remaining_pairs"] > 0, (name, result)
            if name == "case-3":
                # Repeated Create effects qualify; indexed non-NULL-attr join
                # completion is a separate unsupported certificate.
                assert result["verdict"] == "UNKNOWN" and result["coverage"] == "Checked", (name, result)
            assert not result["search_performed"], (name, result)
            if name == "case-6":
                # The success path joins both workers; the only worker/main conflict
                # is the init failure branch. A pair is stronger than generic UNKNOWN.
                assert any("INIT_FAILURE_ACCESS" in line for line in body.splitlines())
            if name == "case-35":
                assert any(o["reason"] == "external call effects not covered"
                           for o in result["obligations"]), (name, result)
            count += 1
            print(f"pthread-attributes {name}/bits={args.bits}: effects={len(proofs)} "
                  f"verdict={result['verdict']} pairs={result['remaining_pairs']}", flush=True)
            return data

        for case, (proofs, safe, pairs) in POSITIVES.items():
            body = f"#define CASE {case}\n" + fixture
            check(f"case-{case}", body, proofs, safe, pairs)
        for case in REFUSALS:
            # CASE 10: undefined POSIX detached join is only a certificate refusal.
            # CASE 11: legal detached/no-join control independently retains a conflict.
            check(f"case-{case}", f"#define CASE {case}\n" + fixture, 0,
                  safe=False if case == 11 else None)
        check("disabled-same-source", fixture, 0, False, disabled=True)
        check("same-name-definition", fixture + "\nint pthread_attr_init(pthread_attr_t *p) { "
              "((volatile unsigned char *)p)[0] = 1; return 0; }\n", 0)
        # Existing NULL-only handle/completion/BMC proofs get no attribute permission.
        null_body = fixture.replace("&attr, worker,", "0, worker,")
        check("NULL-create-independent-effects", null_body, 3, True)
        # No barrier or join is inferred merely from ending a main-local lifetime.
        no_join = ("#define CASE 7\n" + fixture).replace(
            "  if (pthread_join(thread[0], 0)) return 0;", "  /* no join */")
        check("destroy-and-local-exit-no-completion", no_join, 3, False, True)
    verify_runtime(identity)
    print(f"PASS {count} private pthread attribute controls, bits={args.bits}", flush=True)


if __name__ == "__main__":
    main()
