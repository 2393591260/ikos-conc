#!/usr/bin/env python3
"""Directed real-analyzer calloc source/extent/lifetime and replay checks."""
import argparse
import copy
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--project-root",type=Path,default=Path(__file__).resolve().parents[1])
    ap.add_argument("--ikos",type=Path)
    ap.add_argument("--fixture",type=Path)
    ap.add_argument("--bits",type=int,choices=(32,64),default=32)
    ap.add_argument("--output",type=Path)
    ap.add_argument("--diagnose",action="store_true",help="collect unmet expectations before exiting")
    args=ap.parse_args();root=args.project_root.resolve()
    sys.path.insert(0,str(root/"scripts"));sys.path.insert(0,str(root))
    from ikos_runtime import runtime_command,runtime_identity
    from analyzer.python.ikos.race_result import validate_race_result
    from validate_bmc_execution import Replay,check,digest_json
    import svcomp_witness as wrapper
    ikos=args.ikos or root/"install/bin/ikos"
    fixture=(args.fixture or root/"analyzer/test/regression/bmc-values/calloc-execution.c").read_text()
    positive=(0,1,2,3,5,6,20,21,22,24,26,27)
    negative=(7,8,9,10,11,12,13,14,15,16,17,18,19,23,25,28,29)
    env={k:v for k,v in os.environ.items() if not k.startswith("IKOS_") and k not in ("PYTHONPATH","PYTHONHOME")}
    env["IKOS_BMC_RECOVER"]="1"
    identity=runtime_identity(ikos);rows=[];failures=[];executions=mutations=projections=0
    def demand(ok,reason):
        if not ok: raise AssertionError(reason)
    with tempfile.TemporaryDirectory(prefix="ikos-calloc-") as tmp:
        work=Path(tmp)
        for wide in (False,True):
            for mode,disabled in [(m,False) for m in positive+negative]+[(0,True)]:
                source=work/"renamed.c";report=work/"report.json"
                source.write_text("#define MODE "+str(mode)+"\n"+("#define WIDE\n" if wide else "")+fixture)
                run_env=dict(env)
                if disabled:run_env["IKOS_BMC_DISABLE_CALLOC"]="1"
                report.unlink(missing_ok=True)
                row=dict(bits=args.bits,wide=wide,mode=mode,disabled=disabled)
                try:
                    proc=subprocess.run(runtime_command(ikos)+[
                        "--analyses=race","--concurrency=auto","--partitioning=guards",
                        "--demote-race-to-unknown","-m"+str(args.bits),
                        "--bmc-deadline-ms=20000","--bmc-solver-timeout-ms=1000",
                        "--format=json","--report-file="+str(report),
                        "-o",str(work/"output.db"),str(source)],env=run_env,capture_output=True,text=True,timeout=45)
                    demand(proc.returncode==0,(proc.stdout,proc.stderr))
                    data=json.loads(report.read_text());metadata=validate_race_result(data)
                    certificates=[r["info"]["counterexample"] for r in data["reports"]
                                  if r.get("info",{}).get("counterexample")]
                    row.update(verdict=metadata["verdict"],certificates=len(certificates))
                    if disabled or mode in negative:
                        demand(metadata["verdict"] in ("SAFE","UNKNOWN") and not certificates,metadata)
                        demand(metadata["analysis_complete"] and metadata["checks_complete"],metadata)
                    else:
                        demand(metadata["verdict"]=="FALSE" and certificates,metadata)
                        for original in certificates:
                            cert=copy.deepcopy(original);program=cert.pop("program");cert["program_sha256"]=digest_json(program)
                            result=check(program,cert);demand(result["status"]=="ACCEPT",result);executions+=1
                            executed={s["statement"] for t in cert["traces"] for s in t["steps"]}
                            allocs=[(sid,inst) for sid,inst in program["instructions"].items()
                                    if sid in executed and "native_calloc" in inst]
                            demand(allocs,"missing actual calloc native metadata")
                            for sid,inst in allocs:
                                native=inst["native_calloc"]
                                demand(native.get("source_operands_bound") is True and
                                       native["argument_values"]==inst["operands"][1:],"unbound operands")
                                for key,value in (("source_operands_bound",False),("matching_signature",False),
                                                  ("argument_values",[]),("name","malloc")):
                                    changed=copy.deepcopy(program);changed["instructions"][sid]["native_calloc"][key]=value
                                    damaged=copy.deepcopy(cert);damaged["program_sha256"]=digest_json(changed)
                                    demand(check(changed,damaged)["status"]=="REJECT",(sid,key));mutations+=1
                                name=inst["operands"][0]["id"]
                                locations=(
                                    ("functions",name,"parameter_types",0), ("functions",name,"parameter_types",1),
                                    ("functions",name,"return_type"), ("instructions",sid,"operands",0,"type"),
                                    ("instructions",sid,"operands",1,"type"), ("instructions",sid,"operands",2,"type"),
                                    ("instructions",sid,"result","type"),
                                    ("instructions",sid,"native_calloc","argument_values",0,"type"),
                                    ("instructions",sid,"native_calloc","argument_values",1,"type"))
                                for location in locations:
                                    for field in ("size","extent"):
                                        changed=copy.deepcopy(program);owner=changed
                                        for key in location:owner=owner[key]
                                        owner[field]=str(args.bits//8+1)
                                        damaged=copy.deepcopy(cert);damaged["program_sha256"]=digest_json(changed)
                                        demand(check(changed,damaged)["status"]=="REJECT",(location,field));mutations+=1
                            for field in ("size","extent"):
                                # Mutate every occurrence consistently so equality checks
                                # alone cannot reject before the actual native layout gate.
                                changed=copy.deepcopy(program)
                                def damage_layout(value):
                                    if isinstance(value,dict):
                                        if value.get("kind") in ("integer","pointer") and value.get("bits")==args.bits:
                                            value[field]=str(args.bits//8+1)
                                        for child in value.values():damage_layout(child)
                                    elif isinstance(value,list):
                                        for child in value:damage_layout(child)
                                damage_layout(changed)
                                damaged=copy.deepcopy(cert);damaged["program_sha256"]=digest_json(changed)
                                demand(check(changed,damaged)["status"]=="REJECT","connected "+field+" layout")
                                mutations+=1
                            traces={t["id"]:t for t in cert["traces"]}
                            heap_steps=[(t["id"],i,s) for t in cert["traces"] for i,s in enumerate(t["steps"])
                                if s.get("encoded_address",{}).get("object",{}).get("kind")=="heap"]
                            demand(heap_steps,"no concrete heap address")
                            for trace,index,step in heap_steps[:1]:
                                for key,value in (("extent","1"),("occurrence",99),("allocation","missing")):
                                    damaged=copy.deepcopy(cert)
                                    ds=next(t for t in damaged["traces"] if t["id"]==trace)["steps"][index]
                                    ds["encoded_address"]["object"][key]=value
                                    demand(check(program,damaged)["status"]=="REJECT",(trace,index,key));mutations+=1
                            for t in cert["traces"]:
                                for index,step in enumerate(t["steps"]):
                                    if program["instructions"][step["statement"]]["kind"]=="load" and "encoded_result" in step:
                                        damaged=copy.deepcopy(cert)
                                        ds=next(d for d in damaged["traces"] if d["id"]==t["id"])["steps"][index]
                                        ds["encoded_result"]=str(int(ds["encoded_result"])+1)
                                        demand(check(program,damaged)["status"]=="REJECT","corrupt loaded value");mutations+=1
                                        break
                            if mode == 27:
                                children=[t for t in cert["traces"] if t.get("creator_trace") is not None]
                                demand(children,"missing selected Create binding")
                                for child in children:
                                    parent=traces[child["creator_trace"]]
                                    creates=[i for i,s in enumerate(parent["steps"])
                                             if s["statement"] == parent["steps"][child["creator_step"]]["statement"]]
                                    # Alter dynamic occurrence, keeping the static source call unchanged.
                                    alternatives=[i for i in creates if i != child["creator_step"] and
                                        parent["steps"][i]["statement"] == parent["steps"][child["creator_step"]]["statement"]]
                                    demand(alternatives,"missing repeated static Create occurrences")
                                    if alternatives:
                                        damaged=copy.deepcopy(cert)
                                        next(t for t in damaged["traces"] if t["id"]==child["id"])["creator_step"]=alternatives[0]
                                        demand(check(program,damaged)["status"]=="REJECT","wrong dynamic Create occurrence")
                                        mutations+=1
                            info=dict(counterexample=original)
                            demand(wrapper.validate_selected_execution(info,"ILP32" if args.bits==32 else "LP64")["status"]=="ACCEPT","wrapper replay")
                            witness=wrapper.build_witness(str(source),info,"ILP32" if args.bits==32 else "LP64",str(ikos))
                            demand(witness[0]["entry_type"]=="violation_sequence","source projection");projections+=1
                except Exception as error:
                    row["failure"]=str(error);failures.append(row.copy())
                    if not args.diagnose: raise
                rows.append(row)
    demand(runtime_identity(ikos)==identity,"runtime identity changed")
    result=dict(bits=args.bits,rows=rows,failures=failures,executions=executions,mutations=mutations,
        source_projections=projections,official_witness_validation=False,runtime=identity)
    if args.output:args.output.write_text(json.dumps(result,indent=2)+"\n")
    print(json.dumps(dict(controls=len(rows),failures=len(failures),executions=executions,
                         mutations=mutations,source_projections=projections)))
    return bool(failures)
if __name__=="__main__":raise SystemExit(main())
