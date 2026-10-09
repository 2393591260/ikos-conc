#!/usr/bin/env python3
"""Assert successful-spawn guards and failure-branch controls on both models."""
import argparse
import json
import os
from pathlib import Path
import subprocess


def main(args):
    args.output.mkdir(parents=True, exist_ok=False)
    rows=[]
    for bits in (32,64):
        for source in sorted(Path(__file__).parent.glob('*.c')):
            tag=source.stem+'-'+str(bits)
            command=[str(args.ikos.resolve()),'--analyses=race','--concurrency=auto',
                     '--demote-race-to-unknown','-m',str(bits),'--format=json',
                     '--report-file='+str(args.output/(tag+'.json')),
                     '-o',str(args.output/(tag+'.db')),str(source.resolve())]
            result=subprocess.run(command,env=dict(os.environ,IKOS_BMC_RECOVER='1',IKOS_BMC_DIAGNOSTICS='1'),
                                  capture_output=True,text=True,timeout=30)
            (args.output/(tag+'.log')).write_text(result.stdout+result.stderr)
            assert result.returncode==0,(tag,result.returncode)
            verdict=next((s for s in result.stdout.splitlines() if s.startswith('The program is')),'NONE')
            race=source.stem!='failure-branch-control'
            assert ('definitely UNSAFE' in verdict)==race,(tag,verdict)
            creates=[]
            report=json.loads((args.output/(tag+'.json')).read_text())
            for group in report['reports']:
                for candidate in group.get('info',{}).get('bmc',{}).get('candidates',[]):
                    if 'replay'not in candidate:continue
                    replay=candidate['replay'];instructions=replay['program']['instructions']
                    for trace in replay['traces']:
                        for step in trace['steps']:
                            ar=instructions[step['statement']]
                            if ar['kind']=='call' and ar['operands'][0].get('id')=='pthread_create' and 'encoded_result'in step:
                                assert step['encoded_result']=='0'
                                creates.append(dict(trace=trace['id'],statement=step['statement'],result=step['encoded_result']))
            if race:assert creates,(tag,'missing successful spawn value')
            row=dict(case=source.stem,bits=bits,verdict=verdict,creates=creates)
            rows.append(row);print(tag,verdict,flush=True)
    (args.output/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')


if __name__=='__main__':
    root=Path(__file__).resolve().parents[3]
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ikos',type=Path,default=root/'install/bin/ikos')
    p.add_argument('--output',type=Path,required=True)
    main(p.parse_args())
