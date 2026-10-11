#!/usr/bin/env python3
"""Check lossless unsigned intrinsic lengths across pointer widths."""
import argparse,json,re,subprocess
from pathlib import Path

def main(a):
 a.output.mkdir(parents=True,exist_ok=False); rows=[]
 for bits in (32,64):
  for operation in ('memset','memcpy','memmove'):
   for name,width,value in [('wide_constant',64,'64'),('narrow_constant',8,'255'),('same_variable',bits,'%n'),('narrow_variable',8,'%n'),('wide_variable',64,'%n'),('oversize',64,'4294967296')]:
    folder=a.output/f'{operation}-{bits}-{name}';folder.mkdir()
    it=f'i{width}'; intr=f'llvm.{operation}.p0i8.'+('p0i8.' if operation!='memset' else '')+it
    args=f'i8* %dst, '+('i8 0' if operation=='memset' else 'i8* %src')+f', {it} {value}, i1 false'
    decl=f'i8*, '+('i8' if operation=='memset' else 'i8*')+f', {it}, i1'
    source=f'target datalayout = "e-p:{bits}:{bits}-i64:64-n8:16:32:64"\n'+f'declare void @{intr}({decl})\ndefine void @f(i8* %dst, i8* %src, {it} %n) {{\n call void @{intr}({args})\n ret void\n}}\n'
    path=folder/'input.ll';path.write_text(source)
    bc=folder/'input.bc';subprocess.run([str(a.llvm_as),str(path),'-o',str(bc)],check=True,capture_output=True)
    cmd=[str(a.importer),str(bc),'--show-operand-types'];p=subprocess.run(cmd,capture_output=True,text=True,timeout=15)
    (folder/'run.log').write_text(p.stdout+p.stderr)
    reject=bits==32 and name in ('wide_variable','oversize')
    if not a.observe:
     if reject:assert p.returncode!=0 and 'memory intrinsic length is not representable in size_t' in p.stderr,(cmd,p.stderr)
     else:
      assert p.returncode==0,(cmd,p.stderr)
      if name=='narrow_constant':assert f'ui{bits} 255' in p.stdout,p.stdout
      if name=='wide_constant':assert f'ui{bits} 64' in p.stdout,p.stdout
      if name=='oversize':assert 'ui64 4294967296' in p.stdout,p.stdout
      assert re.search(r'@ar\.'+operation+r'\([^,]+,[^,]+, ui'+str(bits)+r' ',p.stdout),p.stdout
      assert 'trunc ' not in p.stdout and 'sext ' not in p.stdout,p.stdout
      if name=='narrow_variable':
       assert re.search(r'zext ui8 %',p.stdout),p.stdout
       assert 'bitcast si8 %n' in p.stdout,p.stdout
    rows.append(dict(bits=bits,operation=operation,case=name,llvm_length_width=width,llvm_length_value=value,expected='UNSUPPORTED_LENGTH' if reject else 'LOSSLESS_IMPORT',command=cmd,returncode=p.returncode,diagnostics=[x for x in (p.stdout+p.stderr).splitlines() if 'ar.mem' in x or 'ext ' in x or 'error:' in x or 'Assertion' in x]))
 (a.output/'summary.json').write_text(json.dumps(rows,indent=2)+'\n');print('observations',len(rows))
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--importer',type=Path,required=True);p.add_argument('--llvm-as',type=Path,default=Path('/home/ruan/llvm-install/bin/llvm-as'));p.add_argument('--output',type=Path,required=True);p.add_argument('--observe',action='store_true');main(p.parse_args())
