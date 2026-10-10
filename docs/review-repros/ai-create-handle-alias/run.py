#!/usr/bin/env python3
"""Ambiguous pthread_create outputs preserve every old handle alternative."""
import argparse,json,os,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'scripts'))
from sweep_bmc_execution import validate_report

def main(args):
 args.output.mkdir(parents=True,exist_ok=False);rows=[]
 renamed=args.output/'renamed.c';renamed.write_text(Path(__file__).with_name('choice.c').read_text().replace('data','payload').replace('left','first').replace('right','second'))
 for bits in (32,64):
  for case in range(8):
   for mode in ('ai','bmc'):
    folder=args.output/f'{bits}-{case}-{mode}';folder.mkdir();env=dict(os.environ);env.pop('IKOS_BMC_RECOVER',None)
    if mode=='bmc':env['IKOS_BMC_RECOVER']='1'
    source=renamed if bits==64 else Path(__file__).with_name('choice.c').resolve()
    cmd=[str(args.ikos.resolve()),'--analyses=race','--concurrency=auto','--demote-race-to-unknown','-m',str(bits),f'-DCASE={case}','--format=json','--report-file='+str(folder/'report.json'),'-o',str(folder/'output.db'),str(source)]
    proc=subprocess.run(cmd,cwd=folder,env=env,capture_output=True,text=True,timeout=30);(folder/'run.log').write_text(proc.stdout+proc.stderr)
    verdict=next((s for s in proc.stdout.splitlines() if s.startswith('The program is')),'NONE');assert proc.returncode==0 and verdict!='NONE',(folder,proc.stderr)
    executions=validate_report(json.loads((folder/'report.json').read_text())) if mode=='bmc' else []
    expected='Race Found' if case in (0,3,4,6) else 'Safe'
    if not args.observe:
     if expected=='Race Found':assert verdict!='The program is SAFE',(folder,verdict)
     else:assert verdict!='The program is definitely UNSAFE',(folder,verdict)
     if case==2:assert verdict=='The program is SAFE',(folder,verdict)
     if verdict=='The program is definitely UNSAFE':assert executions and all(e['status']=='ACCEPT' for e in executions),(folder,executions)
    rows.append(dict(bits=bits,case=case,mode=mode,expected=expected,verdict=verdict,executions=executions));print(folder.name,verdict,flush=True)
 (args.output/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--ikos',type=Path,default=ROOT/'install/bin/ikos');p.add_argument('--output',type=Path,required=True);p.add_argument('--observe',action='store_true');main(p.parse_args())
