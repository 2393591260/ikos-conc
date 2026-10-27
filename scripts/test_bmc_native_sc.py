#!/usr/bin/env python3
"""Native SC integration, rejection and independent replay controls."""
import argparse, copy, hashlib, json, os
from pathlib import Path
import subprocess, sys, tempfile
from ikos_runtime import runtime_command, runtime_identity, verify_runtime
from validate_bmc_execution import check, digest_json
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from svcomp_race_test import classify, VERDICT_RACE, VERDICT_UNKNOWN, VERDICT_SAFE
POSITIVE={0,2,4,5,6,10,12,13,15,16}
# 8 is a broken premise, 11 impossible control, 7/9/14 unsupported semantics.
NEGATIVE={1,3,7,8,9,11,14}
def replay(info):
 c=copy.deepcopy(info['counterexample']);p=c.pop('program');c['program_sha256']=digest_json(p)
 result=check(p,c);assert result['status']=='ACCEPT',result
 native=[(t['id'],i) for t in c['traces'] for i,s in enumerate(t['steps'])
         if p['instructions'][s['statement']].get('native_sc')]
 assert native,'positive native test must actually execute a native operation'
 changed=copy.deepcopy(c);name,i=native[0]
 step=next(t for t in changed['traces'] if t['id']==name)['steps'][i]
 ins=p['instructions'][step['statement']]
 if ins['kind']=='load':step.pop('read_from',None)
 else:step['clock']=str(10**9)
 assert check(p,changed)['status']!='ACCEPT','broken native evidence accepted'
 return result

def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--bits',choices=('32','64'),default='32');p.add_argument('--ikos',type=Path,default=ROOT/'install/bin/ikos')
 p.add_argument('--output',type=Path);args=p.parse_args();ikos=args.ikos.resolve();identity=runtime_identity(ikos);rows=[]
 fixture=(ROOT/'analyzer/test/regression/bmc-values/native-sc.c').read_text()
 validator=hashlib.sha256((ROOT/'scripts/validate_bmc_execution.py').read_bytes()).hexdigest()
 with tempfile.TemporaryDirectory(prefix='ikos-native-sc-') as temp:
  work=Path(temp)
  for datum_bits in (8,16,32,64):
   wide=datum_bits==64
   for case in sorted(POSITIVE|NEGATIVE):
    source=work/'renamed.c';source.write_text(f'#define CASE {case}\n'+({8:'#define TINY\n',16:'#define SHORT\n',32:'',64:'#define WIDE\n'}[datum_bits])+fixture.replace('payload','shared_result').replace('first','worker_left').replace('second','worker_right'))
    for disabled in (False,True):
     report=work/'report.json';report.unlink(missing_ok=True)
     env={k:v for k,v in os.environ.items() if not k.startswith('IKOS_')};env.pop('PYTHONPATH',None);env.pop('PYTHONHOME',None)
     env.update(IKOS_BMC_RECOVER='1',IKOS_BMC_DIAGNOSTICS='1')
     if disabled:env['IKOS_BMC_DISABLE_NATIVE_SC']='1'
     cmd=runtime_command(ikos)+['--analyses=race','--concurrency=auto','--demote-race-to-unknown','-m',args.bits,'--bmc-deadline-ms=20000','--bmc-solver-timeout-ms=1000','--format=json','--report-file='+str(report),'-o',str(work/'output.db'),str(source)]
     proc=subprocess.run(cmd,cwd=work,capture_output=True,text=True,env=env,timeout=30)
     if args.output:
      evidence=args.output.parent/('artifacts-'+args.bits)/f'case{case}-bits{datum_bits}-off{int(disabled)}'
      evidence.mkdir(parents=True,exist_ok=True)
      (evidence/'source.c').write_text(source.read_text())
      (evidence/'stdout.log').write_text(proc.stdout+proc.stderr)
      (evidence/'command.json').write_text(json.dumps(cmd,indent=2)+'\n')
      if report.exists():(evidence/'report.json').write_bytes(report.read_bytes())
     actual=classify(proc.stdout+proc.stderr,proc.returncode,False);israce=actual==VERDICT_RACE
     expected=case in POSITIVE and not disabled
     # Unsupported fence import is an honest rejection, never a successful analysis.
     assert israce==expected,(args.bits,wide,case,disabled,actual,proc.stdout,proc.stderr)
     if proc.returncode==0:
      if expected:assert actual==VERDICT_RACE
      elif case in (1,3,11):assert actual in (VERDICT_SAFE,VERDICT_UNKNOWN)
      else:assert actual==VERDICT_UNKNOWN,(case,disabled,'racy/unsupported control escaped UNKNOWN',actual)
     if proc.returncode:
      message=(proc.stdout+proc.stderr).lower()
      assert case==9 and 'fence' in message and any(x in message for x in ('unsupported','unexpected','import')), (case,proc.returncode,message)
     checks=[]
     if israce:
      data=json.loads(report.read_text())
      checks=[replay(r['info']) for r in data.get('reports',[]) if 'counterexample' in r.get('info',{})]
      assert checks,'confirmed result lacks independent native replay'
     rows.append(dict(bits=args.bits,datum_bits=datum_bits,wide=wide,case=case,disabled=disabled,verdict=actual,returncode=proc.returncode,replays=checks))
 verify_runtime(identity)
 assert validator==hashlib.sha256((ROOT/'scripts/validate_bmc_execution.py').read_bytes()).hexdigest()
 if args.output:args.output.write_text(json.dumps(dict(status='PASS',rows=rows,official_source_witness=False),indent=2)+'\n')
 print(f'{len(rows)} native SC integration controls passed')
if __name__=='__main__':main()
