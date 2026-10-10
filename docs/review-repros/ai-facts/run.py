#!/usr/bin/env python3
"""Check the restricted AI proof consumption and concrete executions in both ABIs."""
import argparse
import json
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[3]


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
    args=p.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    rows=[]
    for bits in (32,64):
        target=args.output/str(bits)
        subprocess.run(['python3',str(ROOT/'scripts/ablate_bmc.py'),'--input',str(Path(__file__).with_name('unused-local.c')),
                        '--repeat','1','--bits',str(bits),'--output',str(target)],check=True)
        samples=json.loads((target/'results.json').read_text())
        assert len(samples)==2
        for row in samples:
            assert row['verdict']=='Race Found' and row['validations']
            assert row['metrics']['path_queries']==1 and row['metrics']['solver_queries']>=1
            assert row['metrics']['ai_no_wrap']==(1 if row['ai_facts'] else 0)
            assert (row['metrics']['ai_bounds']>0)==row['ai_facts']
            assert all(e['status']=='ACCEPT' for e in row['validations'])
        rows.append(dict(bits=bits,verdict='Race Found',proof_consumed=True,all_executions='ACCEPT'))
    (args.output/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')


if __name__=='__main__':main()
