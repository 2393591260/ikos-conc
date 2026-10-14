"""Identify the installed runtime actually used by an evaluation command.

Supports the standard bin/ikos + libexec virtualenv layout on Linux. This is an
identity check, not a sandbox or a snapshot of the operating system/headers.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


_PROBE = r"""
import json, sys
import ikos, ikos.analyzer, ikos.settings
s = ikos.settings
print(json.dumps({
    'executable': sys.executable, 'prefix': sys.prefix,
    'base_prefix': sys.base_prefix, 'version': sys.version,
    'package': list(ikos.__path__),
    'modules': {name: module.__file__ for name, module in sys.modules.items()
                if getattr(module, '__file__', None)},
    'bytecode': sorted({module.__cached__ for module in sys.modules.values()
                        if getattr(module, '__cached__', None)}),
    'tools': {name: getattr(s, name)() for name in
              ('ikos_analyzer', 'ikos_pp', 'clang', 'opt', 'llvm_link',
               'llvm_objdump', 'llvm_objcopy')},
    'settings': {name: getattr(s, name) for name in
                 ('PREFIX', 'BUILD_MODE', 'INCLUDE_DIR', 'LLVM_VERSION')}
}))
"""


def runtime_command(ikos: Path) -> list[str]:
    """Use the selected install's Python, irrespective of the copied shebang."""
    ikos = Path(os.path.abspath(ikos))
    prefix = ikos.parent.parent
    python = prefix / 'libexec/bin/python'
    if (ikos.name != 'ikos' or ikos.parent.name != 'bin'
            or not ikos.is_file() or not python.is_file()
            or not (prefix / 'libexec/pyvenv.cfg').is_file()):
        raise RuntimeError(f'unsupported IKOS install layout: {ikos}')
    # Isolation prevents PYTHONPATH/PYTHONHOME and user-site import substitution.
    return [str(python), '-I', '-B', str(ikos)]


