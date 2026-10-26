#!/usr/bin/env python3
"""Native ABI controls for owned bucket and shared MAY node-graph certificates."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sqlite3
import subprocess
import tempfile
from ikos_runtime import runtime_command, runtime_identity, verify_runtime

ROOT = Path(__file__).resolve().parents[1]
OWNED = 'complete-finite-closed-bucket-graph-v1'
SHARED = 'complete-typed-immutable-node-graph-v1'
CLOSED = 'all-concurrent-payloads-share-sealed-normal-mutex'


def replace_once(text, old, new):
    assert text.count(old) == 1, old
    return text.replace(old, new, 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--bits', type=int, choices=(32, 64), default=32)
    args = parser.parse_args()
    entry = args.ikos.resolve()
    identity = runtime_identity(entry)
    command = runtime_command(entry)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith('IKOS_') and k not in {'PYTHONPATH', 'PYTHONHOME'}}
    owned = (ROOT / 'analyzer/test/regression/bmc-values/bucket-list-ownership.c').read_text()
    shared = (ROOT / 'analyzer/test/regression/bmc-values/shared-node-graph.c').read_text()
    with tempfile.TemporaryDirectory(prefix='ikos-bucket-abi-') as temporary:
        work = Path(temporary)

        def check(name, body, expected_proof, *, graph=False, safe=None,
                  closed=None, llvm=False, frontend_error=None, expected_packed=None):
            source = work / ('control.ll' if llvm else 'control.c')
            source.write_text(body)
            database = work / 'control.db'
            extra = []
            if expected_packed is not None:
                temps = work / (name + '-temps')
                temps.mkdir()
                extra = ['--save-temps', '--temp-dir=' + str(temps)]
            proc = subprocess.run([*command, *extra, '--analyses=race', '--concurrency=auto',
                '--partitioning=guards', '--demote-race-to-unknown',
                '--bmc-deadline-ms=0', '-m' + str(args.bits),
                '-o', str(database), str(source)], env=env, text=True,
                capture_output=True, timeout=45)
            output = proc.stdout + proc.stderr
            if frontend_error is not None:
                assert not expected_proof and proc.returncode == 5, (name, output)
                assert frontend_error in output and 'The program is SAFE' not in output
                print(f'bucket-abi={name}/bits={args.bits}: '
                      'ERROR unsupported source-to-AR cast (outside theorem)', flush=True)
                return
            assert proc.returncode == 0, (name, args.bits, output)
            with sqlite3.connect(database) as db:
                infos = [json.loads(row[0]) for row in db.execute('SELECT info FROM checks')]
                result = json.loads(db.execute(
                    "SELECT value FROM settings WHERE name='race_result'").fetchone()[0])
                if expected_packed is not None:
                    raw = db.execute("SELECT value FROM settings WHERE name='pp-bc-file'").fetchone()[0]
                    try:
                        raw = json.loads(raw)
                    except (ValueError, TypeError):
                        pass
                    pp = Path(raw).resolve(strict=True)
                    assert pp.is_relative_to(temps.resolve()), (name, pp)
                    before = hashlib.sha256(pp.read_bytes()).digest()
                    actual = work / 'actual-packed.ll'
                    subprocess.run([str(Path(compiler[0]).parent / 'opt'), '-S',
                        str(pp), '-o', str(actual)], env=env, check=True, capture_output=True)
                    assert hashlib.sha256(pp.read_bytes()).digest() == before
                    line = next(x for x in actual.read_text().splitlines()
                                if x.startswith('%struct.item = type '))
                    assert line.startswith('%struct.item = type <{') == expected_packed, (name, line)
            contract = SHARED if graph else OWNED
            proofs = [x for x in infos if x.get('bucket_list_contract') == contract]
            payload = [x for x in proofs if x.get('shared_payload_proof') == CLOSED]
            assert bool(proofs) == expected_proof, (name, args.bits, proofs, result, output)
            if safe is not None:
                assert (result['verdict'] == 'SAFE') == safe, (name, result, output)
            if closed is not None:
                assert bool(payload) == closed, (name, payload, output)
            print(f'bucket-abi={name}/bits={args.bits}: proofs={len(proofs)} '
                  f'closed={len(payload)} result={result["verdict"]}', flush=True)

        compiler = json.loads(subprocess.check_output(command[:-1] + ['-c',
            'import json; from ikos import settings, analyzer; '
            'print(json.dumps([settings.clang(), analyzer.clang_ikos_flags()]))'],
            env=env, text=True))

        def packed_check(name, body, *, graph=False):
            source = work / 'packed.c'
            native = work / 'packed.ll'
            source.write_text(body)
            subprocess.run([compiler[0], *compiler[1], '-S', '-emit-llvm',
                '-m' + str(args.bits), str(source), '-o', str(native)],
                env=env, check=True, capture_output=True)
            ir = native.read_text()
            line = next(x for x in ir.splitlines()
                        if x.startswith('%struct.item = type '))
            packed = line.startswith('%struct.item = type <{')
            # Clang omits packed when the natural field offsets already agree.
            # Check that representation, then separately force a real packed IR.
            check(name + '-C', body, not packed, graph=graph,
                  safe=True if not packed else None, expected_packed=packed)
            if not packed:
                assert line.startswith('%struct.item = type {') and line.endswith('}')
                ir = ir.replace(line, line.replace('type {', 'type <{') + '>', 1)
            assert '%struct.item = type <{' in ir
            check(name + '-LLVM', ir, False, graph=graph, llvm=True, expected_packed=True)

        for wide in (False, True):
            prelude = '#define WIDE\n' if wide else ''
            suffix = '-wide' if wide else '-int'
            check('owned-positive' + suffix, prelude + owned, True, safe=True)
            check('shared-positive' + suffix, prelude + shared, True,
                  graph=True, safe=True, closed=True)
            check('native-layout-projection' + suffix, '#define CASE 48\n' + prelude + owned,
                  True, safe=True)
            # Corruption touches bytes absent from an ILP32 pointer but covered
            # by a genuine LP64 pointer. Both ABIs still reject partial writes.
            body = replace_once(owned, '  pthread_t one, two;',
                '  pthread_t one, two;')
            body = replace_once(body, '#if CASE == 10\n',
                '  ((unsigned char *)&directory.entries[0].next)[sizeof(void *) - 1] = 1;\n#if CASE == 10\n')
            check('owned-link-upper-byte' + suffix, prelude + body, False)
            body = replace_once(shared, '#if CASE == 28\n',
                '  ((unsigned char *)&a->next)[sizeof(void *) - 1] = 1;\n#if CASE == 28\n')
            check('shared-link-upper-byte' + suffix, prelude + body, False, graph=True)
            body = replace_once(owned, '  pthread_create(&two, 0, visit, 0);',
                '  pthread_create(&two, 0, visit, 0);\n'
                '  ((unsigned char *)&one)[sizeof one - 1] = 1;')
            check('scalar-handle-upper-byte' + suffix, prelude + body, False)
            body = replace_once(owned, '  pthread_t one, two;', '  unsigned int one, two;')
            check('scalar-handle-width' + suffix, prelude + body, args.bits == 32,
                  safe=True if args.bits == 32 else None)
            body = replace_once(owned, '  fresh->value = value;',
                '  *(short *)&fresh->value = (short)value;')
            check('partial-payload-birth' + suffix, prelude + body, False)
            body = replace_once(owned, 'struct item { datum value; struct edge link; };',
                'struct item { datum value; struct edge link; } __attribute__((packed));')
            packed_check('packed-node' + suffix, prelude + body)
            body = replace_once(shared, 'struct item { datum value; struct edge link; };',
                'struct item { datum value; struct edge link; } __attribute__((packed));')
            packed_check('packed-shared-node' + suffix, prelude + body, graph=True)
            # Reaching the footprint proof is required for these broken locking
            # premises; failing an earlier ABI gate cannot make the test pass.
            for case in (1, 2, 17, 29, 30):
                check(f'shared-unclosed-{case}' + suffix,
                      f'#define CASE {case}\n' + prelude + shared, True,
                      graph=True, safe=False, closed=False)
        # Same-size anonymous mutex storage still needs a zero initializer and
        # closed actual API uses; a native layout alone grants no lock proof.
        bad_initial = replace_once(owned,
            'static pthread_mutex_t secondary = PTHREAD_MUTEX_INITIALIZER;',
            'static pthread_mutex_t secondary = { .__data = { .__lock = 1 } };')
        check('native-layout-nonzero-secondary', '#define CASE 48\n' + bad_initial, False)
        escaped = replace_once(owned, '  pthread_t one, two;',
            '  pthread_t one, two;\n'
            '  extern void observe_mutex(void *); observe_mutex(&secondary);')
        check('native-layout-escaped-secondary', '#define CASE 48\n' + escaped, False)
        check('native-long-payload', owned.replace('typedef int datum;', 'typedef long datum;'),
              True, safe=True)
        # A source-level high native offset is outside this bounded graph theorem.
        body = replace_once(owned, '    struct edge *edge = directory.entries[bucket].next;',
            '    struct edge *edge = directory.entries[bucket].next;\n'
            '    edge = (struct edge *)((char *)edge + ((size_t)1 << (sizeof(size_t) * 8 - 1)));')
        check('high-native-index', body, False)
        body = replace_once(owned, '    struct edge *edge = directory.entries[bucket].next;',
            '    struct edge *edge = directory.entries[bucket].next;\n'
            '    edge = (struct edge *)((char *)edge - 1);')
        check('negative-misaligned-index', body, False)
        body = replace_once(owned, '#if CASE != 28\n    pthread_mutex_init(&directory.locks[bucket], 0);',
            '#if CASE != 28\n    pthread_mutex_init(&directory.locks[bucket], (pthread_mutexattr_t *)1);')
        check('non-default-mutex-attributes', body, False)
        body = replace_once(owned, '  struct item *saved = 0;',
            '  struct item *saved = 0;\n')
        body = replace_once(body, '} directory;', '} directory __attribute__((address_space(1)));\n'
            '#define directory (*(struct directory *)(void *)&directory)')
        check('non-default-address-space', body, False,
              frontend_error='unsupported llvm cast: addrspacecast')

        source = work / 'native.c'
        source.write_text(owned)
        native = work / 'native.ll'
        subprocess.run([compiler[0], *compiler[1], '-S', '-emit-llvm',
            '-m' + str(args.bits), str(source), '-o', str(native)],
            env=env, check=True, capture_output=True)
        original = native.read_text()
        check('llvm-positive', original, True, llvm=True, safe=True)
        for callee in ('malloc', 'pthread_mutex_lock', 'pthread_create',
                       '__VERIFIER_nondet_int'):
            lines = original.splitlines()
            calls = declarations = 0
            for index, line in enumerate(lines):
                if '@' + callee + '(' not in line:
                    continue
                if ' call ' in line:
                    lines[index] = line.replace('call ', 'call fastcc ', 1)
                    calls += 1
                elif line.startswith('declare '):
                    lines[index] = line.replace('declare ', 'declare fastcc ', 1)
                    declarations += 1
            assert calls and declarations == 1, (callee, calls, declarations)
            check('non-C-' + callee, '\n'.join(lines) + '\n', False, llvm=True)
        for callee in ('malloc', 'pthread_mutex_lock', 'pthread_create'):
            pattern = r'(@' + callee + r'\([^ ,)]+)'
            changed, count = re.subn(pattern, r'\g<1> inreg', original)
            assert count >= 2 and changed != original, (callee, count)
            check('non-ordinary-inreg-' + callee, changed, False, llvm=True)
        mutex_line = next(line for line in original.splitlines()
                          if line.startswith('%union.pthread_mutex_t = type '))
        mutex_size = 24 if args.bits == 32 else 40
        check('wrong-mutex-size', original.replace(mutex_line,
            '%union.pthread_mutex_t = type { [' + str(mutex_size - 1) + ' x i8] }'),
            False, llvm=True)
        check('packed-mutex-leaf', original.replace(mutex_line,
            '%union.pthread_mutex_t = type <{ [' + str(mutex_size) + ' x i8] }>'),
            False, llvm=True)
        changed, count = re.subn(r'(alloca i' + str(args.bits) + r', align )\d+',
                                r'\g<1>1', original)
        assert count
        check('underaligned-native-handle', changed, False, llvm=True)
        lines = original.splitlines()
        count = 0
        for index, line in enumerate(lines):
            if ' call ' in line and '@pthread_create(' in line:
                needle = 'i' + str(args.bits) + '*'
                assert needle in line
                lines[index] = line.replace(needle, needle + ' byval(i' + str(args.bits) + ')', 1)
                count += 1
        assert count
        check('create-byval', '\n'.join(lines) + '\n', False, llvm=True)
        layout = next(x for x in original.splitlines() if x.startswith('target datalayout = '))
        fields = [x for x in layout.split('"')[1].split('-')
                  if not re.match(r'^p(?:0)?:', x)]
        fields.append('p:{0}:{0}:{0}:{1}'.format(args.bits, 16 if args.bits == 32 else 32))
        check('index-width-mismatch', original.replace(layout,
            'target datalayout = "' + '-'.join(fields) + '"'), False, llvm=True)
        # Preserve an actual legal IR signature while deliberately using the
        # wrong malloc size_t width; source_call_binding alone is insufficient.
        other = 64 if args.bits == 32 else 32
        changed = original
        pattern = r'(@malloc\()i' + str(args.bits) + r'([^\d][^)]*\))'
        def resize_malloc(match):
            operand = match.group(2)
            # The owned node-size actual is an immediate integer, so no SSA
            # operand needs a new cast. The declaration may carry attrs.
            return match.group(1) + 'i' + str(other) + operand
        changed, count = re.subn(pattern, resize_malloc, changed)
        assert count >= 2 and changed != original
        check('malloc-size-t-mismatch', changed, False, llvm=True)
    verify_runtime(identity)


if __name__ == '__main__':
    main()
