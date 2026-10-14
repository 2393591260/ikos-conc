#!/usr/bin/env python3
"""Small fake-tool tests: one-run evidence, coverage, and process-tree cleanup."""
import contextlib
import gzip
import io
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import types
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import svcomp_race_test as suite
import sweep_bmc_execution


FAKE = '''import json, pathlib, subprocess, sys, time
source = pathlib.Path(sys.argv[-1])
config = json.loads(source.read_text())
with source.with_suffix('.calls').open('a') as stream:
    stream.write(json.dumps(sys.argv[1:]) + '\\n')
database = pathlib.Path(sys.argv[sys.argv.index('-o') + 1])
if config.get('database', True):
    database.write_text(json.dumps(config.get('report')))
if config.get('database_sidecar'):
    database.with_name(database.name + '-journal').write_text('interrupted transaction')
if config.get('child'):
    child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])
    source.with_suffix('.child').write_text(str(child.pid))
    time.sleep(60)
time.sleep(config.get('sleep', 0))
for arg in sys.argv:
    if arg.startswith('--report-file=') and config.get('report') is not None:
        pathlib.Path(arg.split('=', 1)[1]).write_text(json.dumps(config['report']))
source.with_suffix('.done').touch()
print(config.get('output', 'The program is definitely UNSAFE'), flush=True)
raise SystemExit(config.get('exit', 0))
'''


class RunnerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="runner-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.fake = self.root / "fake.py"
        self.fake.write_text(FAKE)
        self.bench = self.root / "bench"
        (self.bench / "c/pthread").mkdir(parents=True)
        self.runtime = types.SimpleNamespace(
            runtime_identity=mock.Mock(return_value={"command": [sys.executable, str(self.fake)]}),
            verify_runtime=mock.Mock())
        self.modules = mock.patch.dict(sys.modules, ikos_runtime=self.runtime)
        self.modules.start()
        self.addCleanup(self.modules.stop)

    def task(self, name="one", expected="false", **config):
        source = self.bench / "c/pthread" / (name + ".c")
        source.write_text(json.dumps(config))
        source.with_suffix(".yml").write_text(
            f"input_files: '{name}.c'\nproperties:\n"
            f"  - property_file: ../properties/no-data-race.prp\n"
            f"    expected_verdict: {expected}\noptions:\n  data_model: ILP32\n")
        return source

    def run_evidence(self, *args):
        destination = self.root / "evidence"
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            code = suite.main([str(self.bench), "--ikos", str(self.fake), "--jobs", "2",
                               "--evidence-dir", str(destination), *args])
        return code, json.loads((destination / "summary.json").read_text()), json.loads(
            (destination / "results.json").read_text())

    def selected_report(self):
        return {"reports": [{"info": {}}, {"statements": [23], "info": {
            "confirmed_pair_id": 9, "access_a": {"file": "input.c", "line": 7},
            "access_b": {"file": "input.c", "line": 12},
            "thread_creations": [{"line": 5}],
            "counterexample": {"program": {"marker": "same-run"}}}}]}

    def test_one_invocation_and_offline_validation(self):
        report = self.selected_report()
        first = self.task(report=report)
        second = self.task("two", report=report, sleep=0.1)
        original_collect = suite.collect_report

        def collect(*args):
            self.assertTrue(first.with_suffix(".done").exists())
            self.assertTrue(second.with_suffix(".done").exists())
            return original_collect(*args)

        def validate(selected):
            # The checker must start after both invocations have returned reports.
            self.assertEqual(len(first.with_suffix(".calls").read_text().splitlines()), 1)
            self.assertEqual(len(second.with_suffix(".calls").read_text().splitlines()), 1)
            self.assertTrue(first.with_suffix(".done").exists())
            self.assertTrue(second.with_suffix(".done").exists())
            self.assertEqual(selected["reports"][0]["info"], report["reports"][1]["info"])
            return [{"group": 0, "status": "ACCEPT"}]

        with mock.patch.object(suite, "collect_report", side_effect=collect) as collector, \
                mock.patch.object(sweep_bmc_execution, "validate_report", side_effect=validate) as check:
            code, summary, results = self.run_evidence("--bmc-deadline-ms", "20000",
                                                       "--bmc-solver-timeout-ms", "1000")
        self.assertEqual(code, 0)
        self.assertEqual(check.call_count, 2)
        self.assertEqual(collector.call_count, 2)
        self.assertTrue(summary["complete"])
        self.assertTrue(summary["execution_checks_passed"])
        self.assertEqual(summary["validator_start"], summary["validator_end"])
        self.assertEqual(summary["selected_executions"], 2)
        self.runtime.verify_runtime.assert_called_once()
        for source, row in zip((first, second), results):
            calls = source.with_suffix(".calls").read_text().splitlines()
            self.assertEqual(len(calls), 1)
            command = json.loads(calls[0])
            self.assertIn("--bmc-deadline-ms=20000", command)
            self.assertIn("--bmc-solver-timeout-ms=1000", command)
            self.assertIn("--format=json", command)
            self.assertFalse(Path(command[command.index("-o") + 1]).parent.exists())
            self.assertEqual(row["executions"][0]["group"], 1)
            with gzip.open(self.root / "evidence" / row["evidence"], "rt") as stream:
                selected = json.load(stream)["reports"][0]
                self.assertEqual(selected["info"], report["reports"][1]["info"])
                self.assertEqual(selected["statements"], [23])
                self.assertEqual(selected["original_group"], 1)
            self.assertNotIn("raw_report", row)
        self.assertFalse((self.root / "evidence/.raw").exists())
        self.assertEqual(len((self.root / "evidence/results.jsonl").read_text().splitlines()), 2)

    def test_missing_report_fails_without_rerun(self):
        source = self.task()
        code, summary, results = self.run_evidence()
        self.assertEqual(code, 2)
        self.assertTrue(summary["complete"])
        self.assertFalse(summary["execution_checks_passed"])
        self.assertEqual(results[0]["report_status"], "missing")
        self.assertEqual(len(source.with_suffix(".calls").read_text().splitlines()), 1)

    def test_database_capture_is_offline_and_uses_same_invocation(self):
        report = self.selected_report()
        sources = [self.task(report=report), self.task("two", report=report, sleep=0.1)]

        def export(database, destination, command):
            self.assertTrue(all(source.with_suffix(".done").exists() for source in sources))
            self.assertEqual(command, [sys.executable, str(self.fake)])
            destination.write_text(database.read_text())

        with mock.patch.object(suite, "export_database_report", side_effect=export) as exporter, \
                mock.patch.object(sweep_bmc_execution, "validate_report", side_effect=lambda report: [{"group": 0, "status": "ACCEPT"}]):
            code, summary, rows = self.run_evidence("--evidence-source", "database")
        self.assertEqual(code, 0)
        self.assertEqual(exporter.call_count, 2)
        self.assertTrue(summary["execution_checks_passed"])
        self.assertEqual(summary["evidence_source"], "database")
        for source, row in zip(sources, rows):
            calls = source.with_suffix(".calls").read_text().splitlines()
            self.assertEqual(len(calls), 1)
            self.assertNotIn("--format=json", json.loads(calls[0]))
            self.assertEqual(len(row["database_sha256"]), 64)
            self.assertNotIn("raw_database", row)
            with gzip.open(self.root / "evidence" / row["evidence"], "rt") as stream:
                self.assertEqual(json.load(stream)["reports"][0]["info"], report["reports"][1]["info"])
        self.assertFalse((self.root / "evidence/.raw").exists())

    def test_database_mutation_rejects_evidence_without_rerun(self):
        source = self.task(report=self.selected_report())

        def export(database, destination, command):
            destination.write_text(database.read_text())
            database.write_text("changed")

        with mock.patch.object(suite, "export_database_report", side_effect=export):
            code, summary, rows = self.run_evidence("--evidence-source", "database")
        self.assertEqual(code, 2)
        self.assertFalse(summary["execution_checks_passed"])
        self.assertIn("database changed", summary["failure"])
        self.assertTrue((self.root / "evidence" / rows[0]["raw_database"]).is_file())
        self.assertEqual(len(source.with_suffix(".calls").read_text().splitlines()), 1)

    def test_missing_database_cannot_certify_false(self):
        source = self.task(database=False)
        code, summary, rows = self.run_evidence("--evidence-source", "database")
        self.assertEqual(code, 2)
        self.assertFalse(summary["execution_checks_passed"])
        self.assertEqual(rows[0]["report_status"], "missing")
        self.assertEqual(len(source.with_suffix(".calls").read_text().splitlines()), 1)

    def test_database_source_requires_evidence_directory(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            suite.main([str(self.bench), "--evidence-source", "database"])
        self.assertEqual(error.exception.code, 2)

    def test_failed_database_invocation_never_supplies_evidence(self):
        source = self.task(report=self.selected_report(), exit=2, database_sidecar=True)
        with mock.patch.object(suite, "export_database_report") as exporter:
            code, summary, rows = self.run_evidence("--evidence-source", "database")
        exporter.assert_not_called()
        self.assertEqual(code, 0)  # Complete scan with a separately counted tool error.
        self.assertEqual(summary["counts"]["err"], 1)
        self.assertEqual(rows[0]["executions"], [])
        self.assertEqual(len(source.with_suffix(".calls").read_text().splitlines()), 1)
        self.assertFalse((self.root / "evidence/.raw").exists())

    def test_unknown_diagnostics_survive_raw_cleanup(self):
        self.task(output="The program is UNKNOWN", report={"reports": [{"info": {
            "unknown_reason": "unknown destination write effects not covered",
            "bmc_result": {"status": "UNSUPPORTED"},
            "bmc": {"counts": {"prefix_address": 2}}}}]})
        code, summary, results = self.run_evidence()
        self.assertEqual(code, 0)
        self.assertEqual(results[0]["unknown_reason_counts"],
                         {"unknown destination write effects not covered": 1})
        self.assertEqual(results[0]["bmc_reason_counts"], {"prefix_address": 2})
        self.assertEqual(results[0]["bmc_status_counts"], {"UNSUPPORTED": 1})
        self.assertFalse((self.root / "evidence/.raw").exists())

    def test_rejected_execution(self):
        self.task(report=self.selected_report())
        code, summary, results = self.run_evidence()
        self.assertEqual(code, 2)
        self.assertFalse(summary["execution_checks_passed"])
        self.assertEqual(results[0]["executions"][0]["status"], "REJECT")

    def test_empty_selected_execution_rejected(self):
        self.task(report={"reports": []})
        code, summary, results = self.run_evidence()
        self.assertEqual(code, 2)
        self.assertFalse(summary["execution_checks_passed"])
        self.assertEqual(results[0]["executions"], [])

    def test_missing_selected_replay_is_unsupported(self):
        self.task(report={"reports": [{"info": {"confirmed_pair_id": 7}}]})
        code, summary, results = self.run_evidence()
        self.assertEqual(code, 2)
        self.assertFalse(summary["execution_checks_passed"])
        self.assertEqual(results[0]["executions"][0]["status"], "UNSUPPORTED")

    def test_candidate_replay_preserves_mapping_without_bulk_diagnostics(self):
        report = self.selected_report()
        info = report["reports"][1]["info"]
        replay = info.pop("counterexample")
        info["bmc"] = {"candidates": [{"pair_id": 9, "replay": replay}], "large_diagnostic": []}
        path = self.root / "report.json"
        path.write_text(json.dumps(report))
        result = suite.collect_report(path, suite.VERDICT_RACE, self.root, "candidate")
        with gzip.open(self.root / result["evidence"], "rt") as stream:
            selected = json.load(stream)["reports"][0]
        self.assertEqual(selected["info"]["counterexample"], replay)
        self.assertEqual(selected["info"]["access_a"], info["access_a"])
        self.assertEqual(selected["info"]["access_b"], info["access_b"])
        self.assertEqual(selected["info"]["thread_creations"], info["thread_creations"])
        self.assertNotIn("bmc", selected["info"])
        self.assertEqual(selected["original_group"], 1)

    def test_invalid_report(self):
        self.task(report={"not_reports": []})
        code, summary, results = self.run_evidence()
        self.assertEqual(code, 2)
        self.assertEqual(results[0]["report_status"], "invalid")

    def test_runtime_change_cannot_pass(self):
        self.task(expected="true", output="The program is SAFE", report={"reports": []})
        self.runtime.verify_runtime.side_effect = RuntimeError("runtime changed")
        code, summary, results = self.run_evidence()
        self.assertEqual(code, 2)
        self.assertTrue(summary["complete"])
        self.assertFalse(summary["execution_checks_passed"])
        self.assertEqual(summary["failure"], "runtime changed")

    def test_interruption_retains_progress(self):
        self.task(expected="true", output="The program is SAFE", report={"reports": []}, sleep=0.1)
        child_source = self.task("two", child=True)
        real_completed = suite.as_completed

        def interrupt(futures):
            yield next(real_completed(futures))
            raise KeyboardInterrupt

        with mock.patch.object(suite, "as_completed", side_effect=interrupt), \
                mock.patch.object(suite, "collect_report") as collector:
            code, summary, results = self.run_evidence()
        collector.assert_not_called()
        self.assertEqual(code, 130)
        self.assertFalse(summary["complete"])
        self.assertFalse(summary["execution_checks_passed"])
        self.assertEqual(summary["status"], "interrupted")
        self.assertEqual(len(results), 1)
        self.assertEqual(len((self.root / "evidence/results.jsonl").read_text().splitlines()), 1)
        self.assertTrue((self.root / "evidence" / results[0]["raw_report"]).is_file())
        pid = int(child_source.with_suffix(".child").read_text())
        self.addCleanup(self.kill_if_live, pid)
        self.assert_process_stopped(pid)

    @unittest.skipUnless(os.name == "posix", "POSIX signals and process groups")
    def test_sigterm_cleans_children_and_preserves_partial_reports(self):
        self.task(expected="true", output="The program is SAFE", report={"reports": []})
        child_source = self.task("two", child=True)
        destination = self.root / "evidence"
        driver = self.root / "driver.py"
        driver.write_text('''import json, pathlib, signal, sys, types
sys.path.insert(0, sys.argv[1])
import svcomp_race_test as suite
sys.modules['ikos_runtime'] = types.SimpleNamespace(
    runtime_identity=lambda path: {'command': [sys.executable, str(path)]},
    verify_runtime=lambda identity: None)
signal.signal(signal.SIGTERM, signal.SIG_IGN)
code = suite.main([sys.argv[2], '--ikos', sys.argv[3], '--jobs', '2',
                   '--evidence-dir', sys.argv[4]])
pathlib.Path(sys.argv[4], 'handler.json').write_text(json.dumps({
    'restored': signal.getsignal(signal.SIGTERM) == signal.SIG_IGN}))
raise SystemExit(code)
''')
        proc = subprocess.Popen([sys.executable, str(driver), str(Path(suite.__file__).parent),
                                 str(self.bench), str(self.fake), str(destination)],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(self.kill_if_live, proc.pid)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if child_source.with_suffix(".child").exists() and (destination / "results.jsonl").exists():
                break
            time.sleep(0.01)
        else:
            self.fail("fake child/results were not ready for SIGTERM")
        pid = int(child_source.with_suffix(".child").read_text())
        self.addCleanup(self.kill_if_live, pid)
        proc.terminate()
        stdout, stderr = proc.communicate(timeout=5)
        self.assertEqual(proc.returncode, 130, stdout + stderr)
        self.assert_process_stopped(pid)
        summary = json.loads((destination / "summary.json").read_text())
        results = json.loads((destination / "results.json").read_text())
        self.assertEqual(summary["status"], "interrupted")
        self.assertFalse(summary["complete"])
        self.assertFalse(summary["execution_checks_passed"])
        self.assertEqual(len(results), 1)
        self.assertTrue((destination / results[0]["raw_report"]).is_file())
        self.assertTrue(json.loads((destination / "handler.json").read_text())["restored"])

    def test_timeout_is_collected_but_not_a_successful_verdict(self):
        self.task(sleep=3)
        code, summary, results = self.run_evidence("--timeout", "1")
        self.assertEqual(code, 0)
        self.assertTrue(summary["complete"])
        self.assertEqual(summary["counts"]["err"], 1)
        self.assertEqual(summary["counts"]["race_population"], 1)
        self.assertEqual(summary["counts"]["race_coverage"], 0)
        self.assertEqual(results[0]["verdict"], "Error (timeout)")

    def test_validator_change_cannot_pass(self):
        self.task(expected="true", output="The program is SAFE", report={"reports": []})
        original_hash = suite.file_sha256
        calls = 0

        def changed_hash(path):
            nonlocal calls
            if Path(path).name == "validate_bmc_execution.py":
                calls += 1
                if calls > 1:
                    return "changed"
            return original_hash(path)

        with mock.patch.object(suite, "file_sha256", side_effect=changed_hash):
            code, summary, results = self.run_evidence()
        self.assertEqual(code, 2)
        self.assertFalse(summary["execution_checks_passed"])
        self.assertNotEqual(summary["validator_start"], summary["validator_end"])

    def test_coverage_includes_unknown_and_errors(self):
        rows = [("a", suite.VERDICT_RACE, "ILP32", suite.VERDICT_RACE),
                ("b", suite.VERDICT_RACE, "ILP32", suite.VERDICT_UNKNOWN),
                ("c", suite.VERDICT_RACE, "ILP32", "Error (timeout)"),
                ("d", suite.VERDICT_SAFE, "ILP32", suite.VERDICT_SAFE),
                ("e", suite.VERDICT_SAFE, "ILP32", suite.VERDICT_UNKNOWN)]
        counts = suite.confusion(rows)
        self.assertEqual(counts["race_coverage"], 1 / 3)
        self.assertEqual(counts["safe_coverage"], 1 / 2)
        self.assertNotIn("recall", counts)

    @unittest.skipUnless(os.name == "posix", "POSIX process groups")
    def test_timeout_kills_wrapper_child(self):
        source = self.task(child=True)
        verdict, elapsed = suite.run_one(
            source, self.fake, 0.3, self.root / "output.db", "auto", "ILP32", False,
            command=[sys.executable, str(self.fake)])
        self.assertEqual(verdict, "Error (timeout)")
        self.assertLess(elapsed, 2)
        pid = int(source.with_suffix(".child").read_text())
        self.addCleanup(self.kill_if_live, pid)
        self.assert_process_stopped(pid)

    @unittest.skipUnless(os.name == "posix", "POSIX process groups")
    def test_seven_argument_call_compatibility(self):
        self.fake.write_text("#!" + sys.executable + "\n" + FAKE)
        self.fake.chmod(0o755)
        source = self.task(output="The program is SAFE")
        verdict, elapsed = suite.run_one(source, self.fake, 5, self.root / "db", "auto", "ILP32", False)
        self.assertEqual(verdict, suite.VERDICT_SAFE)

    @staticmethod
    def kill_if_live(pid):
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass

    def assert_process_stopped(self, pid):
        for _ in range(50):
            try:
                state = Path(f"/proc/{pid}/stat").read_text().split()[2]
            except FileNotFoundError:
                return
            if state == "Z":
                return
            time.sleep(0.01)
        self.fail(f"child {pid} survived timeout")


if __name__ == "__main__":
    unittest.main()
