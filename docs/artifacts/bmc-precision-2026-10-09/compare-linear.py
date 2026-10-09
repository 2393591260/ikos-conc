import json,os,subprocess,time
from pathlib import Path
import sys
sys.path.insert(0,'/home/ruan/ikos-conc')
import svcomp_race_test as suite
p=Path('/tmp/ikos-precision');folder=p/'linear-comparison';folder.mkdir(exist_ok=True)
env=dict(os.environ,IKOS_BMC_RECOVER='1',IKOS_BMC_DIAGNOSTICS='1',
 PYTHONPATH='/home/ruan/ikos-conc/install/libexec/lib/python3.12/site-packages',
 LD_LIBRARY_PATH='/home/ruan/ikos-conc/install/lib:/home/ruan/ikos-conc/install/llvm/lib'+(':'+os.environ['LD_LIBRARY_PATH'] if os.environ.get('LD_LIBRARY_PATH') else ''))
rows=[]
for trial in range(3):
 for name in ('qw2004-1','qw2004-2'):
  for version,binary in [('before',p/'ikos-analyzer.before-linear'),('after',Path('/home/ruan/ikos-conc/install/bin/ikos-analyzer'))]:
   f=folder/f'{name}-{trial}-{version}';f.mkdir(exist_ok=True)
   cmd=['python3',str(p/'version-launcher.py'),'--analyses=race','--concurrency=auto','--demote-race-to-unknown','-m','32','--format=json','--report-file='+str(f/'report.json'),'-o',str(f/'output.db'),'/home/ruan/sv-benchmarks-2026/c/pthread-lit/'+name+'.i']
   start=time.monotonic();proc=subprocess.run(cmd,env=dict(env,IKOS_TEST_ANALYZER=str(binary)),cwd=f,capture_output=True,text=True,timeout=30);elapsed=time.monotonic()-start
   output=proc.stdout+proc.stderr;(f/'analyzer.log').write_text(output)
   verdict=suite.classify(output,proc.returncode,False);assert proc.returncode==0,(name,version,output)
   rows.append(dict(input=name,trial=trial,version=version,binary=str(binary),seconds=elapsed,verdict=verdict,command=cmd))
   (folder/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
   for db in f.glob('output.db*'):db.unlink()
   print(name,trial,version,round(elapsed,3),verdict,flush=True)
