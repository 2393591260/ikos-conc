#!/usr/bin/env python3
"""Check actual stream HB in atomic-target certificates and a serialized control."""
import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'scripts'))
from validate_bmc_execution import check,digest_json


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ikos',type=Path,default=ROOT/'install/bin/ikos')
    p.add_argument('--output',type=Path,required=True)
    args=p.parse_args();args.output.mkdir(parents=True,exist_ok=False);rows=[]
    for bits in (32,64):
        for fmt in ('puts','printf','guarded'):
            folder=args.output/f'{bits}-{fmt}';folder.mkdir();source=folder/'renamed.c'
            text=Path(__file__).with_name('stream.c').read_text().replace('data','payload')
            if fmt=='guarded':
                text=text.replace('extern void __VERIFIER_atomic_begin(void);','extern void __VERIFIER_assume(int);\nextern void __VERIFIER_atomic_begin(void);')
                text=text.replace('volatile int sink=payload; (void)sink','int value=payload; __VERIFIER_assume(value==1)')
            else:text=text.replace('puts(',fmt+'(')
            source.write_text(text)
            report=folder/'report.json'
            cmd=[str(args.ikos.resolve()),'--analyses=race','--concurrency=auto','--demote-race-to-unknown','-m',str(bits),
                 '--format=json','--report-file='+str(report),'-o',str(folder/'output.db'),str(source)]
            proc=subprocess.run(cmd,cwd=folder,env=dict(os.environ,IKOS_BMC_RECOVER='1'),capture_output=True,text=True,timeout=30)
            (folder/'run.log').write_text(proc.stdout+proc.stderr);assert proc.returncode==0
            certs=[r['info']['counterexample'] for r in json.loads(report.read_text())['reports'] if 'counterexample' in r['info']]
            assert certs,(bits,fmt,proc.stdout)
            for original in certs:
                c=copy.deepcopy(original);program=c.pop('program');c['program_sha256']=digest_json(program)
                assert check(program,c)['status']=='ACCEPT',(bits,fmt,check(program,c))
                writer=next(t for t in c['traces'] if t['entry']=='writer');reader=next(t for t in c['traces'] if t['entry']=='reader')
                wi=next(i for i,s in enumerate(writer['steps']) if program['instructions'][s['statement']]['kind']=='store')
                for i,s in enumerate(writer['steps']):s['clock']=str(i)
                for i,s in enumerate(reader['steps']):
                    s['clock']=str(len(writer['steps'])+1+i)
                    if 'read_from' in s and program['instructions'][s['statement']]['operands'][0]['kind']=='global':
                        s['read_from']=[dict(trace=writer['id'],step=wi) for _ in s['read_from']]
                    if 'encoded_result' in s and program['instructions'][s['statement']]['kind']=='load':s['encoded_result']='1'
                result=check(program,c)
                assert result==dict(status='REJECT',reason='targets ordered by pthread synchronization'),(bits,fmt,result)
            rows.append(dict(bits=bits,format=fmt,selected='ACCEPT',serial_stream='REJECT'))
            print(bits,fmt,'actual race accepted; stream-ordered pair rejected',flush=True)
    for bits in (32,64):
        folder=args.output/f'{bits}-joined-control';folder.mkdir();source=folder/'control.c'
        text=Path(__file__).with_name('stream.c').read_text().replace('pthread_create(&b,0,reader,0);','pthread_join(a,0);pthread_create(&b,0,reader,0);')
        source.write_text(text)
        report=folder/'report.json'
        cmd=[str(args.ikos.resolve()),'--analyses=race','--concurrency=auto','--demote-race-to-unknown','-m',str(bits),
             '--format=json','--report-file='+str(report),'-o',str(folder/'output.db'),str(source)]
        proc=subprocess.run(cmd,cwd=folder,env=dict(os.environ,IKOS_BMC_RECOVER='1'),capture_output=True,text=True,timeout=30)
        assert proc.returncode==0 and 'The program is definitely UNSAFE' not in proc.stdout,(bits,proc.stdout)
        rows.append(dict(bits=bits,case='joined-control',false_confirmation=False))
    (args.output/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')


if __name__=='__main__':main()
