#!/usr/bin/env python3
"""Run the boundary test suite and report theory-vs-code alignment."""
import subprocess, sys, os, glob
from pathlib import Path

IKOS = "/home/ruan/ikos/install/bin/ikos"
HERE = Path(__file__).parent

# manifest: filename -> theory_expected ("safe"/"race")
MANIFEST = {
 "d1-01-mutex-common-safe.c":   "safe",
 "d1-02-mutex-missing-race.c":  "race",
 "d1-03-mutex-different-race.c":"race",
 "d1-04-rwlock-rr-race.c":      "race",
 "d1-05-rwlock-rdwr-safe.c":    "safe",
 "d1-06-rwlock-ww-safe.c":      "safe",
 "d1-07-trylock-success-safe.c":"safe",
 "d1-08-trylock-ebusy-race.c":  "race",
 "d2-01-create-before-safe.c":  "safe",
 "d2-02-create-after-race.c":   "race",
 "d2-03-join-after-safe.c":     "safe",
 "d2-04-join-before-race.c":    "race",
 "d3-01-unique-self-safe.c":    "safe",
 "d3-02-two-same-fn-race.c":    "race",
 "d4-01-local-counter-safe.c":  "safe",
 "d4-02-global-counter-race.c": "race",
 "d5-01-distinct-globals-safe.c":"safe",
 "d5-02-alias-race.c":          "race",
 "d5-03-spawn-arg-escape-race.c":"race",
 "d5-04-global-ptr-escape-race.c":"race",
 "d6-01-verifier-atomic-safe.c":"safe",
 "d6-02-c11-atomic-safe.c":     "safe",
 "d7-01-condvar-mutex-safe.c": "safe",
 "d7-02-barrier-safe.c":        "safe",
 "d7-03-tls-safe.c":            "safe",
 "d8-01-struct-field-safe.c":   "safe",
 "d8-02-struct-two-fields-race.c":"race",
 "d9-01-atomic-nonatomic-mixed.c":"unknown",
 "d9-02-atomic-atomic-safe.c":  "safe",
 "d9-03-plain-race.c":          "race",
 "d10-03-global-array-index-race.c":"race",
}

def classify(out):
    if "The program is UNKNOWN" in out:
        return "unknown"
    if "potential data race" in out or "UNSAFE" in out:
        return "race"
    if "The program is SAFE" in out:
        return "safe"
    return "err"

def run_one(f):
    r = subprocess.run([IKOS, "--analyses=race", "--concurrency=auto",
                        "-o", f"/tmp/bt_{os.getpid()}.db", str(f)],
                       capture_output=True, text=True, timeout=60)
    return classify(r.stdout + r.stderr)

def main():
    rows = []
    for f in sorted(HERE.glob("*.c")):
        if f.name.startswith("_"): continue
        theory = MANIFEST[f.name]
        actual = run_one(f)
        aligned = (theory == actual)
        rows.append((f.name, theory, actual, "OK" if aligned else "GAP"))
    print(f"{'file':34s} {'theory':6s} {'code':6s} {'result'}")
    for name, th, ac, res in rows:
        print(f"{name:34s} {th:6s} {ac:6s} {res}")
    ok = sum(1 for r in rows if r[3]=="OK")
    gap = sum(1 for r in rows if r[3]=="GAP")
    print(f"\nAligned={ok}  Gap(misaligned)={gap}  total={len(rows)}")

if __name__ == "__main__":
    main()
