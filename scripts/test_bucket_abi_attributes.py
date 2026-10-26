#!/usr/bin/env python3
"""Bucket value-ABI controls with same-run lowered-form prerequisites."""
import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import re
import sqlite3
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CONTRACT = 'complete-finite-closed-bucket-graph-v1'
ABI_REASON = 'source call ABI attributes not covered'
NARROW = (
    ('signed-char', 'signed char', 8, 'signext', -3),
    ('unsigned-char', 'unsigned char', 8, 'zeroext', 128),
    ('signed-short', 'short', 16, 'signext', -3),
    ('unsigned-short', 'unsigned short', 16, 'zeroext', 32768),
)
SSA = r'%[-a-zA-Z$._0-9]+'


def prerequisite(condition, name, detail):
    if not condition:
        raise AssertionError(
            f'EVIDENCE_PRECONDITION_FAILED {name}: {detail}; '
            'do not classify this as a Bucket verdict regression')


def replace_once(text, old, new):
    assert text.count(old) == 1, old
    return text.replace(old, new, 1)


def narrow_source(owned, ctype, adjustment):
    helper = ('static __attribute__((noinline)) ' + ctype +
              ' abi_echo(' + ctype + ' value) { return value; }\n')
    body = replace_once(owned, 'static struct item *construct(int value) {',
                        helper + 'static struct item *construct(int value) {')
    return replace_once(body, '  fresh->value = value;',
                        '  fresh->value = abi_echo((' + ctype +
                        ')(value + (' + str(adjustment) + ')));')


def abi_attributes(text):
    # Ignore non-ABI facts such as noundef; require every mutated ABI token.
    return sorted(re.findall(r'\b(?:signext|zeroext|inreg)\b|\bbyval\([^()]+\)', text))


def signature(line, symbol):
    span = parameter_span(line, symbol)
    assert span is not None, (symbol, line)
    start, end = span
    prefix = line[:line.index('@' + symbol + '(')]
    ret = re.search(r'(void|i\d+\**|%[-a-zA-Z$._0-9]+\*+|ptr)\s*$', prefix)
    assert ret, (symbol, line)
    parameters = []
    for value in split_parameters(line[start:end]):
        value = value.strip()
        if not value:
            continue
        match = re.match(r'(i\d+\**|%[-a-zA-Z$._0-9]+\*+|ptr)(?=\s|$)', value)
        assert match, (symbol, value)
        parameters.append((match.group(1), abi_attributes(value[match.end():])))
    return (ret.group(1), abi_attributes(prefix[:ret.start()]), parameters)


def symbol_sites(ir, symbol):
    sites = {'call': [], 'declaration': []}
    for line in ir.splitlines():
        role = line_role(line)
        if role and parameter_span(line, symbol) is not None:
            sites[role].append(signature(line, symbol))
    return sites


def require_sites(actual, original, symbol, name):
    expected = symbol_sites(original, symbol)
    found = symbol_sites(actual, symbol)
    prerequisite(len(expected['declaration']) == 1, name,
                 'input does not have exactly one declaration/definition: ' + symbol)
    prerequisite(found == expected, name,
                 'actual call/entry types or ABI attributes changed: ' + symbol +
                 '; expected=' + repr(expected) + '; actual=' + repr(found))
    return found


def function_bodies(ir):
    return {match.group(1): match.group(2) for match in re.finditer(
        r'^define[^\n]*@([-a-zA-Z$._0-9]+)\([^\n]*\)[^\n]*\{\n(.*?)^}',
        ir, re.MULTILINE | re.DOTALL)}


