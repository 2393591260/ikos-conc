#!/usr/bin/env python3
"""Exercise real analyzer snapshots, exclusion models, and conservative fallbacks."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import sys

ROOT = Path(__file__).resolve().parents[3]

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / 'scripts'))

CASES = [0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 13]
CONFIGS = [
    ('interval', 'interval', None),
    ('congruence', 'interval-congruence', None),
    ('weak-query', 'interval', '1'),
    ('native', 'interval-congruence', 'native'),
    ('query', 'interval-congruence', '1'),
]


def validate_model(row):
    if row.get('status') != 'SAT':
        return
    model = {k: int(v) for k, v in row['abstract_model'].items()}
    space = 2 ** row['pointer_bits']
    for side in ('a', 'b'):
        facts = row[side]
        start, byte = model['start_' + side], model['byte_' + side]
        assert int(facts['lo']) <= start <= int(facts['hi']), row
        modulus, residue = int(facts['modulus']), int(facts['residue'])
        assert (start - residue) % modulus == 0 if modulus else start == residue
        assert 0 <= byte < int(facts['width']), row
        assert (start + byte) % space == model['address'], row


def main(args):
    args.output.mkdir(parents=True, exist_ok=False)
    renamed = args.output / 'renamed.c'
    renamed.write_text((HERE / 'feedback.c').read_text().replace('data', 'cells').replace('worker', 'thread_body'))
    rows = []
    for bits in (32, 64):
        for case in CASES:
            for name, domain, switch in CONFIGS:
                folder = args.output / f'{bits}-{case}-{name}'
                folder.mkdir()
                evidence = folder / 'exclusions.jsonl'
                env = dict(os.environ)
                env.pop('IKOS_BMC_RECOVER', None)
                if args.bmc:
                    env['IKOS_BMC_RECOVER'] = '1'
                env.pop('IKOS_AI_PAIR_EXCLUSION', None)
                env.pop('IKOS_AI_PAIR_EXCLUSION_REPORT', None)
                if switch:
                    env['IKOS_AI_PAIR_EXCLUSION'] = switch
                    env['IKOS_AI_PAIR_EXCLUSION_REPORT'] = str(evidence)
                command = [str(args.ikos.resolve()), '--analyses=race', '--concurrency=auto',
                           '--demote-race-to-unknown', '-m', str(bits), '-d', domain,
                           f'-DCASE={case}', '--format=json',
                           '--report-file=' + str(folder / 'report.json'),
                           '-o', str(folder / 'output.db'), str(renamed if bits == 64 else HERE / 'feedback.c')]
                started = time.monotonic()
                proc = subprocess.run(command, cwd=folder, env=env, capture_output=True,
                                      text=True, timeout=30)
                elapsed = time.monotonic() - started
                (folder / 'run.log').write_text(proc.stdout + proc.stderr)
                verdict = next((line for line in proc.stdout.splitlines()
                                if line.startswith('The program is')), 'NONE')
                assert proc.returncode == 0 and verdict != 'NONE', (folder, proc.stderr)
                records = [json.loads(line) for line in evidence.read_text().splitlines()] if evidence.exists() else []
                for record in records:
                    validate_model(record)
                summaries = [record for record in records if record.get('kind') == 'summary']
                safe = case in (0, 1, 6, 7, 8)
                executions = []
                if args.bmc:
                    from sweep_bmc_execution import validate_report
                    executions = validate_report(json.loads((folder/'report.json').read_text()))
                if not args.observe:
                    if safe:
                        assert verdict != 'The program is definitely UNSAFE', (folder, verdict)
                    if verdict == 'The program is definitely UNSAFE':
                        assert executions and all(e['status'] == 'ACCEPT' for e in executions), (folder, executions)
                    if not safe:
                        assert verdict != 'The program is SAFE', (folder, verdict)
                    if case == 0 and name in ('native', 'query'):
                        assert verdict == 'The program is SAFE', (folder, verdict)
                        assert sum(r['native_excluded'] for r in summaries) > 0, folder
                    if case == 1 and name == 'query':
                        assert verdict == 'The program is SAFE', (folder, verdict)
                        assert sum(r['smt_excluded'] for r in summaries) > 0, folder
                    if case == 1 and name in ('interval', 'congruence', 'native'):
                        assert verdict != 'The program is SAFE', (folder, verdict)
                    if case in (7, 8):
                        assert sum(r['solver_queries'] + r['native_excluded'] for r in summaries) == 0, folder
                rows.append(dict(bits=bits, case=case, config=name, domain=domain,
                                 verdict=verdict, wall_seconds=elapsed, summaries=summaries, executions=executions,
                                 model_count=sum(r.get('status') == 'SAT' for r in records),
                                 command=command))
                print(folder.name, verdict, flush=True)
                (args.output / 'summary.json').write_text(json.dumps(rows, indent=2)+'\n')
    (args.output / 'metadata.json').write_text(json.dumps(dict(
        fixture_sha256=hashlib.sha256((HERE / 'feedback.c').read_bytes()).hexdigest(),
        observations=len(rows), mode='bmc' if args.bmc else 'ai_only', exact_smt_models_are_executions=False,
        note='Output facts and models are local AI overapproximations, not race witnesses.'), indent=2)+'\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=Path('/home/ruan/ikos-conc/install/bin/ikos'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--observe', action='store_true')
    parser.add_argument('--bmc', action='store_true')
    main(parser.parse_args())