def _file(path):
    path = Path(os.path.abspath(path))
    resolved = path.resolve(strict=True)
    before = resolved.stat()
    digest = hashlib.sha256()
    with resolved.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    after = resolved.stat()
    if (before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (
            after.st_size, after.st_mtime_ns, after.st_ctime_ns):
        raise RuntimeError(f'runtime file changed while hashing: {path}')
    return dict(path=str(path), resolved=str(resolved), sha256=digest.hexdigest())


def _inside(path, root, role):
    if not Path(path).resolve().is_relative_to(root.resolve()):
        raise RuntimeError(f'{role} resolves outside selected install: {path}')


def _tree(root, prefix):
    root = Path(root)
    _inside(root, prefix, 'module/include tree')
    files = {}
    for path in sorted(root.rglob('*')):
        _inside(path, prefix, 'module/include tree entry')
        if path.is_file():
            files[str(path.relative_to(root))] = _file(path)
        elif path.is_symlink():
            raise RuntimeError(f'unsupported directory symlink in runtime tree: {path}')
    if not files:
        raise RuntimeError(f'empty or missing runtime tree: {root}')
    digest = hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()
    return dict(path=str(root), sha256=digest, files=len(files))


def _dependencies(paths):
    """Hash resolved ELF dependencies; ldd does not invoke analyzer main()."""
    ldd = shutil.which('ldd')
    if not ldd:
        raise RuntimeError('ldd is required to identify runtime dependencies')
    dependencies = {}
    for path in sorted(set(paths)):
        with open(path, 'rb') as source:
            if source.read(4) != b'\x7fELF':
                continue
        result = subprocess.run([ldd, path], capture_output=True, text=True,
                                env=dict(os.environ, LC_ALL='C'), timeout=30)
        output = result.stdout + result.stderr
        if 'not found' in output:
            raise RuntimeError(f'unresolved runtime dependency: {path}')
        if result.returncode and not any(message in output for message in
                ('not a dynamic executable', 'statically linked')):
            raise RuntimeError(f'cannot resolve runtime dependencies: {path}')
        for line in output.splitlines():
            match = re.search(r'(?:=>\s+|^\s*)(/.*?)\s+\(0x[0-9a-f]+\)', line)
            if match:
                dependency = match.group(1)
                if dependency not in dependencies:
                    dependencies[dependency] = _file(dependency)
    return dict(resolver=_file(ldd), files=dependencies)


def runtime_identity(ikos: Path) -> dict:
    """Probe imports/settings with the command's interpreter and hash its inputs.

    Call before and after the run, with unchanged cwd/environment. Python's -B
    avoids cache writes; existing bytecode is included in the package digest.
    """
    try:
        command = runtime_command(ikos)
        prefix = Path(command[-1]).parent.parent
        result = subprocess.run(command[:-1] + ['-c', _PROBE],
                                capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise RuntimeError('selected IKOS Python cannot import/query its runtime')
        probe = json.loads(result.stdout)
        if os.path.abspath(probe['executable']) != command[0]:
            raise RuntimeError('Python executable differs from selected interpreter')
        if Path(probe['prefix']).resolve() != (prefix / 'libexec').resolve():
            raise RuntimeError('Python virtualenv resolves outside selected install')
        if Path(probe['settings']['PREFIX']).resolve() != prefix.resolve():
            raise RuntimeError('IKOS settings resolve outside selected install')
        _inside(command[-1], prefix, 'wrapper')
        for name, path in probe['modules'].items():
            if name == 'ikos' or name.startswith('ikos.'):
                _inside(path, prefix / 'libexec', f'Python module {name}')
        for name in ('ikos_analyzer', 'ikos_pp'):
            _inside(probe['tools'][name], prefix, name)
        for name, path in probe['tools'].items():
            if not Path(path).is_absolute() or not os.access(path, os.X_OK):
                raise RuntimeError(f'{name} is not an absolute executable: {path}')
        trees = [_tree(path, prefix) for path in probe.pop('package')]
        tools = {name: _file(path) for name, path in probe.pop('tools').items()}
        modules = {name: _file(path) for name, path in probe.pop('modules').items()}
        bytecode = [_file(path) for path in probe.pop('bytecode') if Path(path).is_file()]
        interpreter = _file(command[0])
        native_paths = [interpreter['resolved']]
        native_paths += [value['resolved'] for value in tools.values()]
        native_paths += [value['resolved'] for value in modules.values()]
        # Only execution-relevant variables, hashed to avoid publishing values.
        env_keys = {'PATH', 'LD_LIBRARY_PATH', 'LD_PRELOAD', 'LD_AUDIT',
                    'CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH',
                    'LIBRARY_PATH', 'LANG', 'LC_ALL', 'TMPDIR'}
        env_keys.update(key for key in os.environ if key.startswith('IKOS_'))
        environment = {key: hashlib.sha256(os.environ[key].encode()).hexdigest()
                       for key in sorted(env_keys) if key in os.environ}
        return dict(schema=1, command=command, cwd=os.getcwd(),
                    environment_sha256=environment, python=probe,
                    wrapper=_file(command[-1]), interpreter=interpreter,
                    venv_config=_file(prefix / 'libexec/pyvenv.cfg'),
                    module_trees=trees, modules=modules, bytecode=bytecode, tools=tools,
                    includes=_tree(probe['settings']['INCLUDE_DIR'], prefix),
                    dependencies=_dependencies(native_paths))
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        raise RuntimeError(f'cannot establish IKOS runtime identity: {error}') from error


def verify_runtime(identity: dict) -> None:
    """Reject results if the selected runtime changed since its initial probe."""
    current = runtime_identity(Path(identity['command'][-1]))
    changed = sorted(key for key in identity.keys() | current.keys()
                     if identity.get(key) != current.get(key))
    if changed:
        raise RuntimeError('IKOS runtime changed during evaluation: ' + ', '.join(changed))
