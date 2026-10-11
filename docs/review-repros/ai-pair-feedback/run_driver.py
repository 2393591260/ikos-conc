#!/usr/bin/env python3
"""Check model-selected fresh analysis, same IR, and failed-retry fallback."""
import argparse
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'scripts'))
import ai_feedback


def main(args):
    args.output.mkdir(parents=True, exist_ok=False)
    source = Path(__file__).with_name('feedback.c').resolve()
    renamed = args.output / 'renamed.c'
    renamed.write_text(source.read_text().replace('data', 'cells').replace('worker', 'thread_body'))
    model = dict(engine='smt', status='SAT',
                 abstract_model=dict(start_a='4', start_b='4'),
                 a=dict(lo='0', hi='8', modulus='1', access_id=1),
                 b=dict(lo='0', hi='8', modulus='1', access_id=2))
    assert ai_feedback.refinement_hint([model])
    endpoint = dict(model, abstract_model=dict(start_a='0', start_b='8'))
    assert ai_feedback.refinement_hint([endpoint]) is None
    assert ai_feedback.refinement_hint([dict(model, status='UNSAT')]) is None
    rows = []
    for bits in (32, 64):
        for case in (0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 13):
            opts = argparse.Namespace(source=renamed if bits == 64 else source,
                output=args.output / f'{bits}-{case}', prefix=ROOT / 'install',
                bits=bits, define=[f'CASE={case}'], timeout=30, analyzer=None, bmc=False)
            assert ai_feedback.main(opts) == 0
            result = json.loads((opts.output / 'summary.json').read_text())
            rounds = result['rounds']
            assert 1 <= len(rounds) <= 2
            assert len({r['pp_bc_sha256'] for r in rounds}) == 1
            assert len({r['database'] for r in rounds}) == len(rounds)
            safe = case in (0, 1, 6, 7, 8)
            assert result['final_verdict'] != ('The program is definitely UNSAFE' if safe else 'The program is SAFE')
            if case in (0, 1):
                assert result['final_verdict'] == 'The program is SAFE' and len(rounds) == 2
            rows.append(dict(bits=bits, case=case, verdict=result['final_verdict'],
                rounds=len(rounds), pp_bc_sha256=result['pp_bc_sha256'],
                model_used=result['refinement_hint'] is not None,
                wall_seconds=result['wall_seconds']))
    failures = []
    for kind in ('error', 'timeout'):
        stub = args.output / (kind + '-analyzer')
        action = 'raise SystemExit(7)' if kind == 'error' else 'time.sleep(10)'
        binary = str(ROOT / 'install/bin/ikos-analyzer')
        stub.write_text("#!/usr/bin/env python3\nimport os,sys,time\n"
            "if '-d=interval-congruence' in sys.argv:\n    " + action + "\n"
            "os.execv(" + repr(binary) + ", [" + repr(binary) + "] + sys.argv[1:])\n")
        stub.chmod(0o755)
        opts = argparse.Namespace(source=source, output=args.output / (kind + '-run'),
            prefix=ROOT / 'install', bits=32, define=['CASE=1'], timeout=2,
            analyzer=stub, bmc=False)
        assert ai_feedback.main(opts) == 0
        result = json.loads((opts.output / 'summary.json').read_text())
        assert len(result['rounds']) == 2
        assert result['rounds'][1]['returncode'] != 0
        assert result['final_verdict'] == result['rounds'][0]['verdict'] != 'The program is SAFE'
        failures.append(dict(kind=kind, returncodes=[r['returncode'] for r in result['rounds']],
            final_verdict=result['final_verdict'],
            same_ir=len({r['pp_bc_sha256'] for r in result['rounds']}) == 1))
    (args.output / 'summary.json').write_text(json.dumps(dict(
        hint_controls=3, fixtures=rows, failed_retries=failures), indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    main(parser.parse_args())
