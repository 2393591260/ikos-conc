#!/usr/bin/env python3
"""Check birth ordering separately from ordinary shared-data race exclusion."""
import argparse
import json
import os
import re
from pathlib import Path
import sqlite3
import subprocess
import tempfile

from ikos_runtime import runtime_command, runtime_identity, verify_runtime

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--bits', choices=('32', '64'), default='32')
    args = parser.parse_args()
    command = runtime_command(args.ikos.resolve())
    identity = runtime_identity(args.ikos.resolve())
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/first-publication.c').read_text()
    env = dict(os.environ)
    env.pop('IKOS_BMC_RECOVER', None)
    positives = {0, 16, 20}
    with tempfile.TemporaryDirectory(prefix='ikos-first-publication-') as temp:
        work = Path(temp)
        for wide in (False, True):
            for case in range(22):
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') + fixture)
                dbpath = work / 'output.db'
                result = subprocess.run([*command, '--analyses=race',
                    '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                    '--bmc-deadline-ms=0', '--log=debug', '-m' + args.bits, '-o', str(dbpath), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                output = result.stdout + result.stderr
                assert result.returncode == 0, (case, wide, output)
                safe = 'The program is SAFE' in output
                with sqlite3.connect(dbpath) as db:
                    proofs = [json.loads(text) for text, in db.execute(
                        "SELECT info FROM checks WHERE info LIKE '%closed-first-publication-v1%'")]
                if case in positives:
                    assert safe and proofs, (case, wide, output, proofs)
                else:
                    assert not safe, (case, wide, output)
                if case == 5:
                    # Good/Good is deliberately not an exclusion, even if some
                    # birth pairs received a sound order certificate.
                    assert proofs, (case, wide, output)
                print(f'first publication={case}/wide={wide}: safe={safe}, proofs={len(proofs)}', flush=True)

        for wide in (False, True):
            source = work / 'permuted.c'
            renamed = fixture
            for old, new in (('initialize', 'construct'), ('insert', 'publish'),
                             ('take', 'traverse'), ('head', 'anchor'), ('next', 'link_field')):
                renamed = renamed.replace(old, new)
            source.write_text('#define COUNT 10000\n' + ('#define WIDE\n' if wide else '') + renamed)
            proc = subprocess.run([*command, '--analyses=race', '--concurrency=auto',
                '--partitioning=guards', '--demote-race-to-unknown', '--bmc-deadline-ms=0', '-m' + args.bits,
                '-o', str(work / 'output.db'), str(source)], env=env, capture_output=True, text=True, timeout=30)
            assert proc.returncode == 0 and 'The program is SAFE' in proc.stdout + proc.stderr, (wide, proc.stdout, proc.stderr)
            print(f'renamed publication/10000/wide={wide}: SAFE', flush=True)

        graph_fixture = fixture
        fixture = (ROOT / 'analyzer/test/regression/bmc-values/first-publication-arrays.c').read_text()
        for wide in (False, True):
            for case in (*range(8), 9):
                source = work / 'renamed.c'
                source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') + fixture)
                dbpath = work / 'output.db'
                result = subprocess.run([*command, '--analyses=race',
                    '--concurrency=auto', '--partitioning=guards', '--demote-race-to-unknown',
                    '--bmc-deadline-ms=0', '--log=debug', '-m' + args.bits, '-o', str(dbpath), str(source)],
                    env=env, capture_output=True, text=True, timeout=30)
                output = result.stdout + result.stderr
                assert result.returncode == 0, (case, wide, output)
                with sqlite3.connect(dbpath) as db:
                    proofs = list(db.execute("SELECT info FROM checks WHERE info LIKE '%closed-first-publication-v1%'"))
                safe = 'The program is SAFE' in output
                if case == 0:
                    assert safe and proofs, (case, wide, output, proofs)
                elif case == 9:
                    assert not safe and not proofs, (case, wide, output)
                else:
                    assert not safe, (case, wide, output)
                print(f'array publication={case}/wide={wide}: safe={safe}, proofs={len(proofs)}', flush=True)


        def check_negative(name, text, llvm=False, positive=False):
            source = work / ('damaged.ll' if llvm else 'damaged.c')
            source.write_text(text)
            dbpath = work / 'output.db'
            proc = subprocess.run([*command, '--analyses=race', '--concurrency=auto',
                '--partitioning=guards', '--demote-race-to-unknown', '--bmc-deadline-ms=0',
                '--log=debug', '-m' + args.bits, '-o', str(dbpath), str(source)],
                env=env, capture_output=True, text=True, timeout=30)
            output = proc.stdout + proc.stderr
            assert proc.returncode == 0, (name, output)
            with sqlite3.connect(dbpath) as db:
                proofs = list(db.execute(
                    "SELECT info FROM checks WHERE info LIKE '%closed-first-publication-v1%'"))
            if positive:
                assert proofs and 'The program is SAFE' in output, (name, output, proofs)
            else:
                assert not proofs and 'The program is SAFE' not in output, (name, output, proofs)
            print(f'first publication native/{name}/bits={args.bits}: positive={positive}', flush=True)

        word = int(args.bits) // 8
        broken = {
            'packed-graph': graph_fixture.replace('struct cell { payload datum; struct cell *next; };',
                'struct __attribute__((packed)) cell { char marker; payload datum; struct cell *next; };'),
            'partial-link': graph_fixture.replace('p->next = 0;',
                'p->next = 0; ((unsigned char *)&p->next)[' + str(word-1) + '] = 1;', 1),
            'wrong-allocation': graph_fixture.replace('malloc(sizeof *p)', 'malloc(sizeof *p - 1)'),
            'mutex-upper-byte': graph_fixture.replace('int main(void) {',
                'int main(void) { ((unsigned char *)&graph)[sizeof graph - 1] = 1;', 1),
            'partial-root': graph_fixture.replace(
                'for (int k = 0; k < COUNT; ++k) pthread_create(&b[k], 0, reader, 0);',
                'for (int k = 0; k < COUNT; ++k) pthread_create(&b[k], 0, reader, 0);' +
                ' ((unsigned char *)&head)[' + str(word-1) + '] = 1;', 1),
        }
        for name, text in broken.items():
            check_negative(name, text)

        compiler = json.loads(subprocess.check_output(command[:-1] + ['-c',
            'import json; from ikos import settings, analyzer; '
            'print(json.dumps([settings.clang(), analyzer.clang_ikos_flags()]))'], text=True))
        source = work / 'native.c'
        source.write_text(graph_fixture)
        native = work / 'native.ll'
        subprocess.run([compiler[0], *compiler[1], '-S', '-emit-llvm',
                        '-m' + args.bits, str(source), '-o', str(native)],
                       capture_output=True, check=True)
        original = native.read_text()
        check_negative('native-positive', original, llvm=True, positive=True)
        for place in ('declaration', 'call'):
            lines = original.splitlines()
            changed = 0
            for i, line in enumerate(lines):
                selected = line.startswith('declare ') if place == 'declaration' else ' call ' in line
                if selected and '@pthread_mutex_lock(' in line:
                    prefix, actuals = line.split('@pthread_mutex_lock(', 1)
                    needle = '%union.pthread_mutex_t*'
                    assert needle in actuals, line
                    actuals = actuals.replace(needle, needle + ' byval(%union.pthread_mutex_t)', 1)
                    lines[i] = prefix + '@pthread_mutex_lock(' + actuals
                    changed += 1
            assert changed > 0
            check_negative('byval-' + place, '\n'.join(lines) + '\n', llvm=True)
        misaligned, changes = re.subn(r'(^@graph = [^\n]*\balign )\d+',
                                     r'\g<1>1', original, flags=re.MULTILINE)
        assert changes == 1
        check_negative('mutex-alignment', misaligned, llvm=True)
        pointer = args.bits
        index = '16' if pointer == '32' else '32'
        layout_line = next(line for line in original.splitlines()
                           if line.startswith('target datalayout = '))
        layout = layout_line.split('"')[1].split('-')
        layout = [field for field in layout if not field.startswith('p:')]
        damaged_pointer = 'p:' + pointer + ':' + pointer + ':' + pointer + ':' + index
        layout.insert(1, damaged_pointer)
        assert [field for field in layout if field.startswith('p:')] == [damaged_pointer]
        narrowed = original.replace(layout_line, 'target datalayout = "' + '-'.join(layout) + '"', 1)
        assert narrowed != original
        check_negative('native-index-width', narrowed, llvm=True)
    verify_runtime(identity)


if __name__ == '__main__':
    main()
