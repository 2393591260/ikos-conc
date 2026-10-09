#!/usr/bin/env python3
"""Record partial fixed-memory checks for every saved IKOS candidate.

This does not classify any candidate as fully independently validated.
"""
import argparse
import concurrent.futures as futures
import hashlib
import json
from pathlib import Path
import sys
from types import SimpleNamespace
from calibrate import build, digest, events, run, PIN
from map_ikos import translate
root=Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--reports', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--export-summary', type=Path, default=root/'docs/artifacts/bmc-replay-2026-10-09/export-summary.json')
parser.add_argument('--timeout', type=int, default=60)
parser.add_argument('--bound', type=int, default=2)
parser.add_argument('--jobs', type=int, default=2)
parser.add_argument('--reference', type=Path, default=Path('/home/ruan/dartagnan-svcomp2026/dartagnan'))
parser.add_argument('--source', type=Path, default=Path('/tmp/ikos-dartagnan-source/Dat3M-'+PIN))
parser.add_argument('--javac', type=Path, default=Path('/tmp/ikos-reference-java/jdk/usr/lib/jvm/java-17-openjdk-amd64/bin/javac'))
cli=parser.parse_args()
if cli.jobs < 1 or cli.timeout < 1:parser.error('jobs and timeout must be positive')
reports=cli.reports.resolve()
out=cli.output.resolve()
out.mkdir(exist_ok=False)
args=cli
cp=build(args,out)
manifest=json.loads((reports/'results.json').read_text())
hashes={r['task']:r['input_sha256'] for r in json.loads(cli.export_summary.read_text())['observations']}
def task(row):
 folder=out/row['artifacts'];folder.mkdir()
 source=Path(row['input'])
 results=[]
 if digest(source)!=hashes[row['task']]:
  return {'task':row['task'],'status':'INPUT_CHANGED','pairs':[]}
 report_path=reports/row['artifacts']/'report.json'
 report=json.loads(report_path.read_text())
 base=run(args,cp,source,32 if row['data_model']=='ILP32' else 64,folder/'source')
 try:h,es=events(folder/'source/events.tsv')
 except FileNotFoundError:h,es=None,None
 for group_index,group in enumerate(report['reports']):
  for pair in group.get('info',{}).get('bmc',{}).get('candidates',[]):
   if not pair.get('replay'):continue
   p={'report_index':group_index,'pair_id':pair['pair_id'],'full_counterexample_validated':False}
   try:
    if es is None:raise ValueError('no reference IR: '+base['status'])
    cert,coverage=translate(pair['replay'],h,es)
    p.update(coverage)
    run_row=run(args,cp,source,pair['replay']['program']['pointer_bits'],folder/f'group{group_index}-pair{pair["pair_id"]}',cert)
    p.update(result=run_row,status=run_row['status'])
   except (ValueError,KeyError,IndexError,TypeError) as exc:p.update(status='UNMAPPED',reason=str(exc))
   results.append(p)
 result={'task':row['task'],'input':str(source),'input_sha256':digest(source),'report_sha256':digest(report_path),'reference_result':base,'pairs':results}
 (folder/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
 return result
rows=[]
with futures.ThreadPoolExecutor(max_workers=cli.jobs) as pool:
 for f in futures.as_completed([pool.submit(task,row) for row in manifest]):
  r=f.result();rows.append(r)
  print(len(rows),r['task'],','.join(p['status'] for p in r['pairs']),flush=True)
from collections import Counter
summary={'reference_commit':PIN,'reference_jar_sha256':digest(args.reference/'dartagnan/target/dartagnan.jar'),
 'cat_sha256':digest(args.reference/'cat/svcomp.cat'),'mapper_sha256':digest(root/'scripts/dartagnan_strict/map_ikos.py'),
 'hook_sha256':digest(root/'scripts/dartagnan_strict/StrictWitness.java'),'scope':'PARTIAL_FIXED_MEMORY_MODEL',
 'full_counterexample_validated':False,'bound':cli.bound,'timeout':cli.timeout,'jobs':cli.jobs,'tasks':len(rows),'pairs':sum(len(r['pairs']) for r in rows),
 'counts':dict(Counter(p['status'] for r in rows for p in r['pairs'])),
 'rows':sorted(rows,key=lambda r:r['task'])}
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps({k:v for k,v in summary.items() if k!='rows'}),flush=True)
