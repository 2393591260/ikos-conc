#!/usr/bin/env python3
"""Check native GNU/Linux layouts and the competition entry on both ABIs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from ikos_runtime import runtime_identity, verify_runtime


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--bits', type=int, choices=(32, 64))
    args = parser.parse_args()
    ikos = args.ikos.resolve()
    identity = runtime_identity(ikos)
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')
    for name in ('PYTHONPATH', 'PYTHONHOME'):
        env.pop(name, None)
    entry = [sys.executable, str(ROOT / 'svcomp_witness.py'), '--ikos', str(ikos)]
    with tempfile.TemporaryDirectory(prefix='ikos-native-abi-') as temp:
        work = Path(temp)
        prop = work / 'no-data-race.prp'
        prop.write_text('CHECK(init(main()), LTL(G ! data-race))\n')
        for bits in ([args.bits] if args.bits else (32, 64)):
            model = 'ILP32' if bits == 32 else 'LP64'
            width, mutex = bits // 8, 24 if bits == 32 else 40
            header = '#include <pthread.h>\n#include <stddef.h>\n'
            layout = ''.join(
                f'_Static_assert(sizeof({typ}) == {size}, "{typ}");\n'
                for typ, size in [('int', 4), ('long long', 8), ('long', width),
                                  ('void *', width), ('size_t', width),
                                  ('pthread_t', width), ('pthread_mutex_t', mutex)])
            race = ('struct S { void *p; long long value; };\nstruct S shared;\n'
                    'void *worker(void *p) { shared.value = 1; return 0; }\n'
                    'int main(void) { pthread_t t; pthread_create(&t, 0, worker, 0);\n'
                    '  shared.value = 2; pthread_join(t, 0); return 0; }\n')
            safe = ('pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;\nlong long shared;\n'
                    'void *worker(void *p) { pthread_mutex_lock(&lock); shared = 1;\n'
                    '  pthread_mutex_unlock(&lock); return 0; }\n'
                    'int main(void) { pthread_t t; pthread_create(&t, 0, worker, 0);\n'
                    '  pthread_mutex_lock(&lock); shared = 2;\n'
                    '  pthread_mutex_unlock(&lock); pthread_join(t, 0); return 0; }\n')
            cases = [('layout', layout + 'int main(void) { return 0; }\n', 'SAFE'),
                     ('race', race, 'FALSE'), ('safe', safe, 'SAFE'),
                     ('unknown', race, 'UNKNOWN'), ('compile-error', 'int main( {', 'ERROR')]
            for name, body, expected in cases:
                source, witness = work / 'native.c', work / 'witness.yml'
                source.write_text(header + body)
                witness.write_text('stale')
                cmd = entry + ['--data-model', model, '--property', str(prop),
                               '--out', str(witness)]
                if expected == 'UNKNOWN':
                    cmd += ['--bmc-deadline-ms', '0']
                result = subprocess.run(cmd + [str(source)], cwd=work, env=env,
                                        capture_output=True, text=True, timeout=30)
                assert result.returncode == (1 if expected == 'ERROR' else 0), (model, name, result)
                if expected == 'FALSE':
                    assert 'The program is definitely UNSAFE' in result.stdout, result
                    task = json.loads(witness.read_text())[0]['metadata']['task']
                    assert task['data_model'] == model, task
                    assert task['input_file_hashes'][source.name] == hashlib.sha256(source.read_bytes()).hexdigest()
                else:
                    assert not witness.exists(), (model, name)
                    if expected == 'SAFE':
                        assert 'The program is SAFE' in result.stdout, result
                    elif expected == 'UNKNOWN':
                        assert any(s in result.stdout for s in ('The program is UNKNOWN',
                                   'The program is potentially UNSAFE')), result
                    else:
                        assert not result.stdout and result.stderr, result
                print(f'{model} {name}: {expected}', flush=True)
        result = subprocess.run(entry + ['--data-model', 'INVALID', str(source)],
                                cwd=work, env=env, capture_output=True, text=True, timeout=30)
        assert result.returncode == 2 and result.stderr and not result.stdout, result
    verify_runtime(identity)


if __name__ == '__main__':
    main()
