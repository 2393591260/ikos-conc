#!/usr/bin/env python3
"""Check opaque heap handle sources, loaded generations and actual completions."""
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
from validate_bmc_execution import check, digest_json
from analyzer.python.ikos.race_result import validate_race_result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--bits', type=int, choices=(32, 64), default=32)
    args = parser.parse_args()
    fixture = (ROOT / 'analyzer/test/regression/bmc-auxiliary/heap-completion.c').read_text()
    positive = {0, 1, 12, 20, 25, 26}
    cases = [(case, False, False) for case in range(28)]
    cases += [(case, True, False) for case in (0, 1, 2, 3, 12)]
    cases += [(case, False, True) for case in (0, 1, 2, 12, 20, 21)]
    with tempfile.TemporaryDirectory(prefix='ikos-heap-completion-') as temp:
        work = Path(temp)
        for case, wide, literal, covered in [(*c, covered) for covered in (False, True) for c in cases]:
            source = work / 'renamed.c'
            body = fixture
            if covered:
                body = body.replace('extern void __VERIFIER_assume(int);',
                    'static void __VERIFIER_assume(int c) { if (!c) __builtin_abort(); }')
            source.write_text(f'#define CASE {case}\n' + ('#define WIDE\n' if wide else '') +
                              ('#define LITERAL\n' if literal else '') +
                              body.replace('writer', 'entry').replace('datum', 'payload'))
            report = work / 'report.json'
            report.unlink(missing_ok=True)
            proc = subprocess.run(runtime_command(args.ikos.resolve()) + ['--analyses=race',
                '--concurrency=auto', '--demote-race-to-unknown', f'-m{args.bits}', '--format=json',
                '--report-file=' + str(report), '-o', str(work / 'output.db'), str(source)],
                env=dict(os.environ, IKOS_BMC_RECOVER='1'), capture_output=True,
                text=True, timeout=30)
            verdict = classify(proc.stdout, proc.returncode, False)
            data = json.loads(report.read_text())
            metadata = validate_race_result(data)
            if not covered and not literal:
                assert metadata['verdict'] == 'UNKNOWN' and not metadata['search_performed'], metadata
                assert any(o['reason'] == 'external call effects not covered' and not o['contract']
                           for o in metadata['obligations']), metadata
            confirmed = (covered or literal) and case in positive
            assert proc.returncode == 0 and (verdict == VERDICT_RACE) == confirmed, (
                case, wide, literal, verdict, proc.stdout, proc.stderr)
            if confirmed:
                executions = [r['info']['counterexample'] for r in json.loads(report.read_text())['reports']
                              if 'counterexample' in r['info']]
                assert executions
                for execution in executions:
                    certificate = copy.deepcopy(execution)
                    program = certificate.pop('program')
                    certificate['program_sha256'] = digest_json(program)
                    result = check(program, certificate)
                    assert result['status'] == 'ACCEPT', (case, result)
                    if case == 25:
                        assert set(result['instances']) in ({0, 1}, {0, 2})
                        continue  # This conflict precedes the joins; no completion trace is needed.
                    assert set(result['instances']) == ({0, 2} if case in (0, 12, 20, 26) else {0, 1})
                    sources = []
                    for trace in certificate['traces']:
                        for index, step in enumerate(trace['steps']):
                            for origin in step.get('read_from', []):
                                if isinstance(origin, dict) and origin.get('trace') == trace['id']:
                                    source_step = trace['steps'][origin['step']]
                                    if program['instructions'][source_step['statement']]['kind'] == 'call':
                                        sources.append((trace['id'], index))
                    assert sources, 'actual create write source missing'
                    broken = copy.deepcopy(certificate)
                    label, index = sources[0]
                    changed = next(t for t in broken['traces'] if t['id'] == label)['steps'][index]
                    changed['read_from'] = ['initial'] * len(changed['read_from'])
                    assert check(program, broken)['status'] == 'REJECT'
                    broken = copy.deepcopy(certificate)
                    auxiliary = next(t for t in broken['traces'] if t['id'].startswith('aux_'))
                    auxiliary['steps'].pop()
                    assert check(program, broken)['status'] == 'REJECT'
            print(f'heap-completion case={case}/wide={wide}/literal={literal}/covered={covered}: {verdict}', flush=True)


if __name__ == '__main__':
    main()
