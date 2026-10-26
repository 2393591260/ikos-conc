#!/usr/bin/env python3
"""Require retained constant-global GEP arithmetic before checking certificates."""
import argparse
from contextlib import nullcontext
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
CONTRACT = 'closed-stable-flat-array-slices-v1'


def controls(bits):
    # Each term is (actual LLVM index width, signed index, allocation stride).
    # Python integers supply an independent unbounded mathematical oracle.
    quarter = 1 << (bits - 2)
    eighth = 1 << (bits - 3)
    return [
        ('baseline', True, []),
        ('zero', True, [(bits, 0, 4)]),
        ('small-cancel', True, [(bits, 1, 8), (bits, -2, 4)]),
        ('narrow-negative-cancel', True, [(bits, 1, 8), (8, -2, 4)]),
        ('huge-cancel', True, [(bits, eighth, 8), (bits, -quarter, 4)]),
        ('positive-product-wrap', False, [(bits, quarter, 4)]),
        ('negative-product-wrap', False, [(bits, -quarter, 4)]),
        ('positive-sum-wrap', False, [(bits, eighth - 1, 8), (bits, 2, 4)]),
        ('negative-sum-wrap', False, [(bits, 1 - eighth, 8), (bits, -2, 4)]),
        ('lossy-wide-index', False, [(128, 1 << bits, 4)]),
    ]


def arithmetic(terms, bits):
    modulus = 1 << bits
    exact = 0
    native = 0
    for width, index, stride in terms:
        assert -(1 << (width - 1)) <= index < (1 << (width - 1))
        exact += index * stride
        # LLVM sign-extends narrow indices and truncates wider indices to W.
        residue = index % modulus
        normalized = residue if residue < (modulus >> 1) else residue - modulus
        native = (native + normalized * stride) % modulus
    return {
        'terms': [{'index_bits': width, 'index': str(index), 'stride': stride}
                  for width, index, stride in terms],
        'exact_displacement': str(exact),
        'native_displacement': str(native),
        'index_widths_supported': all(width <= bits for width, _, _ in terms),
        'exact_final_offset_in_native_range': 0 <= exact < modulus,
    }


def probe_instructions(terms):
    if not terms:
        return []
    if len(terms) == 1:
        width, index, stride = terms[0]
        assert stride == 4
        lines = [f'%probe_address = getelementptr i32, i32* @constant_probe, i{width} {index}']
    else:
        assert len(terms) == 2 and [term[2] for term in terms] == [8, 4]
        first, second = terms
        lines = [
            '%probe_array = bitcast i32* @constant_probe to [2 x i32]*',
            '%probe_address = getelementptr [2 x i32], [2 x i32]* %probe_array, '
            f'i{first[0]} {first[1]}, i{second[0]} {second[1]}',
        ]
    # Non-inbounds GEPs all have native residue zero and read the actual scalar.
    # Using the load in a main guard keeps the probe semantically connected.
    return lines + ['%probe_value = load i32, i32* %probe_address, align 4']


