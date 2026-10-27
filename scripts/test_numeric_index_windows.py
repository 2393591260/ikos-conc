#!/usr/bin/env python3
"""Numeric thread tokens: default AI windows and broken-premise controls."""
import argparse
import json
import os
from pathlib import Path
import re
import sqlite3
import subprocess
import sys

CASES=(0,1,2,3,10,11,12,13,14,15,16,17,18,20,21,22,23,24,25,26,27,28,29,30,31)
POSITIVE={0,1,2,3,15,20,22}
CONTRACT="creation-family-numeric-index-window-v1"

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ikos",type=Path,required=True)
    parser.add_argument("--output",type=Path)
    parser.add_argument("--bits",type=int,choices=(32,64),action="append")
    parser.add_argument("--case",type=int,choices=CASES,action="append")
    args=parser.parse_args()
    from ikos_runtime import runtime_identity,runtime_command,verify_runtime
    identity=runtime_identity(args.ikos)
    command=runtime_command(args.ikos)
    fixture=(Path(__file__).resolve().parents[1]/"analyzer/test/regression/bmc-values/numeric-index-windows.c").read_text()
    import tempfile
    temporary=None
    if args.output is None:
        temporary=tempfile.TemporaryDirectory(prefix="ikos numeric index windows ")
        args.output=Path(temporary.name)
    args.output.mkdir(parents=True,exist_ok=True)
    rows=[]
    for bits in args.bits or (32,64):
        for case in args.case or CASES:
            if case in (30,31) and bits!=64:
                continue
            positive=case in POSITIVE or (case==30 and bits==64)
            diagnostic=False
            modes=("on","producer-off","consumer-off") if positive else ("on",)
            for mode in modes:
                folder=args.output/f"case{case}-{bits}-{mode}"
                folder.mkdir(parents=True,exist_ok=True)
                source=folder/"renamed.c"
                source.write_text(f"#define CASE {case}\n"+fixture)
                database=folder/"result.db"
                rules=folder/"rules.jsonl"
                database.unlink(missing_ok=True)
                rules.unlink(missing_ok=True)
                env={k:v for k,v in os.environ.items()
                     if not k.startswith("IKOS_") and k not in ("PYTHONHOME","PYTHONPATH")}
                if mode!="on":
                    env["IKOS_RACE_DISABLE_NUMERIC_ARGUMENT_"+mode.split("-")[0].upper()]="1"
                env["IKOS_RACE_RULE_REPORT"]=str(rules)
                proc=subprocess.run(command+[
                    "--analyses=race","--concurrency=auto","--partitioning=guards",
                    "--demote-race-to-unknown","--bmc-deadline-ms=0",
                    f"-m{bits}","-o",str(database),str(source)],
                    env=env,text=True,capture_output=True,timeout=60)
                output=proc.stdout+proc.stderr
                (folder/"output.log").write_text(output)
                verdicts=re.findall(r"The program is (SAFE|UNKNOWN|UNSAFE|FALSE)",output)
                assert proc.returncode==0 and verdicts,(case,bits,mode,output)
                verdict=verdicts[-1]
                expected_safe=positive and mode=="on"
                with sqlite3.connect(database) as db:
                    settings=dict(db.execute("SELECT name,value FROM settings"))
                    infos=[json.loads(row[0]) for row in db.execute(
                        "SELECT info FROM checks WHERE info IS NOT NULL")]
                assert settings["machine-int-domain"]=="interval",(case,bits,settings)
                assert json.loads(settings["use-partitioning-domain"]) is True
                assert json.loads(settings["auto-boolean-partitioning"]) is True
                result=json.loads(settings["race_result"])
                proofs=[p for p in infos if p.get("thread_argument_contract")==CONTRACT]
                if diagnostic:
                    assert verdict in ("SAFE","UNKNOWN"),(case,bits,output)
                else:
                    assert (verdict=="SAFE")==expected_safe,(case,bits,mode,verdict,output)
                if positive:
                    assert result["coverage"]=="Checked",(case,bits,mode,result)
                    if expected_safe:
                        assert proofs,(case,bits,mode,infos)
                    else:
                        assert not proofs and result["remaining_pairs"]>0,(case,bits,mode,result,proofs)
                row={"case":case,"bits":bits,"mode":mode,"verdict":verdict,
                     "numeric_proofs":len(proofs),"coverage":result["coverage"],
                     "remaining_pairs":result["remaining_pairs"],
                     "diagnostic_not_acceptance":diagnostic}
                rows.append(row)
                (args.output/"summary.json").write_text(json.dumps(rows,indent=2)+"\n")
                print(json.dumps(row),flush=True)
    verify_runtime(identity)
    if temporary: temporary.cleanup()

if __name__=="__main__":
    main()
