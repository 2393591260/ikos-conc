#!/usr/bin/env python3
"""Ensure packaged settings use bundled LLVM after moving the tool directory."""

import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("svcomp_package", ROOT / "svcomp/package.py")
package = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(package)


class PackageSettingsTests(unittest.TestCase):
    def check_relocation(self, prefix_assignment):
        with tempfile.TemporaryDirectory(prefix="ikos-package-") as directory:
            prefix = Path(directory) / "original tool"
            settings = prefix / "libexec/lib/python3.12/site-packages/ikos/settings/__init__.py"
            settings.parent.mkdir(parents=True)
            settings.write_text(
                "import os\n" + prefix_assignment + "\n"
                "LLVM_PREFIX = '/usr/lib/llvm-14'\n"
                "LLVM_BIN_DIR = '/usr/lib/llvm-14/bin'\n"
                "LLVM_INCLUDE_DIR = '/usr/lib/llvm-14/include'\n"
                "LLVM_LIB_DIR = '/usr/lib/llvm-14/lib'\n"
                "CLANG = '/usr/lib/llvm-14/bin/clang'\n"
                "CLANGXX = '/usr/lib/llvm-14/bin/clang++'\n"
            )
            clang = prefix / "llvm/bin/clang"
            clang.parent.mkdir(parents=True)
            clang.write_text("#!/bin/sh\nprintf 'bundled clang\\n'\n")
            clang.chmod(0o755)
            package.patch_settings(str(settings))
            patched = settings.read_bytes()
            package.patch_settings(str(settings))
            self.assertEqual(settings.read_bytes(), patched)

            moved = Path(directory) / "relocated tool"
            shutil.move(str(prefix), moved)
            settings = moved / settings.relative_to(prefix)
            values = {"__file__": str(settings)}
            exec(compile(settings.read_text(), str(settings), "exec"), values)
            self.assertEqual(Path(values["PREFIX"]), moved)
            self.assertEqual(Path(values["LLVM_PREFIX"]), moved / "llvm")
            self.assertEqual(Path(values["LLVM_BIN_DIR"]), moved / "llvm/bin")
            self.assertEqual(Path(values["LLVM_INCLUDE_DIR"]), moved / "llvm/include")
            self.assertEqual(Path(values["LLVM_LIB_DIR"]), moved / "llvm/lib")
            self.assertEqual(Path(values["CLANGXX"]), moved / "llvm/bin/clang++")
            result = subprocess.run(
                [values["CLANG"]], capture_output=True, text=True, check=True
            )
            self.assertEqual(result.stdout, "bundled clang\n")

    def test_absolute_install_prefix(self):
        self.check_relocation("PREFIX = '/original/install'")

    def test_relocatable_prefix_with_system_llvm(self):
        self.check_relocation(
            "PREFIX = os.path.abspath(\n"
            "    os.path.join(os.path.dirname(os.path.abspath(__file__)),\n"
            "                 *([os.pardir] * 6)))"
        )


if __name__ == "__main__":
    unittest.main()
