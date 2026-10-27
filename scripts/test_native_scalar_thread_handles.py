#!/usr/bin/env python3
"""Exercise scalar native storage without assuming numerical IDs or completion.

Defaults to both ABIs; --bits selects a focused run.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import sqlite3
import subprocess
import tempfile
from ikos_runtime import runtime_command, runtime_identity, verify_runtime

ROOT = Path(__file__).resolve().parents[1]
INVENTORY = 'Native pthread handles: closed scalar storage inventory'
PREFLIGHT = 'Native pthread handles: complete output-effect preflight'


def source_cases(fixture, bits):
    def edit(**parts):
        text = fixture
        for key, value in parts.items():
            text = text.replace('/* ' + key.upper() + ' */', value)
        return text
    cases = [
        ('protected', fixture, True, True, True),
        ('unprotected', '#define UNPROTECTED 1\n' + fixture, True, True, False),
        ('wide-datum', '#define WIDE\n' + fixture, True, True, True),
        ('numeric-observer', edit(between='if (first == 1) data = 3;'), False, False, False),
        ('copy', edit(between='volatile pthread_t old = first;').replace('pthread_join(first,', 'pthread_join(old,'), False, False, None),
        ('escape', edit(prefix='escaped = &first;'), False, False, None),
        ('worker-argument', fixture.replace('&first, NULL, worker, NULL', '&first, NULL, worker, &first'), False, False, None),
        ('byte-copy', edit(between='memcpy(&second, &first, sizeof first);'), False, False, None),
        ('partial-write', edit(between='((unsigned char *)&first)[0] = 1;'), False, False, None),
        ('volatile', fixture.replace('pthread_t first, second;', 'volatile pthread_t first, second;'), False, False, None),
        ('unknown-slot-write', edit(between='unknown(&first);'), False, False, None),
        ('used-create-result', fixture.replace('pthread_create(&first, NULL, worker, NULL);', 'if (pthread_create(&first, NULL, worker, NULL)) data = 9;'), False, False, None),
        ('join-output', fixture.replace('pthread_join(first, NULL);', 'pthread_join(first, &escaped);'), False, False, None),
        ('literal-join', fixture.replace('pthread_join(first, NULL);', 'pthread_join(1, NULL);'), False, False, None),
        ('repeat-scalar', fixture.replace('pthread_create(&first, NULL, worker, NULL);', 'for (int k = 0; k < 2; ++k) pthread_create(&first, NULL, worker, NULL);'), False, False, None),
        ('overwrite-scalar', edit(between='pthread_create(&first, NULL, worker, NULL);'), False, False, None),
        ('mixed', edit(prefix='pthread_t pool[2];\n for (int k = 0; k < 2; ++k) pthread_create(&pool[k], NULL, worker, NULL);', suffix='for (int k = 0; k < 2; ++k) pthread_join(pool[k], NULL);'), True, True, True),
        ('mixed-range-fails', edit(prefix='pthread_t pool[2]; for (int k = 0; k < 3; ++k) pthread_create(&pool[k], NULL, worker, NULL);', suffix='for (int k = 0; k < 2; ++k) pthread_join(pool[k], NULL);'), True, False, False),
        ('mixed-unaccounted', edit(prefix='pthread_t pool[2]; pthread_create(&pool[0], NULL, worker, NULL);', suffix='pthread_join(pool[0], NULL);'), False, False, None),
        ('duplicate-join-no-hb', '#define UNPROTECTED 1\n' + fixture.replace('pthread_join(second, NULL);', 'pthread_join(first, NULL);'), True, True, False),
        ('missing-join-no-hb', '#define UNPROTECTED 1\n' + fixture.replace('pthread_join(second, NULL);', ''), True, True, False),
        ('join-main-no-hb', '#define UNPROTECTED 1\n' + edit(suffix='data++;').replace('pthread_create(&second, NULL, worker, NULL);', '').replace('pthread_join(second, NULL);', ''), True, True, False),
        ('unknown-global-effect', edit(prefix='unknown(&data);'), True, False, False),
    ]
    wrong = 'unsigned long long' if bits == 32 else 'unsigned int'
    cases.append(('same-name-definition', fixture + '\nint pthread_create(pthread_t *out, const pthread_attr_t *attr, void *(*entry)(void *), void *arg) { (void)out; (void)attr; (void)entry; (void)arg; return 0; }\n', False, False, None))
    cases.append(('wrong-storage-width', fixture.replace('pthread_t first, second;', wrong + ' first, second;').replace('&first, NULL, worker', '(pthread_t *)&first, NULL, worker').replace('&second, NULL, worker', '(pthread_t *)&second, NULL, worker'), False, False, None))
    markers = edit(extra_declarations='extern void __VERIFIER_atomic_begin(void);\nextern void __VERIFIER_atomic_end(void);\nextern _Bool __VERIFIER_nondet_bool(void);', prefix='if (__VERIFIER_nondet_bool()) { __VERIFIER_atomic_begin(); __VERIFIER_atomic_end(); }')
    cases.append(('exact-range-call-closure', markers, True, True, True))
    cases.append(('bad-marker-closure', markers.replace('atomic_begin(void)', 'atomic_begin(int)').replace('atomic_begin();', 'atomic_begin(1);'), True, False, None))
    cases.append(('bad-bool-closure', markers.replace('nondet_bool(void)', 'nondet_bool(int)').replace('nondet_bool())', 'nondet_bool(1))'), True, False, None))
    return cases


def seed_ir(bits):
    triple = 'i386-pc-linux-gnu' if bits == 32 else 'x86_64-pc-linux-gnu'
    layout = 'e-m:e-p:32:32-i64:64-f80:32-n8:16:32-S128' if bits == 32 else 'e-m:e-i64:64-f80:128-n8:16:32:64-S128'
    word = bits // 8
    source = f"""target datalayout = "{layout}"