def require_reachability(ir, name):
    bodies = function_bodies(ir)
    prerequisite('main' in bodies and 'visit' in bodies, name,
                 'actual main/worker definition missing')
    reached, pending = set(), ['main']
    while pending:
        current = pending.pop()
        if current in reached:
            continue
        reached.add(current)
        pending.extend(callee for callee in re.findall(
            r'\bcall\b[^\n]*?@([-a-zA-Z$._0-9]+)\(', bodies.get(current, ''))
                       if callee in bodies and callee not in reached)
    prerequisite('construct' in reached and 'reset' in reached, name,
                 'actual main no longer calls the construction/reset path')
    create_lines = [line for fn in reached for line in bodies.get(fn, '').splitlines()
                    if re.search(r'\bcall\b[^\n]*@pthread_create\(', line)]
    prerequisite(len(create_lines) == 2 and all(
        re.search(r'@visit(?=[,\s])', line) for line in create_lines), name,
        'actual main-reachable creates do not both use visit')
    return bodies, reached


def require_payload_dependency(ir, width, extension, name):
    bodies, reached = require_reachability(ir, name)
    prerequisite('abi_echo' in reached, name, 'narrow helper is not main-reachable')
    helper_header = next(line for line in ir.splitlines()
                         if line.startswith('define ') and '@abi_echo(' in line)
    start, end = parameter_span(helper_header, 'abi_echo')
    formal = re.search(SSA + r'\s*$', helper_header[start:end])
    prerequisite(formal is not None and re.search(
        r'\bret i' + str(width) + r' ' + re.escape(formal.group(0).strip()) +
        r'(?=[,\s]|$)', bodies['abi_echo']) is not None, name,
        'actual helper no longer directly returns its narrow formal')
    count = 0
    for owner in reached:
        lines = bodies.get(owner, '').splitlines()
        slots = set()
        for line in lines:
            match = re.search(r'(' + SSA + r') = getelementptr(?: inbounds)? '
                              r'%struct\.item, %struct\.item\* ' + SSA +
                              r', i(?:32|64) 0, i32 0(?=[,\s]|$)', line)
            if match:
                slots.add(match.group(1))
        for line in lines:
            if not re.search(r'\bcall\b[^\n]*@abi_echo\(', line):
                continue
            call = re.match(r'\s*(' + SSA + r')\s*=', line)
            prerequisite(call is not None, name, 'narrow result has no SSA identity')
            cast_opcode = 'sext' if extension == 'signext' else 'zext'
            widened = [match.group(1) for candidate in lines
                       if (match := re.search(
                           r'(' + SSA + r') = ' + cast_opcode + r' i' + str(width) +
                           ' ' + re.escape(call.group(1)) + r' to i32(?=[,\s]|$)',
                           candidate))]
            prerequisite(len(widened) == 1, name,
                         'actual helper result no longer has its signed/unsigned extension')
            stores = [candidate for candidate in lines if any(re.search(
                r'\bstore i32 ' + re.escape(widened[0]) + r', i32\* ' +
                re.escape(slot) + r'(?=[,\s]|$)', candidate) for slot in slots)]
            prerequisite(len(stores) == 1, name,
                         'actual helper result no longer reaches the full payload birth store')
            count += 1
    prerequisite(count > 0, name, 'no retained helper-to-payload dependency')
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    parser.add_argument('--ikos', type=Path)
    parser.add_argument('--bits', type=int, choices=(32, 64), default=32)
    parser.add_argument('--evidence-dir', type=Path,
                        help='Retain source, actual preprocessed LLVM, logs and JSON in a new directory.')
    args = parser.parse_args()
    root = args.source_root.resolve()
    sys.path.insert(0, str(root / 'scripts'))
    # Reuse the accepted parser-aware LLVM attribute mutations; do not maintain
    # a second regex-only argument-list mutator in this runner.
    global parameter_span, split_parameters, line_role
    from test_source_call_abi import (parameter_span, split_parameters, line_role,
                                      mutate_parameter, mutate_return)
    from ikos_runtime import runtime_command, runtime_identity, verify_runtime
    entry = (args.ikos or root / 'install/bin/ikos').resolve()
    identity, command = runtime_identity(entry), runtime_command(entry)
    env = {key: value for key, value in os.environ.items()
           if not key.startswith('IKOS_') and key not in {'PYTHONPATH', 'PYTHONHOME'}}
    compiler, flags, optimizer = json.loads(subprocess.check_output(command[:-1] + [
        '-c', 'import json; from ikos import settings, analyzer; '
        'print(json.dumps([settings.clang(), analyzer.clang_ikos_flags(), settings.opt()]))'],
        env=env, text=True, timeout=30))
    fixture = root / 'analyzer/test/regression/bmc-values/bucket-list-ownership.c'
    owned = fixture.read_text()
    digest = lambda data: hashlib.sha256(data).hexdigest()
    fixture_hash = digest(fixture.read_bytes())
    if args.evidence_dir:
        args.evidence_dir = args.evidence_dir.resolve()
        args.evidence_dir.mkdir(parents=True, exist_ok=False)
    context = (nullcontext(str(args.evidence_dir)) if args.evidence_dir
               else tempfile.TemporaryDirectory(prefix='ikos-bucket-value-abi-'))
    total = 0
    with context as temporary:
        work = Path(temporary)
        compiled = work / 'compiled'
        compiled.mkdir()

        def compile_ir(name, body):
            source, ir = compiled / (name + '.c'), compiled / (name + '.ll')
            source.write_text(body)
            proc = subprocess.run([compiler, *flags, '-S', '-emit-llvm',
                                   '-m' + str(args.bits), str(source), '-o', str(ir)],
                                  env=env, capture_output=True, text=True, timeout=30)
            (compiled / (name + '.log')).write_text(proc.stdout + proc.stderr)
            assert proc.returncode == 0, ('COMPILATION_FAILED_BEFORE_EVIDENCE', name,
                                         proc.stdout, proc.stderr)
            return ir.read_text(), digest(source.read_bytes())

        def control(name, ir, accepted, c_hash, *, symbols=(), narrow=None):
            nonlocal total
            run = work / name
            run.mkdir()
            source, database = run / 'control.ll', run / 'output.db'
            source.write_text(ir)
            temps = run / 'temps'
            temps.mkdir()
            argv = [*command, '--analyses=race', '--concurrency=auto',
                    '--partitioning=guards', '--demote-race-to-unknown',
                    '--bmc-deadline-ms=0', '--opt=basic', '--save-temps',
                    '--temp-dir=' + str(temps), '-m' + str(args.bits),
                    '-o', str(database), str(source)]
            proc = subprocess.run(argv, env=env, capture_output=True, text=True, timeout=45)
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
            prerequisite(setting('opt-level') == 'basic', name, 'unexpected preprocessing mode')
            pp = Path(setting('pp-bc-file')).resolve(strict=True)
            prerequisite(pp.is_relative_to(temps.resolve()), name, 'pp input is outside this run')
            before = digest(pp.read_bytes())
            disassembly = run / 'actual-preprocessed.ll'
            subprocess.run([optimizer, '-S', str(pp), '-o', str(disassembly)],
                           env=env, capture_output=True, text=True, check=True, timeout=30)
            prerequisite(digest(pp.read_bytes()) == before, name, 'pp input changed')
            actual = disassembly.read_text()
            # Every actual-form prerequisite precedes verdict/certificate checks.
            signatures = {symbol: require_sites(actual, ir, symbol, name)
                          for symbol in sorted(set(('main', 'visit', *symbols)))}
            require_reachability(actual, name)
            dependencies = require_payload_dependency(actual, *narrow, name) if narrow else None
            result = setting('race_result')
            proofs = [info for info in infos if info.get('bucket_list_contract') == CONTRACT]
            debts = [item for item in result['obligations']
                     if item.get('source') == 'source_abi' and
                     item.get('reason') == ABI_REASON]
            expected = 'SAFE' if accepted else 'UNKNOWN'
            record = {
                'name': name, 'bits': args.bits, 'opt': setting('opt-level'),
                'fixture_sha256': fixture_hash, 'c_source_sha256': c_hash,
                'llvm_input_sha256': digest(source.read_bytes()), 'pp_sha256': before,
                'actual_llvm_sha256': digest(disassembly.read_bytes()),
                'actual_form_precondition': 'PASS', 'actual_abi_sites': signatures,
                'retained_payload_dependencies': dependencies, 'expected': expected,
                'verdict': result['verdict'], 'proof_count': len(proofs),
                'coverage': result['coverage'], 'source_abi_debts': debts,
                'runtime_identity': identity,
            }
            (run / 'evidence.json').write_text(json.dumps(record, indent=2) + '\n')
            assert result['verdict'] == expected, (name, args.bits, record, output)
            assert bool(proofs) == accepted, (name, args.bits, record, proofs, output)
            if accepted:
                assert not debts, (name, record, output)
            else:
                assert debts and result['coverage'] == 'Blocked' and result['unresolved_count'] > 0, (
                    name, record, output)
                assert not result['search_performed'], (name, result)
                assert all(not item.get('contract') and not item.get('safety_only')
                           for item in debts), (name, debts)
            total += 1
            print(f'bucket-value-abi={name}/bits={args.bits}: {expected}; '
                  f'proofs={len(proofs)}; actual-form=PASS', flush=True)

        baseline, base_hash = compile_ir('native', owned)
        control('native', baseline, True, base_hash)
        positive_ir = {}
        for label, ctype, width, extension, adjustment in NARROW:
            ir, c_hash = compile_ir(label, narrow_source(owned, ctype, adjustment))
            # Assert what the actual compiler produced before deriving mutations.
            sites = symbol_sites(ir, 'abi_echo')
            expected_site = ('i' + str(width), [extension], [('i' + str(width), [extension])])
            prerequisite(sites['declaration'] == [expected_site] and sites['call'] and
                         all(site == expected_site for site in sites['call']), label,
                         'compiler did not emit the expected narrow ABI')
            positive_ir[label] = (ir, c_hash, width, extension)
            control(label, ir, True, c_hash, symbols=('abi_echo',), narrow=(width, extension))
            other = 'zeroext' if extension == 'signext' else 'signext'
            for side, suffix in (({'call'}, 'call'), ({'declaration'}, 'definition')):
                for position, changed in (
                    ('parameter', mutate_parameter(ir, 'abi_echo', 0, other, side)),
                    ('return', mutate_return(ir, 'abi_echo', other, side))):
                    control(label + '-' + suffix + '-' + position + '-mismatch',
                            changed, False, c_hash, symbols=('abi_echo',),
                            narrow=(width, extension))
        ir, c_hash, width, extension = positive_ir['signed-char']
        both = {'call', 'declaration'}
        control('helper-parameter-inreg',
                mutate_parameter(ir, 'abi_echo', 0, 'inreg', both), False, c_hash,
                symbols=('abi_echo',), narrow=(width, extension))
        control('helper-pointer-byval',
                mutate_parameter(baseline, 'reset', 0, 'byval(%struct.edge)', both),
                False, base_hash, symbols=('reset',))
        # main has no ordinary callsite, but the shared checker also inspects
        # entry attributes: this is an end-to-end entry refusal control.
        control('main-return-inreg',
                mutate_return(baseline, 'main', 'inreg', {'declaration'}),
                False, base_hash)
        # These are end-to-end callback controls. The shared native-handle API
        # also checks worker ABI, so do not attribute rejection to execute alone.
        for attribute, suffix in (('inreg', 'inreg'), ('byval(i8)', 'byval')):
            control('worker-parameter-' + suffix,
                    mutate_parameter(baseline, 'visit', 0, attribute, {'declaration'}),
                    False, base_hash)
    verify_runtime(identity)
    assert total == 26, total
    print(f'PASS bits={args.bits} {total} Bucket value-ABI controls', flush=True)


if __name__ == '__main__':
    main()
