#!/usr/bin/env python3
"""Check the real BenchExec interface, including package-root relocation."""

import importlib.util
from pathlib import Path
import tempfile
import unittest

from benchexec import result, util
from benchexec.tools.template import BaseTool2, UnsupportedFeatureException

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "ikos_conc", ROOT / "benchexec/tools/ikos-conc.py"
)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class AdapterTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="ikos adapter ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        for directory in MODULE.Tool.REQUIRED_PATHS:
            (self.root / directory).mkdir()
        self.executable = self.root / "bin/ikos"
        self.executable.write_text("#!/bin/sh\nexit 0\n")
        self.executable.chmod(0o755)
        self.property = self.root / "race.prp"
        self.property.write_text("CHECK( init(main()), LTL(G ! data-race) )\n")
        self.tool = MODULE.Tool()

    def task(self, model="ILP32", files=None):
        return BaseTool2.Task.with_files(
            files or ["input.c"],
            property_file=str(self.property),
            options={"language": "C", "data_model": model},
        )

    def test_package_root_and_runtime_transfer(self):
        executable = self.tool.executable(
            BaseTool2.ToolLocator(tool_directory=str(self.root))
        )
        self.assertEqual(Path(executable), self.executable)
        paths = {Path(p).resolve() for p in self.tool.program_files(executable)}
        self.assertTrue(all(self.root / p in paths for p in MODULE.Tool.REQUIRED_PATHS))

    def test_task_model_overrides_conflicting_options(self):
        for model in ("ILP32", "LP64"):
            command = self.tool.cmdline(
                str(self.executable), ["--data-model", "LP64"], self.task(model), None
            )
            self.assertEqual(command[-3:], ["--data-model", model, "input.c"])
            self.assertEqual(Path(command[1]), self.root / "bin/svcomp_witness.py")

    def test_reject_other_properties(self):
        self.property.write_text(
            "CHECK( init(main()), LTL(G ! call(__VERIFIER_error())) )\n"
        )
        with self.assertRaises(UnsupportedFeatureException):
            self.tool.cmdline(str(self.executable), [], self.task(), None)

    def test_reject_unsupported_inputs(self):
        for task in (
            self.task("LLP64"),
            self.task(files=["a.c", "b.c"]),
            BaseTool2.Task.without_files("identifier"),
        ):
            with self.subTest(task=task), self.assertRaises(
                UnsupportedFeatureException
            ):
                self.tool.cmdline(str(self.executable), [], task, None)

    def test_result_trust_boundary(self):
        cases = [
            ("The program is SAFE", 0, None, result.RESULT_TRUE_PROP),
            ("The program is definitely UNSAFE", 0, None, result.RESULT_FALSE_PROP),
            ("The program is potentially UNSAFE", 0, None, result.RESULT_UNKNOWN),
            ("The program is UNKNOWN", 0, None, result.RESULT_UNKNOWN),
            ("The program is SAFE", 1, None, result.RESULT_UNKNOWN),
            ("The program is definitely UNSAFE", 1, None, result.RESULT_UNKNOWN),
            ("The program is SAFE", 0, "walltime", result.RESULT_TIMEOUT),
        ]
        for text, code, termination, expected in cases:
            with self.subTest(text=text, code=code, termination=termination):
                run = BaseTool2.Run(
                    [],
                    util.ProcessExitCode.create(value=code),
                    BaseTool2.RunOutput([text]),
                    termination,
                )
                self.assertEqual(self.tool.determine_result(run), expected)
        run = BaseTool2.Run(
            [],
            util.ProcessExitCode.create(signal=9),
            BaseTool2.RunOutput(["The program is SAFE"]),
            None,
        )
        self.assertEqual(self.tool.determine_result(run), result.RESULT_UNKNOWN)


if __name__ == "__main__":
    unittest.main()
