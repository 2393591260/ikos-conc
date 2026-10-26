#!/usr/bin/env python3
"""Check the exact POSIX ffs value contract and reject forged results."""
import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from ikos_runtime import runtime_command

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from svcomp_race_test import classify, VERDICT_RACE
from validate_bmc_execution import Replay, Reject, check, digest_json
from analyzer.python.ikos.race_result import validate_race_result


def api_contract():
    replay = Replay.__new__(Replay)
    replay.atomic_owner = None
    integer = dict(kind='integer', bits=32, signed=True, size='4', extent='4')
    replay.functions = {'ffs': dict(declaration=True, variadic=False,
                                    parameter_types=[integer], return_type=integer)}
    inst = dict(calling_convention=0, operands=[{}, dict(type=integer)], result=dict(type=integer))
    invoke = lambda value: replay.api('ffs', [value], inst, {}, 'a', 0, None, None)
    cases = [(0, 0), (-1, 1), (-2, 2), (0x7fffffff, 1)]
    cases += [((1 << bit) if bit != 31 else -(1 << 31), bit + 1) for bit in range(32)]
    for value, expected in cases:
        assert invoke(value) == expected, (value, expected)
    for value in (1 << 31, -(1 << 31) - 1):
        try:
            invoke(value)
        except Reject:
            pass
        else:
            raise AssertionError('accepted out-of-range ffs input')
    for field, invalid in (('declaration', False), ('variadic', True), ('parameter_types', []),
                           ('parameter_types', [dict(integer, bits=64, size='8', extent='8')]),
                           ('return_type', dict(integer, signed=False))):
        original = replay.functions['ffs'][field]
        replay.functions['ffs'][field] = invalid
        try:
            invoke(2)
        except Reject:
            pass
        else:
            raise AssertionError('accepted malformed ffs contract: ' + field)
        finally:
            replay.functions['ffs'][field] = original


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--bits', type=int, choices=(32, 64), default=32)
    args = parser.parse_args()
    api_contract()
    fixture = (ROOT / 'analyzer/test/regression/bmc-values/ffs.c').read_text()
    cases = [(value, (value & 0xffffffff & -(value & 0xffffffff)).bit_length(), [])
             for value in (0, 1, 2, 3, 4, 1 << 30, -(1 << 31), -1, -2, 0x7fffffff)]
    cases += [(2, 2, ['SHARED']), (-2, 2, ['INDIRECT']), (2, 0, ['DEFINED'])]
    cases += [(2, 2, ['LOCKED'])]
    cases += [(value, answer, options + ['WIDE']) for value, answer, options in cases[:]]
    with tempfile.TemporaryDirectory(prefix='ikos-ffs-') as temp:
        work = Path(temp)
        for value, answer, options in cases:
            for valid in (True, False):
                expected = answer if valid else (2 if 'DEFINED' in options else answer + 1)
                source = work / 'renamed.c'
                source.write_text(f'#define INPUT ({value})\n#define EXPECTED {expected}\n' +
                                  ''.join('#define ' + flag + '\n' for flag in options) +
                                  fixture.replace('datum', 'payload').replace('first', 'entry'))
                report = work / 'report.json'
                report.unlink(missing_ok=True)
                result = subprocess.run(runtime_command(args.ikos.resolve()) + ['--analyses=race',
                    '--concurrency=auto', '--demote-race-to-unknown', f'-m{args.bits}',
                    '--format=json', '--report-file=' + str(report),
                    '-o', str(work / 'output.db'), str(source)],
                    env=dict(os.environ, IKOS_BMC_RECOVER='1'), capture_output=True,
                    text=True, timeout=30)
                verdict = classify(result.stdout, result.returncode, False)
                data = json.loads(report.read_text())
                metadata = validate_race_result(data)
                # The frontend resolves INDIRECT's constant local pointer to a direct call.
                assert metadata['coverage'] == 'Checked' and not metadata['obligations'], metadata
                confirmed = valid and 'LOCKED' not in options
                assert result.returncode == 0 and (verdict == VERDICT_RACE) == confirmed, (
                    value, options, valid, verdict, result.stdout, result.stderr)
                if 'LOCKED' in options:
                    assert metadata['verdict'] == 'SAFE', metadata
                elif not valid:
                    assert metadata['verdict'] == ('SAFE' if 'DEFINED' in options else 'UNKNOWN'), metadata
                if confirmed:
                    executions = [r['info']['counterexample'] for r in json.loads(report.read_text())['reports']
                                  if 'counterexample' in r['info']]
                    assert executions
                    for execution in executions:
                        certificate = copy.deepcopy(execution)
                        program = certificate.pop('program')
                        certificate['program_sha256'] = digest_json(program)
                        assert check(program, certificate)['status'] == 'ACCEPT'
                        changed = False
                        for trace in certificate['traces']:
                            for step in trace['steps']:
                                inst = program['instructions'][step['statement']]
                                if (inst['kind'] == 'call' and inst['operands'][0].get('id') == 'ffs'
                                        and 'encoded_result' in step):
                                    step['encoded_result'] = str(int(step['encoded_result']) + 1)
                                    assert check(program, certificate)['status'] == 'REJECT'
                                    changed = True
                                    break
                            if changed:
                                break
                        if not {'INDIRECT', 'DEFINED'}.intersection(options):
                            assert changed, 'missing actual ffs result'
                print(f'ffs({value})/options={options}/valid={valid}: {verdict}', flush=True)

        for option, wide in ((option, wide) for option in
                ('WIDE_API', 'VARARGS', 'STDCALL', 'NO_RETURN', 'RETURNS_TWICE', 'MODULE_ASM', 'UNRESOLVED')
                for wide in (False, True)):
            source = work / 'renamed.c'
            source.write_text('#define ' + option + '\n' + ('#define WIDE\n' if wide else '') + fixture)
            proc = subprocess.run(runtime_command(args.ikos.resolve()) + ['--analyses=race',
                '--concurrency=auto', '--demote-race-to-unknown', f'-m{args.bits}',
                '--format=json', '--report-file=' + str(report),
                '-o', str(work / 'output.db'), str(source)],
                env=dict(os.environ, IKOS_BMC_RECOVER='1'), capture_output=True,
                text=True, timeout=30)
            assert proc.returncode == 0, (option, proc.stdout, proc.stderr)
            metadata = validate_race_result(json.loads(report.read_text()))
            if option == 'NO_RETURN':
                # LLVM makes the first writer unreachable after this declaration.
                assert metadata['verdict'] == 'SAFE' and metadata['coverage'] == 'Checked', metadata
                assert not metadata['search_performed'] and not metadata['obligations'], metadata
            else:
                assert metadata['verdict'] == 'UNKNOWN' and metadata['coverage'] == 'Blocked', (option, metadata)
                assert not metadata['search_performed'] and any(
                    o['reason'] == ('source call ABI attributes not covered' if option == 'STDCALL'
                                    else 'external call effects not covered') and not o['contract']
                    for o in metadata['obligations']), (option, metadata)
            print(f'ffs ABI/effect control {option}/wide={wide}: {metadata["verdict"]}', flush=True)


if __name__ == '__main__':
    main()
