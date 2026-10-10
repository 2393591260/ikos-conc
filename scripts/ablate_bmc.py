#!/usr/bin/env python3
"""Serial paired ablation of numerical AI facts; preserve SAFE/candidates/activation."""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import signal
import statistics
import subprocess
import sys
import time
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
import svcomp_race_test as suite
from sweep_bmc_execution import validate_report


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--input',type=Path,action='append',required=True)
    p.add_argument('--ikos',type=Path,default=ROOT/'install/bin/ikos')
    p.add_argument('--analyzer',type=Path,help='binary fingerprint required with a custom wrapper')
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--repeat',type=int,default=3)
    p.add_argument('--bits',type=int,choices=(32,64),default=32)
    p.add_argument('--timeout',type=int,default=30)
    args=p.parse_args()
    if args.analyzer is None:
        if args.ikos.resolve() != (ROOT/'install/bin/ikos').resolve():p.error('--analyzer is required with a custom wrapper')
        args.analyzer=ROOT/'install/bin/ikos-analyzer'
    args.output=args.output.resolve();args.output.mkdir(parents=True,exist_ok=False)
    assert args.repeat>0 and args.timeout>0
    rows=[]
    for source in args.input:
        source=source.resolve();key=source.stem+'-'+hashlib.sha256(str(source).encode()).hexdigest()[:8]
        for repeat in range(args.repeat):
            for enabled in ((True,False) if repeat%2==0 else (False,True)):
                folder=args.output/f'{key}-{repeat}-{int(enabled)}';folder.mkdir()
                report=folder/'report.json';usage=folder/'usage.txt'
                env=dict(os.environ,IKOS_BMC_RECOVER='1');env.pop('IKOS_BMC_DIAGNOSTICS',None)
                env.pop('IKOS_BMC_DISABLE_AI_FACTS',None)
                if not enabled:env['IKOS_BMC_DISABLE_AI_FACTS']='1'
                cmd=['/usr/bin/time','-f','%e %U %S %M','-o',str(usage),str(args.ikos.resolve()),
                     '--analyses=race','--concurrency=auto','--demote-race-to-unknown','-m',str(args.bits),
                     '--format=json','--report-file='+str(report),'-o',str(folder/'output.db'),str(source)]
                start=time.monotonic();timed_out=False
                proc=subprocess.Popen(cmd,cwd=folder,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,start_new_session=True)
                try:out,_=proc.communicate(timeout=args.timeout)
                except subprocess.TimeoutExpired:
                    timed_out=True
                    try:os.killpg(proc.pid,signal.SIGKILL)
                    except ProcessLookupError:pass
                    out,_=proc.communicate()
                elapsed=time.monotonic()-start;text=out.decode(errors='replace');(folder/'run.log').write_text(text)
                verdict=suite.classify(text,proc.returncode,timed_out)
                data=json.loads(report.read_text()) if report.exists() else {};metrics=Counter();candidate_pairs=0
                for group in data.get('reports',[]):
                    info=group['info'];metrics.update(info.get('bmc_result',{}).get('metrics',{}))
                    candidate_pairs+=len(info.get('conflicting_accesses',[]))
                values=usage.read_text().strip().split() if usage.exists() else []
                if values and len(values)>4:values=values[-4:]
                rss=int(values[3]) if len(values)==4 else None
                validation=validate_report(data)
                assert all(e['status']=='ACCEPT' for e in validation),(source,enabled,validation)
                rows.append(dict(input=str(source),source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                    repeat=repeat,ai_facts=enabled,verdict=verdict,seconds=elapsed,maxrss_kib=rss,
                    candidate_pairs=candidate_pairs,metrics=dict(metrics),validations=validation,
                    command=cmd,data_model=args.bits,timeout=args.timeout))
                print(source.name,repeat,enabled,verdict,round(elapsed,3),dict(metrics),flush=True)
    comparisons=[]
    for source in args.input:
        samples=[r for r in rows if r['input']==str(source.resolve())]
        for repeat in range(args.repeat):
            pair=[r for r in samples if r['repeat']==repeat]
            assert len(pair)==2 and pair[0]['verdict']==pair[1]['verdict'],pair
        summary=dict(input=str(source.resolve()),verdict=samples[0]['verdict'])
        for enabled in (True,False):
            selected=[r for r in samples if r['ai_facts']==enabled]
            summary['with_facts' if enabled else 'without_facts']=dict(
                median_seconds=statistics.median(r['seconds'] for r in selected),
                median_maxrss_kib=statistics.median(r['maxrss_kib'] for r in selected if r['maxrss_kib'] is not None),
                median_metrics={k:statistics.median(r['metrics'].get(k,0) for r in selected)
                                for k in sorted({k for r in selected for k in r['metrics']})})
        comparisons.append(summary)
    summary=dict(tasks=len(args.input),runs=len(rows),jobs=1,repeat=args.repeat,comparisons=comparisons,
        analyzer_sha256=hashlib.sha256(args.analyzer.read_bytes()).hexdigest(),
        retained='AI SAFE, may-candidate filtering, exact initialization, actual startup/activation; disables only BMC offset bounds and local no-wrap certificates',
        memory_scope='/usr/bin/time maximum reported RSS, not summed cgroup peak',
        scope='Small controlled sample; not a global speedup or a comparison with Dartagnan')
    (args.output/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
    (args.output/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')


if __name__=='__main__':main()
