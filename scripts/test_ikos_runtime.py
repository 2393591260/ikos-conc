#!/usr/bin/env python3
"""Runtime identity checks with temporary installs; no analyzer is executed."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import venv

from ikos_runtime import _dependencies, runtime_command, runtime_identity, verify_runtime


class RuntimeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='ikos-runtime-')
        self.addCleanup(self.temp.cleanup)
        self.original = Path(self.temp.name) / 'original'
        self.candidate = Path(self.temp.name) / 'copied install'
        venv.EnvBuilder(with_pip=False, symlinks=True).create(self.original / 'libexec')
        package = (self.original / 'libexec/lib' /
                   f'python{sys.version_info.major}.{sys.version_info.minor}' /
                   'site-packages/ikos')
        package.mkdir()
        (package / '__init__.py').write_text('')
        (package / 'analyzer.py').write_text('VALUE = 1\n')
        (package / 'settings.py').write_text('''from pathlib import Path
PREFIX = str(Path(__file__).resolve().parents[5])
INCLUDE_DIR = str(Path(PREFIX) / 'include')
BUILD_MODE = 'Release'
LLVM_VERSION = 'fixture'
def ikos_analyzer(): return str(Path(PREFIX) / 'bin/ikos-analyzer')
def ikos_pp(): return str(Path(PREFIX) / 'bin/ikos-pp')
def clang(): return str(Path(PREFIX) / 'bin/clang')
opt = llvm_link = llvm_objdump = llvm_objcopy = clang
''')
        (self.original / 'bin').mkdir()
        for name in ('ikos-analyzer', 'ikos-pp', 'clang'):
            shutil.copyfile('/bin/true', self.original / 'bin' / name)
            (self.original / 'bin' / name).chmod(0o755)
        (self.original / 'include').mkdir()
        (self.original / 'include/intrinsic.h').write_text('/* fixture */\n')
        wrapper = self.original / 'bin/ikos'
        wrapper.write_text(f'#!{self.original}/libexec/bin/python\n'
                           'import ikos.analyzer\nprint(ikos.analyzer.__file__)\n')
        wrapper.chmod(0o755)
        shutil.copytree(self.original, self.candidate, symlinks=True)
        self.wrapper = self.candidate / 'bin/ikos'
        self.package = self.candidate / package.relative_to(self.original)

    def test_copy_ignores_old_shebang_and_python_environment(self):
        original_package = self.original / self.package.relative_to(self.candidate)
        actual = subprocess.check_output([str(self.wrapper)], text=True).strip()
        self.assertEqual(actual, str(original_package / 'analyzer.py'))
        with patch.dict(os.environ, {'PYTHONPATH': str(original_package.parent),
                                     'PYTHONHOME': '/does/not/exist'}):
            command = runtime_command(self.wrapper)
            actual = subprocess.check_output(command, text=True).strip()
            self.assertEqual(actual, str(self.package / 'analyzer.py'))
            identity = runtime_identity(self.wrapper)
            self.assertEqual(identity['tools']['ikos_analyzer']['path'],
                             str(self.candidate / 'bin/ikos-analyzer'))
            self.assertEqual(identity['command'], command)
            verify_runtime(json.loads(json.dumps(identity)))

    def test_detects_runtime_changes(self):
        identity = runtime_identity(self.wrapper)
        for path in (self.wrapper, self.package / 'analyzer.py',
                     self.candidate / 'bin/ikos-analyzer',
                     self.candidate / 'bin/ikos-pp',
                     self.candidate / 'bin/clang',
                     self.candidate / 'include/intrinsic.h'):
            with self.subTest(path=path):
                before = path.read_bytes()
                path.write_bytes(before + b'\n# changed\n')
                try:
                    with self.assertRaisesRegex(RuntimeError, 'runtime changed'):
                        verify_runtime(identity)
                finally:
                    path.write_bytes(before)

    def test_rejects_tools_or_modules_from_other_install(self):
        analyzer = self.candidate / 'bin/ikos-analyzer'
        analyzer.unlink()
        analyzer.symlink_to(self.original / 'bin/ikos-analyzer')
        with self.assertRaisesRegex(RuntimeError, 'outside selected install'):
            runtime_identity(self.wrapper)
        analyzer.unlink()
        shutil.copyfile(self.original / 'bin/ikos-analyzer', analyzer)
        (self.package / 'analyzer.py').unlink()
        (self.package / 'analyzer.py').symlink_to(
            self.original / self.package.relative_to(self.candidate) / 'analyzer.py')
        with self.assertRaisesRegex(RuntimeError, 'outside selected install'):
            runtime_identity(self.wrapper)

    def test_rejects_missing_layout(self):
        (self.candidate / 'libexec/pyvenv.cfg').unlink()
        with self.assertRaisesRegex(RuntimeError, 'unsupported IKOS install layout'):
            runtime_command(self.wrapper)

    def test_detects_effective_environment_change(self):
        identity = runtime_identity(self.wrapper)
        with patch.dict(os.environ, {'IKOS_BMC_RECOVER': 'changed'}):
            with self.assertRaisesRegex(RuntimeError, 'environment_sha256'):
                verify_runtime(identity)

    def test_dependency_hashes_and_missing_dependency(self):
        library = self.candidate / 'libfixture.so'
        library.write_bytes(b'library before')
        with patch('ikos_runtime.subprocess.run') as run:
            run.return_value = subprocess.CompletedProcess(
                [], 0, f'libfixture.so => {library} (0x1234)\n', '')
            before = _dependencies(['/bin/true'])
            library.write_bytes(b'library after')
            self.assertNotEqual(before, _dependencies(['/bin/true']))
            run.return_value.stdout = 'libfixture.so => not found\n'
            with self.assertRaisesRegex(RuntimeError, 'unresolved runtime dependency'):
                _dependencies(['/bin/true'])


if __name__ == '__main__':
    unittest.main()
