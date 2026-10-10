#!/usr/bin/env python3
"""Check actual callee/rf binding, controls and dynamic helper contexts."""
import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'scripts'))
from validate_bmc_execution import check, digest_json
RACY={0,3,5,6,7,9,10,11,13}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ikos',type=Path,default=ROOT/'install/bin/ikos')
    p.add_argument('--output',type=Path,required=True)
    args=p.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    renamed=args.output/'renamed.c'
    renamed.write_text((Path(__file__).with_name('callback.c')).read_text().replace('slot','dispatch').replace('callback','action'))
    rows=[]
    for bits in (32,64):
        for case in range(14):
            folder=args.output/f'{bits}-{case}';folder.mkdir()
            report=folder/'report.json'
            cmd=[str(args.ikos.resolve()),'--analyses=race','--concurrency=auto','--demote-race-to-unknown',
                 '-m',str(bits),'-DCASE='+str(case),'--format=json','--report-file='+str(report),
                 '-o',str(folder/'output.db'),str(renamed.resolve())]
            proc=subprocess.run(cmd,cwd=folder,env=dict(os.environ,IKOS_BMC_RECOVER='1'),
                                capture_output=True,text=True,timeout=30)
            (folder/'run.log').write_text(proc.stdout+proc.stderr)
            assert proc.returncode==0,(bits,case,proc.stdout+proc.stderr)
            confirmed=[r['info']['counterexample'] for r in json.loads(report.read_text())['reports'] if 'counterexample' in r['info']]
            assert bool(confirmed)==(case in RACY),(bits,case,proc.stdout+proc.stderr)
            validations=[]
            for original in confirmed:
                cert=copy.deepcopy(original);program=cert.pop('program');cert['program_sha256']=digest_json(program)
                result=check(program,cert);assert result['status']=='ACCEPT',(bits,case,result)
                validations.append(result['status'])
                calls=[(t['id'],i) for t in cert['traces'] for i,s in enumerate(t['steps']) if 'expanded_callee' in s]
                if calls:
                    changed=copy.deepcopy(cert);name,i=calls[-1]
                    next(t for t in changed['traces'] if t['id']==name)['steps'][i]['expanded_callee']='impossible-target'
                    assert check(program,changed)['status']=='REJECT'
                reads=[(t['id'],i) for t in cert['traces'] for i,s in enumerate(t['steps']) if 'encoded_pointer' in s]
                if reads:
                    changed=copy.deepcopy(cert);name,i=reads[0]
                    next(t for t in changed['traces'] if t['id']==name)['steps'][i]['encoded_pointer']=dict(kind='function',id='impossible-target')
                    assert check(program,changed)['status']=='REJECT'
            rows.append(dict(bits=bits,case=case,confirmed=bool(confirmed),validations=validations))
            print(bits,case,validations or 'no false confirmation',flush=True)
    (args.output/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')


if __name__=='__main__':main()
