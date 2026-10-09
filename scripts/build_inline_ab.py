#!/usr/bin/env python3
"""Build an isolated frontend differing only in NoInline removal.

Does not edit the source tree, current build archive, or installed binaries.
"""
import argparse
from pathlib import Path
import json, shlex, subprocess, shutil, hashlib
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
parser.add_argument('--upstream', type=Path, default=Path('/home/ruan/ikos-original'))
parser.add_argument('--output', type=Path, required=True)
args=parser.parse_args()
root=args.root.resolve()
out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
build=root/'build/frontend/llvm'
source=root/'frontend/llvm/src/pass/mark_internal_inline.cpp'
text=source.read_text();line='        f.removeFnAttr(Attribute::NoInline);\n'
assert text.count(line)==1
old=text.replace(line,'')
assert old==(args.upstream/'frontend/llvm/src/pass/mark_internal_inline.cpp').read_text()
cpp=out/'mark_internal_inline.cpp';cpp.write_text(old)
flags={}
for row in (build/'CMakeFiles/ikos-pp-lib.dir/flags.make').read_text().splitlines():
 if ' = ' in row:
  key,value=row.split(' = ',1);flags[key]=shlex.split(value)
obj=out/'mark_internal_inline.cpp.o'
compile=['/usr/bin/c++',*flags['CXX_DEFINES'],*flags['CXX_INCLUDES'],*flags['CXX_FLAGS'],'-c',str(cpp),'-o',str(obj)]
proc=subprocess.run(compile,capture_output=True,text=True)
(out/'compile.log').write_text(proc.stdout+proc.stderr);proc.check_returncode()
archive=out/'libikos-pp.a';shutil.copy2(build/'libikos-pp.a',archive)
subprocess.run(['ar','r',str(archive),str(obj)],check=True)
link=shlex.split((build/'CMakeFiles/ikos-pp.dir/link.txt').read_text())
link[link.index('-o')+1]=str(out/'ikos-pp-preserve-noinline')
link[link.index('libikos-pp.a')]=str(archive)
proc=subprocess.run(link,cwd=build,capture_output=True,text=True)
(out/'link.log').write_text(proc.stdout+proc.stderr);proc.check_returncode()
for name,pp in [('current',root/'install/bin/ikos-pp'),('preserve-noinline',out/'ikos-pp-preserve-noinline')]:
 wrapper=out/f'ikos-{name}'
 wrapper.write_text('#!/usr/bin/python3\nimport os,sys\n'
  +f"os.environ['LD_LIBRARY_PATH']={str(root/'install/lib')!r}+':'+{str(root/'install/llvm/lib')!r}+':'+os.environ.get('LD_LIBRARY_PATH','')\n"
  +f"sys.path.insert(0,{str(root/'install/libexec/lib/python3.12/site-packages')!r})\n"
  +'from ikos import analyzer,settings\n'
  +f'settings.ikos_pp=lambda: {str(pp)!r}\n'
  +'analyzer.main(sys.argv)\n')
 wrapper.chmod(0o755)
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
(out/'build-summary.json').write_text(json.dumps(dict(compile=compile,link=link,
 source_sha256=digest(source),modified_source_sha256=digest(cpp),
 analyzer_sha256=digest(root/'install/bin/ikos-analyzer'),
 current_pp_sha256=digest(root/'install/bin/ikos-pp'),
 preserved_pp_sha256=digest(out/'ikos-pp-preserve-noinline'),
 difference='preserve NoInline; all other passes/archive members/analyzer unchanged'),indent=2)+'\n')
print(out)
