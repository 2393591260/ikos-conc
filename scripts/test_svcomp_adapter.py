#!/usr/bin/env python3
"""Check the real BenchExec interface, including package-root relocation."""

import importlib.util
import shutil
import subprocess
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
        self.executable = self.root / "bin/svcomp_witness.py"
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
            self.assertEqual(
                command[-5:],
                ["--data-model", model, "--property", str(self.property), "input.c"],
            )
            self.assertEqual(Path(command[0]), self.executable)

    def test_forward_other_properties(self):
        self.property.write_text(
            "CHECK( init(main()), LTL(G ! call(__VERIFIER_error())) )\n"
        )
        command = self.tool.cmdline(str(self.executable), [], self.task(), None)
        self.assertIn(str(self.property), command)

    def test_reject_unsupported_inputs(self):
        for task in (
            self.task("LLP64"),
            self.task(files=["a.c", "b.c"]),
            BaseTool2.Task.without_files("identifier"),
        ):
            with (
                self.subTest(task=task),
                self.assertRaises(UnsupportedFeatureException),
            ):
                self.tool.cmdline(str(self.executable), [], task, None)

    def test_no_task_model_is_invented(self):
        for options in (None, {}, {"language": "C"}, {"language": "C++"}):
            task = BaseTool2.Task.with_files(["input.c"], options=options)
            command = self.tool.cmdline(str(self.executable), [], task, None)
            self.assertEqual(command, [str(self.executable), "input.c"])

    def test_relocated_entry_version_and_property_validation(self):
        shutil.copyfile(ROOT / "svcomp_witness.py", self.executable)
        self.executable.chmod(0o755)
        ikos = self.root / "bin/ikos"
        ikos.write_text('#!/bin/sh\nprintf "ikos 3.5\\nNASA license\\n"\n')
        ikos.chmod(0o755)
        self.assertEqual(self.tool.version(str(self.executable)), "3.5")
        sentinel = self.root / "witness.yml"
        sentinel.write_text("keep")
        version = subprocess.run(
            [str(self.executable), "--version"],
            cwd=self.root,
            capture_output=True,
            text=True,
            check=True,
        )
        self.assertTrue(version.stdout.startswith("ikos 3.5\n"))
        self.assertEqual(sentinel.read_text(), "keep")
        self.property.write_text(
            "CHECK(init(main()), LTL(G ! call(__VERIFIER_error())))\n"
        )
        source = self.root / "input.c"
        source.write_text("int main(void) { return 0; }\n")
        command = self.tool.cmdline(
            str(self.executable), [], self.task(files=[str(source)]), None
        )
        process = subprocess.run(command, cwd=self.root, capture_output=True, text=True)
        self.assertNotEqual(process.returncode, 0)
        self.assertIn("only the no-data-race property is supported", process.stderr)
        self.assertFalse(sentinel.exists())

    def test_exact_output_lines(self):
        for output in (
            ["diagnostic: The program is SAFE"],
            ["The program is SAFE-ish"],
            ["error: analysis failed"],
            [""],
            [],
        ):
            with self.subTest(output=output):
                run = BaseTool2.Run(
                    [],
                    util.ProcessExitCode.create(value=0),
                    BaseTool2.RunOutput(output),
                    None,
                )
                self.assertEqual(self.tool.determine_result(run), result.RESULT_ERROR)

    def test_result_trust_boundary(self):
        cases = [
            ("The program is SAFE", 0, None, result.RESULT_TRUE_PROP),
            ("The program is definitely UNSAFE", 0, None, result.RESULT_FALSE_DATARACE),
            ("The program is potentially UNSAFE", 0, None, result.RESULT_UNKNOWN),
            ("The program is UNKNOWN", 0, None, result.RESULT_UNKNOWN),
            ("The program is SAFE", 0, "walltime", result.RESULT_TRUE_PROP),
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
        for exit_code in (
            util.ProcessExitCode.create(value=1),
            util.ProcessExitCode.create(signal=9),
        ):
            for output in (
                ["The program is SAFE"],
                ["The program is definitely UNSAFE"],
                ["The program is potentially UNSAFE"],
                ["The program is UNKNOWN"],
                [],
            ):
                with self.subTest(exit_code=exit_code, output=output):
                    run = BaseTool2.Run(
                        [], exit_code, BaseTool2.RunOutput(output), None
                    )
                    self.assertEqual(self.tool.determine_result(run), result.RESULT_ERROR)


if __name__ == "__main__":
    unittest.main()