target triple = "{triple}"
declare i32 @pthread_create(i{bits}*, i8*, i8* (i8*)*, i8*)
declare i32 @pthread_join(i{bits}, i8**)
declare i{bits} @__ikos_nondet_int{'64' if bits == 64 else ''}()
define i8* @worker(i8* %arg) {{ entry: ret i8* null }}
define i32 @main() {{
entry:
  %first = alloca i{bits}, align {word}
  %seed = call i{bits} @__ikos_nondet_int{'64' if bits == 64 else ''}(), !ikos.freeze_uninit !0
  store i{bits} %seed, i{bits}* %first, align {word}, !ikos.freeze_uninit !0
  %created = call i32 @pthread_create(i{bits}* %first, i8* null, i8* (i8*)* @worker, i8* null)
  %handle = load i{bits}, i{bits}* %first, align {word}
  %joined = call i32 @pthread_join(i{bits} %handle, i8** null)
  ret i32 0
}}
!0 = !{{}}
"""
    return [
        ('seed-exact', source, True),
        ('seed-unmarked-call', source.replace('(), !ikos.freeze_uninit !0', '()'), False),
        ('seed-unmarked-store', source.replace(f'align {word}, !ikos.freeze_uninit !0', f'align {word}'), False),
        ('seed-observed', source.replace('declare i32 @pthread_create', '@observed = global i32 0\ndeclare i32 @pthread_create').replace('  %created =', f'  %observed = icmp eq i{bits} %seed, 0\n  %observed32 = zext i1 %observed to i32\n  store i32 %observed32, i32* @observed, align 4\n  %created ='), False),
        ('seed-unaligned', source.replace(f'align {word}', 'align 1'), False),
        ('seed-wrong-callee', source.replace('@__ikos_nondet_int', '@other_nondet_int'), False),
        ('seed-after-create', source.replace(f'  store i{bits} %seed, i{bits}* %first, align {word}, !ikos.freeze_uninit !0\n', '').replace('  %handle =', f'  store i{bits} %seed, i{bits}* %first, align {word}, !ikos.freeze_uninit !0\n  %handle ='), False),
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--bits', choices=(32, 64), type=int)
    parser.add_argument('--report', type=Path)
    parser.add_argument('--benchmarks', type=Path, help='optional SV-Benchmarks c directory for actual boundary diagnostics')
    args = parser.parse_args()
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/native-scalar-handles.c').read_text()
    env = {k: v for k, v in os.environ.items() if not k.startswith('IKOS_')}
    # Test the opaque value producer independently of completion consumers.
    env['IKOS_RACE_DISABLE_SCALAR_COMPLETION'] = '1'
    before = runtime_identity(args.ikos.resolve())
    rows = []
    with tempfile.TemporaryDirectory(prefix='ikos-native-scalars-') as temp:
        work = Path(temp)
        def run(name, program, bits, native, preflight, safe, suffix='.c'):
            source = work / (name + suffix)
            source.write_text(program)
            database = work / (name + '.db')
            command = runtime_command(args.ikos.resolve()) + ['--analyses=race', '--concurrency=auto',
                '--partitioning=guards', '--demote-race-to-unknown', '--bmc-deadline-ms=0',
                '--log=debug', '-m' + str(bits), '-o', str(database)]
            if suffix == '.ll': command += ['--opt=none']
            proc = subprocess.Popen(command + [str(source)], env=env,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    text=True, start_new_session=True)
            try:
                stdout, stderr = proc.communicate(timeout=60)
            except BaseException:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.communicate()
                raise
            output = stdout + stderr
            assert proc.returncode == 0, (name, bits, output)
            assert (INVENTORY in output) == native, (name, bits, output)
            if preflight is not None: assert (PREFLIGHT in output) == preflight, (name, bits, output)
            if safe is not None: assert ('The program is SAFE' in output) == safe, (name, bits, output)
            with sqlite3.connect(database) as db:
                result = json.loads(db.execute("SELECT value FROM settings WHERE name='race_result'").fetchone()[0])
                reasons = [row[0] for row in db.execute("SELECT info FROM checks WHERE info LIKE '%unknown_reason%'")]
            if native:
                assert not any('synthetic pthread handle representation not modeled' in reason for reason in reasons), (name, reasons)
            if safe is True: assert result['coverage'] == 'Checked' and result['remaining_pairs'] == 0, result
            if name in {'unprotected', 'duplicate-join-no-hb', 'missing-join-no-hb', 'join-main-no-hb'}:
                assert result['coverage'] == 'Checked' and result['remaining_pairs'] > 0, result
            rows.append(dict(name=name, bits=bits, inventory=native, preflight=PREFLIGHT in output,
                             result=result, reasons=reasons))
            print(f'native scalar {name}/{bits}: {result["verdict"]}, coverage={result["coverage"]}', flush=True)
        for bits in ([args.bits] if args.bits else [32, 64]):
            for name, program, native, preflight, safe in source_cases(fixture, bits):
                run(name, program, bits, native, preflight, safe)
            for name, program, native in seed_ir(bits):
                run(name, program, bits, native, None, None, '.ll')
        if args.benchmarks:
            originals = [
                ('832', 'weaver/popl20-bad-dot-product-alt.wvr.c', True),
                ('881', 'weaver/popl20-prod-cons-eq.wvr.c', True),
                ('793', 'weaver/loop-tiling-eq.wvr.c', True),
                ('61', 'pthread/twostage_3.i', False),
            ]
            for name, relative, native in originals:
                original = args.benchmarks / relative
                # Record coverage, pairs and remaining reasons. These actual
                # originals have no predeclared recovered verdict in this runner.
                run(name, original.read_text(), 32, native, None, None)
    verify_runtime(before)
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(rows, indent=2) + '\n')


if __name__ == '__main__':
    main()
