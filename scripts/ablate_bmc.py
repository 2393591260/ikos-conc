#!/usr/bin/env python3
"""Serial paired AI-fact ablation with separate cost and capability outcomes."""
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


FACT_FLAGS = {
    'bounds': 'IKOS_BMC_DISABLE_AI_BOUNDS',
    'no-wrap': 'IKOS_BMC_DISABLE_AI_NO_WRAP',
    'single-execution': 'IKOS_BMC_DISABLE_AI_SINGLE_EXECUTION',
}
LEGACY_FLAG = 'IKOS_BMC_DISABLE_AI_FACTS'
FACTORS = {
    'numerical': ('bounds', 'no-wrap'),
    'bounds': ('bounds',),
    'no-wrap': ('no-wrap',),
    'single-execution': ('single-execution',),
    'all': tuple(FACT_FLAGS),
}


def fact_environment(inherited, factor, enabled):
    env = dict(inherited, IKOS_BMC_RECOVER='1')
    for name in (LEGACY_FLAG, *FACT_FLAGS.values(), 'IKOS_BMC_DIAGNOSTICS'):
        env.pop(name, None)
    if not enabled:
        flags = (LEGACY_FLAG,) if factor == 'all' else tuple(FACT_FLAGS[f] for f in FACTORS[factor])
        env.update(dict.fromkeys(flags, '1'))
    return env


def evidence_error(verdict, validations):
    if verdict == suite.VERDICT_RACE and not validations:
        return 'FALSE has no selected execution evidence'
    failures = [e.get('status', 'MISSING_STATUS') for e in validations if e.get('status') != 'ACCEPT']
    return 'selected execution evidence is not ACCEPT: ' + ', '.join(failures) if failures else None


def compare_samples(samples):
    transitions = []
    for repeat in sorted({r['repeat'] for r in samples}):
        pair = {r['ai_facts']: r for r in samples if r['repeat'] == repeat}
        assert set(pair) == {True, False}, 'missing ablation side'
        transitions.append(dict(repeat=repeat, with_facts=pair[True]['verdict'],
                                without_facts=pair[False]['verdict'],
                                same_verdict=pair[True]['verdict'] == pair[False]['verdict']))
    verdicts = {r['verdict'] for r in samples}
    comparable = len(verdicts) == 1 and all(
        not r['timed_out'] and not r['verdict'].startswith('Error') and not r['evidence_error']
        for r in samples)
    summary = dict(input=samples[0]['input'], verdict=samples[0]['verdict'] if len(verdicts) == 1 else None,
                   transitions=transitions, cost_comparable=comparable,
                   cost_scope='Matching observed outcomes only; no general capability-preservation claim'
                   if comparable else 'Descriptive timings only: outcomes, completion or evidence differ')
    for enabled in (True, False):
        selected = [r for r in samples if r['ai_facts'] == enabled]
        rss = [r['maxrss_kib'] for r in selected if r['maxrss_kib'] is not None]
        summary['with_facts' if enabled else 'without_facts'] = dict(
            verdict_counts=dict(Counter(r['verdict'] for r in selected)),
            timeouts=sum(r['timed_out'] for r in selected),
            evidence_failures=sum(r['evidence_error'] is not None for r in selected),
            median_seconds=statistics.median(r['seconds'] for r in selected),
            median_maxrss_kib=statistics.median(rss) if rss else None,
            median_metrics={k: statistics.median(r['metrics'].get(k, 0) for r in selected)
                            for k in sorted({k for r in selected for k in r['metrics']})})
    return summary


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--input',type=Path,action='append',required=True)
    p.add_argument('--ikos',type=Path,default=ROOT/'install/bin/ikos')
    p.add_argument('--analyzer',type=Path,help='binary fingerprint required with a custom wrapper')
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--factor',choices=FACTORS,default='numerical',
                   help='facts disabled on the without-facts side (default: bounds and no-wrap)')
    p.add_argument('--repeat',type=int,default=5)
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
                env=fact_environment(os.environ,args.factor,enabled)
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
                metrics=Counter();candidate_pairs=0;validation=[];error=None
                try:
                    data=json.loads(report.read_text()) if report.exists() else {}
                    if not isinstance(data,dict):raise ValueError('report is not an object')
                    for group in data.get('reports',[]):
                        if not isinstance(group,dict):raise ValueError('report group is not an object')
                        info=group.get('info') or {}
                        if not isinstance(info,dict):raise ValueError('report info is not an object')
                        group['info']=info
                        result=info.get('bmc_result') or {}
                        if not isinstance(result,dict):raise ValueError('bmc_result is not an object')
                        metrics.update(result.get('metrics',{}))
                        candidate_pairs+=len(info.get('conflicting_accesses',[]))
                    validation=validate_report(data)
                    error=evidence_error(verdict,validation)
                except (KeyError,TypeError,ValueError) as exc:
                    error='malformed report/execution evidence: '+str(exc)
                values=usage.read_text().strip().split() if usage.exists() else []
                if values and len(values)>4:values=values[-4:]
                rss=int(values[3]) if len(values)==4 else None
                rows.append(dict(input=str(source),source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                    repeat=repeat,ai_facts=enabled,verdict=verdict,seconds=elapsed,maxrss_kib=rss,
                    candidate_pairs=candidate_pairs,metrics=dict(metrics),validations=validation,
                    command=cmd,data_model=args.bits,timeout=args.timeout,timed_out=timed_out,returncode=proc.returncode,
                    factor=args.factor,disabled_facts=[] if enabled else list(FACTORS[args.factor]),
                    environment={k:env[k] for k in ('IKOS_BMC_RECOVER',LEGACY_FLAG,*FACT_FLAGS.values()) if k in env},
                    evidence_error=error))
                print(source.name,repeat,enabled,verdict,round(elapsed,3),dict(metrics),flush=True)
                if error:print(source.name,error,file=sys.stderr,flush=True)
    comparisons=[]
    for source in args.input:
        samples=[r for r in rows if r['input']==str(source.resolve())]
        comparisons.append(compare_samples(samples))
    summary=dict(tasks=len(args.input),runs=len(rows),jobs=1,repeat=args.repeat,comparisons=comparisons,
        analyzer_sha256=hashlib.sha256(args.analyzer.read_bytes()).hexdigest(),
        factor=args.factor,disabled_facts=list(FACTORS[args.factor]),
        retained_facts=[f for f in FACT_FLAGS if f not in FACTORS[args.factor]],
        evidence_failures=sum(r['evidence_error'] is not None for r in rows),
        timeouts=sum(r['timed_out'] for r in rows),
        retained='AI SAFE, may-candidate filtering, typed initialization and startup instructions; disabling single-execution may remove supported helper publication',
        ai_facts_field='Whether the selected factor is enabled; other facts are retained on both sides',
        metric_scope={'ai_single_execution': 'Positive annotations of non-main pointer-store occurrences in attempted prefixes; not unique functions or successful publications'},
        memory_scope='/usr/bin/time maximum reported RSS, not summed cgroup peak',
        scope='Small controlled sample; not a global speedup or a comparison with Dartagnan')
    (args.output/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
    (args.output/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    return int(summary['evidence_failures'] != 0)


if __name__=='__main__':raise SystemExit(main())
