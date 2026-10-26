#!/usr/bin/env python3
"""Check complete shallow loop alternatives without relaxing execution checks."""
import argparse, copy, json, os, subprocess, sys, tempfile
from pathlib import Path

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--repo',type=Path,default=Path(__file__).resolve().parents[1])
    p.add_argument('--ikos',type=Path)
    p.add_argument('--bits',choices=['32','64','both'],default='both')
    p.add_argument('--evidence-dir',type=Path)
    p.add_argument('--case',type=int,action='append')
    p.add_argument('--outer',type=int,action='append')
    args=p.parse_args();repo=args.repo.resolve();ikos=(args.ikos or repo/'install/bin/ikos').resolve()
    sys.path[:0]=[str(repo),str(repo/'scripts')]
    from ikos_runtime import runtime_command,runtime_identity
    from analyzer.python.ikos.race_result import validate_race_result
    from validate_bmc_execution import check,digest_json
    temporary=tempfile.TemporaryDirectory(prefix='ikos-finite-ladder-') if args.evidence_dir is None else None
    out=args.evidence_dir or Path(temporary.name)/'evidence';out.mkdir(parents=True,exist_ok=False)
    before=runtime_identity(ikos);fixture=(repo/'analyzer/test/regression/bmc-values/finite-ladder.c').read_text();rows=[]
    for bits in (['32','64'] if args.bits=='both' else [args.bits]):
      for case in (args.case or range(4)):
       for outer in (args.outer or ([2,3,4] if case==0 else [3])):
        for disabled in [False,True]:
         folder=out/f'case{case}-outer{outer}-{bits}-off{int(disabled)}';folder.mkdir()
         source=folder/'input.c';source.write_text(f'#define CASE {case}\n#define OUTER {outer}\n'+fixture)
         report=folder/'report.json';env={k:v for k,v in os.environ.items() if not k.startswith('IKOS_') and k not in ('PYTHONPATH','PYTHONHOME')}
         env.update(IKOS_BMC_RECOVER='1',IKOS_BMC_DIAGNOSTICS='1')
         if disabled:env['IKOS_BMC_DISABLE_FINITE_LADDER']='1'
         command=[*runtime_command(ikos),'--analyses=race','--concurrency=auto','--partitioning=guards','--demote-race-to-unknown','-m'+bits,'--bmc-deadline-ms=20000','--bmc-solver-timeout-ms=1000','--format=json','--report-file='+str(report),'-o',str(folder/'output.db'),str(source)]
         run=subprocess.run(command,env=env,cwd=folder,capture_output=True,text=True,timeout=60)
         (folder/'stdout.txt').write_text(run.stdout);(folder/'stderr.txt').write_text(run.stderr)
         assert run.returncode==0 and report.exists(),(folder,run.returncode,run.stderr)
         data=json.loads(report.read_text());meta=validate_race_result(data);executions=[x['info']['counterexample'] for x in data['reports'] if 'counterexample' in x.get('info',{})]
         counts=[x.get('info',{}).get('bmc',{}).get('counts',{}) for x in data['reports']]
         hits=sum(c.get('interrupted_shallow_bound_three',0) for c in counts)
         if disabled:assert hits==0,(folder,hits)
         if not disabled and case==0 and outer==4:assert hits>0,(folder,'new branch not exercised')
         if case in [0,1]:assert meta['verdict'] in ['FALSE','UNKNOWN'],(folder,meta)
         if case in [2,3]:assert meta['verdict'] in ['SAFE','UNKNOWN'] and not executions,(folder,meta)
         if meta['verdict']=='FALSE':assert executions,(folder,meta)
         for e in executions:
          c=copy.deepcopy(e);program=c.pop('program');c['program_sha256']=digest_json(program);assert check(program,c)['status']=='ACCEPT',folder
         rows.append(dict(case=case,outer=outer,bits=int(bits),disabled=disabled,verdict=meta['verdict'],hits=hits,counts=counts,executions=len(executions)))
         print(folder.name,meta['verdict'],hits,flush=True)
    assert before==runtime_identity(ikos)
    (out/'summary.json').write_text(json.dumps(dict(runtime=before,rows=rows),indent=2)+'\n')
if __name__=='__main__':main()