def require_retained(text, instructions, name):
    def fail(reason):
        raise AssertionError(
            f'EVIDENCE_PRECONDITION_FAILED {name}: {reason}; '
            'do not classify this as a producer verdict regression')

    main = re.search(r'^define\s+[^\n]*@main\([^\n]*\)[^{]*\{\n(.*?)^}',
                     text, re.MULTILINE | re.DOTALL)
    if not main:
        fail('actual preprocessed main was not found')
    lines = [' '.join(line.split()) for line in main.group(1).splitlines()]
    if not re.search(r'^@constant_probe\s*=\s*constant i32 7, align 4$',
                     text, re.MULTILINE):
        fail('actual constant i32 global was removed or changed')
    positions = []
    for instruction in instructions:
        expected = ' '.join(instruction.split())
        if lines.count(expected) != 1:
            fail('required instruction was folded, removed, or changed: ' + expected)
        positions.append(lines.index(expected))
    if positions != sorted(positions):
        fail('probe instruction order changed')
    if instructions:
        guard = '%probe_ok = icmp eq i32 %probe_value, 7'
        if lines.count(guard) != 1 or lines.index(guard) <= positions[-1]:
            fail('actual load-to-main-guard dependency was not retained')
        nondet = [i for i, line in enumerate(lines)
                  if 'call i32 @__VERIFIER_nondet_int(' in line]
        if not nondet or positions[-1] >= min(nondet):
            fail('probe no longer executes in main before the input/creation phases')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--bits', type=int, choices=(32, 64), default=32)
    parser.add_argument('--evidence-dir', type=Path,
                        help='Retain sources, same-run preprocessed LLVM and result JSONs in a new directory.')
    args = parser.parse_args()
    entry = args.ikos.resolve()
    command = runtime_command(entry)
    identity = runtime_identity(entry)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith('IKOS_') and k not in {'PYTHONPATH', 'PYTHONHOME'}}
    optimizer = json.loads(subprocess.check_output(command[:-1] + [
        '-c', 'import json; from ikos import settings; print(json.dumps(settings.opt()))'],
        env=env, text=True))
    template = (ROOT / 'analyzer/test/regression/bmc-values/'
                'array-slices-constant-gep.ll.in').read_text()
    replacements = {
        '{{WORD}}': str(args.bits), '{{ALIGN}}': str(args.bits // 8),
        '{{I64ALIGN}}': '32' if args.bits == 32 else '64',
        '{{TRIPLE}}': 'i386-pc-linux-gnu' if args.bits == 32 else 'x86_64-pc-linux-gnu',
        '{{EXTEND_N}}': '' if args.bits == 32 else '%native_n = zext i32 %n to i64',
        '{{NATIVE_N}}': '%n' if args.bits == 32 else '%native_n',
        '{{EXTEND_I}}': '' if args.bits == 32 else '%native_i = sext i32 %i to i64',
        '{{NATIVE_I}}': '%i' if args.bits == 32 else '%native_i',
    }
    for token, value in replacements.items():
        template = template.replace(token, value)
    if args.evidence_dir:
        args.evidence_dir = args.evidence_dir.resolve()
        args.evidence_dir.mkdir(parents=True, exist_ok=False)
    context = (nullcontext(str(args.evidence_dir)) if args.evidence_dir
               else tempfile.TemporaryDirectory(prefix='ikos-constant-gep-'))
    digest = lambda data: hashlib.sha256(data).hexdigest()
    with context as temporary:
        work = Path(temporary)
        for name, accepted, terms in controls(args.bits):
            run = work / name
            run.mkdir()
            oracle = arithmetic(terms, args.bits)
            assert int(oracle['native_displacement']) == 0, (name, oracle)
            assert accepted == (oracle['index_widths_supported']
                                and int(oracle['exact_displacement']) == 0), (name, oracle)
            instructions = probe_instructions(terms)
            body = template.replace('{{PROBE_INSTRUCTIONS}}', '\n  '.join(instructions))
            body = body.replace('{{PROBE_VALUE}}', '%probe_value' if terms else '7')
            assert '{{' not in body
            source = run / 'control.ll'
            source.write_text(body)
            database = run / 'output.db'
            temporary_ir = run / 'temps'
            temporary_ir.mkdir()
            argv = [*command, '--analyses=race', '--concurrency=auto',
                    '--partitioning=guards', '--demote-race-to-unknown',
                    '--bmc-deadline-ms=0', '--opt=none', '--save-temps',
                    '--temp-dir=' + str(temporary_ir), '-m' + str(args.bits),
                    '-o', str(database), str(source)]
            proc = subprocess.run(argv, env=env, capture_output=True, text=True, timeout=30)
            output = proc.stdout + proc.stderr
            (run / 'analysis.log').write_text(output)
            assert proc.returncode == 0, ('ANALYSIS_FAILED_BEFORE_EVIDENCE', name, output)
            with sqlite3.connect(database) as db:
                settings = dict(db.execute('SELECT name, value FROM settings'))
                infos = [json.loads(row[0]) for row in db.execute('SELECT info FROM checks')]
            def setting(key):
                value = settings[key]
                try:
                    return json.loads(value)
                except (ValueError, TypeError):
                    return value
            assert setting('opt-level') == 'none', (
                'EVIDENCE_PRECONDITION_FAILED', name, 'unexpected preprocessing mode')
            pp = Path(setting('pp-bc-file')).resolve(strict=True)
            assert pp.is_relative_to(temporary_ir.resolve()), (
                'EVIDENCE_PRECONDITION_FAILED', name, 'preprocessed file is outside this run', pp)
            before = digest(pp.read_bytes())
            disassembly = run / 'actual-preprocessed.ll'
            subprocess.run([optimizer, '-S', str(pp), '-o', str(disassembly)],
                           env=env, capture_output=True, text=True, check=True, timeout=30)
            assert digest(pp.read_bytes()) == before, (
                'EVIDENCE_PRECONDITION_FAILED', name, 'preprocessed input changed')
            text = disassembly.read_text()
            # This prerequisite precedes every verdict/proof assertion.
            require_retained(text, instructions, name)
            result = setting('race_result')
            proofs = [info for info in infos if info.get('array_slices_contract') == CONTRACT]
            record = {
                'name': name, 'bits': args.bits, 'opt': setting('opt-level'),
                'arithmetic_oracle': oracle, 'retention_precondition': 'PASS',
                'source_sha256': digest(source.read_bytes()), 'pp_sha256': before,
                'disassembly_sha256': digest(disassembly.read_bytes()),
                'verdict': result['verdict'], 'proof_count': len(proofs),
                'expected': 'SAFE' if accepted else 'UNKNOWN',
                'runtime_identity': identity,
            }
            (run / 'evidence.json').write_text(json.dumps(record, indent=2) + '\n')
            assert result['verdict'] == record['expected'], (name, args.bits, record, output)
            assert bool(proofs) == accepted, (name, args.bits, proofs, output)
            print(f'constant-gep {name}/bits={args.bits}: {record["expected"]}; '
                  f'proofs={len(proofs)}; exact={oracle["exact_displacement"]}; '
                  f'native={oracle["native_displacement"]}; retention=PASS', flush=True)
    verify_runtime(identity)
    print(f'PASS bits={args.bits} {len(controls(args.bits))} retained constant-GEP controls',
          flush=True)


if __name__ == '__main__':
    main()
