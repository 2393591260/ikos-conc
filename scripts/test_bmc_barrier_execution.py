#!/usr/bin/env python3
"""One-shot capacity-two barriers: physical arrivals, departures and AR replay."""
import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from ikos_runtime import runtime_command, runtime_identity
ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT), str(ROOT/'scripts')]
from validate_bmc_execution import check, digest_json
from analyzer.python.ikos.race_result import validate_race_result
from test_bmc_startup_provider import verify_source_binding
from svcomp_witness import build_witness, WitnessProjectionUnsupported

def verify(info, source, bits, ikos, case):
    cert = copy.deepcopy(info['counterexample']); program = cert.pop('program')
    cert['program_sha256'] = digest_json(program)
    assert check(program,cert)['status'] == 'ACCEPT'
    checks = 1 + verify_source_binding(program,cert,source,'barrier')
    traces = {t['id']:t for t in cert['traces']}
    departures = [(name,i) for name,t in traces.items() for i,s in enumerate(t['steps'])
                  if s.get('synthetic_event') == 'barrier_departure']
    assert len(departures) == 2
    # Keep the first post-barrier write before the second departure. Its vector
    # clock must not leak into the frozen release clock of the other thread.
    main_name=next(n for n,t in traces.items() if t['entry']=='main')
    child_name=next(n for n,t in traces.items() if t['entry']!='main')
    positions=dict(departures)
    ordered=copy.deepcopy(cert)
    chosen={t['id']:t for t in ordered['traces']}
    sequence=(chosen[main_name]['steps'][:positions[main_name]]+
              chosen[child_name]['steps'][:positions[child_name]]+
              chosen[main_name]['steps'][positions[main_name]:]+
              chosen[child_name]['steps'][positions[child_name]:])
    for clock,step in enumerate(sequence):step['clock']=str(clock)
    assert check(program,ordered)['status']=='ACCEPT',check(program,ordered)
    checks+=1
    if case in (0,10):
        # A genuine main Return must terminate the process before a later child step.
        returns=[sid for block in program['functions']['main']['blocks'].values()
                 for sid in block['statements'] if program['instructions'][sid]['kind']=='return']
        assert len(returns)==1
        exited=copy.deepcopy(ordered)
        exited_traces={t['id']:t for t in exited['traces']}
        exited_traces[main_name]['steps'].append(dict(statement=returns[0],clock='0'))
        schedule=(exited_traces[main_name]['steps'][:positions[main_name]]+
                  exited_traces[child_name]['steps'][:positions[child_name]]+
                  exited_traces[main_name]['steps'][positions[main_name]:]+
                  exited_traces[child_name]['steps'][positions[child_name]:])
        for clock,step in enumerate(schedule):step['clock']=str(clock)
        stopped=check(program,exited)
        assert stopped==dict(status='REJECT',reason='barrier execution after process exit'),stopped
        checks+=1
    def trace(c,name): return next(t for t in c['traces'] if t['id'] == name)
    def rejected(c,label):
        nonlocal checks
        result=check(program,c)
        assert result['status']=='REJECT',(label,result)
        checks+=1
    for name,i in departures:
        bad=copy.deepcopy(cert);trace(bad,name)['steps'].pop(i)
        rejected(bad,'missing departure')
        bad=copy.deepcopy(cert);trace(bad,name)['steps'][i]['encoded_result']='0'
        rejected(bad,'departure fabricates an API result')
        bad=copy.deepcopy(cert);trace(bad,name)['steps'][i]['statement']=trace(bad,name)['steps'][i+1]['statement']
        rejected(bad,'departure borrows another call')
    # An otherwise well-ordered trace cannot depart before the other arrival.
    name,i=departures[0];other,j=departures[1]
    bad=copy.deepcopy(cert)
    other_arrival=int(trace(bad,other)['steps'][j-1]['clock'])
    for n,s in enumerate(trace(bad,name)['steps']):s['clock']=str(other_arrival-len(trace(bad,name)['steps'])-1+n)
    rejected(bad,'depart before all physical arrivals')
    # A different trusted program identity still has to meet the ABI contract.
    for mutation,label in [('target','unsupported target'),('cc','call convention'),('layout','barrier extent')]:
        changed=copy.deepcopy(program)
        if mutation=='target': changed['target_triple']='aarch64-unknown-linux-gnu'
        else:
            calls=[i for i in changed['instructions'].values() if i['kind']=='call' and
                   i.get('operands',[{}])[0].get('id')=='pthread_barrier_init']
            assert calls
            if mutation=='cc':calls[0]['calling_convention']=8
            else:calls[0]['argument_layouts'][0]['size']+=1
        damaged=copy.deepcopy(cert);damaged['program_sha256']=digest_json(changed)
        result=check(changed,damaged)
        assert result['status']!='ACCEPT',(label,result)
        checks+=1
    try:
        witness=build_witness(str(source),info,'ILP32' if bits==32 else 'LP64',str(ikos))
        assert witness
    except WitnessProjectionUnsupported as exc:
        # Declared source projection limits remain explicit; never alter a path.
        print('source projection unsupported',str(exc),flush=True)
    checks+=1
    return checks

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ikos',type=Path,default=ROOT/'install/bin/ikos')
    p.add_argument('--evidence-dir',type=Path)
    p.add_argument('--bits',type=int,choices=(32,64),default=32)
    p.add_argument('--case',type=int,action='append')
    args=p.parse_args();count=0;rows=[]
    identity=runtime_identity(args.ikos)
    body=(ROOT/'analyzer/test/regression/bmc-values/barrier-execution.c').read_text()
    env={k:v for k,v in os.environ.items() if not k.startswith('IKOS_') and k not in ('PYTHONHOME','PYTHONPATH')}
    env['IKOS_BMC_RECOVER']='1'
    temporary=tempfile.TemporaryDirectory(prefix='ikos-barrier-execution-') if args.evidence_dir is None else None
    folder=args.evidence_dir or Path(temporary.name)/'evidence'
    folder.mkdir(parents=True,exist_ok=False)
    for case in (args.case if args.case is not None else range(13)):
        work=folder/('case'+str(case));work.mkdir()
        source=work/'renamed.c';source.write_text(f'#define CASE {case}\n'+body)
        report=work/'report.json';report.unlink(missing_ok=True)
        cmd=[*runtime_command(args.ikos),'--analyses=race','--concurrency=auto','--partitioning=guards',
             '--demote-race-to-unknown','--bmc-deadline-ms=20000','--bmc-solver-timeout-ms=1000',
             '-m'+str(args.bits),'--format=json','--report-file='+str(report),'-o',str(work/'result.db'),str(source)]
        result=subprocess.run(cmd,env=env,capture_output=True,text=True,timeout=45)
        (work/'stdout.txt').write_text(result.stdout);(work/'stderr.txt').write_text(result.stderr)
        assert result.returncode==0,(case,result.stdout,result.stderr)
        data=json.loads(report.read_text());meta=validate_race_result(data)
        infos=[r.get('info',{}) for r in data['reports'] if r.get('info',{}).get('counterexample')]
        if case in (0,1,10):
            assert meta['verdict']=='FALSE' and infos,(case,meta,result.stderr)
            for info in infos:count+=verify(info,source,args.bits,args.ikos,case)
        else:
            assert meta['verdict']!='FALSE' and not infos,(case,meta)
            if case in (7,9):assert meta['verdict']!='SAFE',(case,meta)
        rows.append(dict(case=case,bits=args.bits,verdict=meta['verdict'],executions=len(infos)))
        count+=1
        print('CASE',case,args.bits,meta['verdict'],'PASS',flush=True)
    assert runtime_identity(args.ikos)==identity
    (folder/'summary.json').write_text(json.dumps(dict(runtime=identity,rows=rows,assertions=count),indent=2)+'\n')
    print('PASS',count,'barrier checks')
if __name__=='__main__':main()
