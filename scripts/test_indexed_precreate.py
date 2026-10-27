#!/usr/bin/env python3
"""Indexed pre-create ordering and disjoint cells, with default interval AI."""
import argparse
import json
import os
from pathlib import Path
import sqlite3
import tempfile
from test_default_thread_argument_windows import PREFIX, MAIN, analyze

READ = 'typedef int item;\nvoid *worker(void *argument) { int value=*(item*)argument; return 0; }\n'
WRITE = 'typedef int item;\nvoid *worker(void *argument) { *(item*)argument=7; return 0; }\n'
CALL = '    pthread_create(&ids[position],0,worker,&data[position]);'
INIT = '    data[position]=position;\n' + CALL
BASE = MAIN.replace(CALL, INIT)
CASES = [
 ('renamed-read', READ, BASE, True),
 ('worker-write', WRITE, BASE, True),
 ('long-long-cell', READ.replace('typedef int item', 'typedef long long item'), BASE, True),
 ('byte-cell', READ.replace('typedef int item', 'typedef unsigned char item'), BASE, True),
 ('unsigned-counter', READ, BASE.replace('int position=0', 'unsigned position=0'), True),
 ('after-create', READ, MAIN.replace(CALL, CALL+'\n    data[position]=position;'), False),
 ('shifted-child', READ, BASE.replace('worker,&data[position]', 'worker,&data[position+1]'), False),
 ('constant-parent-slot', READ, BASE.replace('data[position]=position;', 'data[0]=position;'), False),
 ('constant-child-slot', READ, BASE.replace('worker,&data[position]', 'worker,data'), False),
 ('overlapping-child-width', READ.replace('int value=*(item*)argument;', 'long long value=*(long long*)argument;'), BASE, False),
 ('overlapping-parent-width', READ, BASE.replace('data[position]=position;', '*(long long*)&data[position]=position;'), False),
 ('later-main-access', WRITE, BASE.replace('  return 0;', '  data[0]=9;\n  return 0;'), False),
 ('second-parent-store', READ, BASE.replace('data[position]=position;', 'data[0]=position; data[position]=position;'), False),
 ('reused-induction', READ, BASE.replace(CALL, CALL+'\n    if (__VERIFIER_nondet_int()) position--;'), False),
 ('family-reentry', READ, BASE.replace('  for (int position=0;', '  for (int round=0; round<2; round++) {\n  for (int position=0;').replace('  return 0;', '  }\n  return 0;'), False),
 ('narrowing-child', READ, BASE.replace('worker,&data[position]', 'worker,&data[(unsigned char)position]'), False),
 ('second-create', READ, BASE.replace(CALL, CALL+'\n'+CALL), False),
 ('ordinary-worker-call', WRITE, BASE.replace('  return 0;', '  worker(data);\n  return 0;'), False),
]

def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--ikos', type=Path, required=True)
 p.add_argument('--bits', type=int, choices=(32,64), required=True)
 p.add_argument('--case', action='append', dest='cases')
 p.add_argument('--evidence-dir', type=Path)
 args=p.parse_args()
 if set(args.cases or [])-{x[0] for x in CASES}:p.error('Unknown case')
 env={k:v for k,v in os.environ.items() if not k.startswith('IKOS_') and k not in ('PYTHONPATH','PYTHONHOME')}
 temporary=None
 if args.evidence_dir:
  root=args.evidence_dir;root.mkdir(parents=True,exist_ok=True)
 else:
  temporary=tempfile.TemporaryDirectory(prefix='ikos indexed precreate ');root=Path(temporary.name)
 records=[]
 for name,worker,body,positive in CASES:
  if args.cases and name not in args.cases:continue
  for mode in (['enabled','precreate-off'] if positive else ['enabled']):
   directory=root/(name+'-'+mode);directory.mkdir(exist_ok=True)
   arm=dict(env)
   if mode=='precreate-off':arm['IKOS_RACE_DISABLE_INDEXED_PRECREATE']='1'
   result,_=analyze(args,args.bits,name,worker+body,'enabled',directory,arm)
   with sqlite3.connect(directory/'result.db') as db:
    infos=[json.loads(x[0]) for x in db.execute('SELECT info FROM checks WHERE info IS NOT NULL')]
   proofs=[x for x in infos if x.get('thread_argument_contract')=='creation-family-indexed-precreate-v1']
   if positive and mode=='enabled':
    assert result['coverage']=='Checked' and result['verdict']=='SAFE' and proofs,(name,args.bits,result,proofs)
   elif positive:
    assert result['coverage']=='Checked' and result['verdict']=='UNKNOWN' and result['remaining_pairs']>0 and not proofs,(name,args.bits,result,proofs)
   else:
    assert result['verdict']=='UNKNOWN' and result['coverage']=='Checked' and result['remaining_pairs']>0,(name,args.bits,result,proofs)
    if name!='later-main-access':assert not proofs,(name,args.bits,result,proofs)
   records.append(dict(case=name,bits=args.bits,mode=mode,result=result,proofs=proofs))
   print(name,args.bits,mode,result['verdict'],len(proofs),flush=True)
 (root/'summary.json').write_text(json.dumps(dict(passed=True,analyses=len(records),records=records),indent=2)+'\n')
 print('PASS',len(records),'indexed pre-create controls')
 if temporary:temporary.cleanup()

if __name__=='__main__':main()
