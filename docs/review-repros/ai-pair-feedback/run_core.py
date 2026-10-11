#!/usr/bin/env python3
"""Build and run only the independent pair-exclusion core tests."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--root', type=Path, default=Path('/home/ruan/ikos-conc'))
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
source = Path(__file__).resolve().parent
args.output.mkdir(parents=True, exist_ok=False)
command = ['c++', '-std=c++14', '-O2', '-Wall', '-Wextra', '-I',
           str(args.root / 'core/include'), '-I', str(args.root/'analyzer/include'), str(source / 'test_pair_exclusion.cpp'),
           '-o', str(args.output / 'test_pair_exclusion'), '-lz3', '-lgmpxx', '-lgmp']
subprocess.run(command, check=True)
proc = subprocess.run([str(args.output / 'test_pair_exclusion')], capture_output=True,
                      text=True, check=True, timeout=60)
rows = [json.loads(line) for line in proc.stdout.splitlines()]
(args.output / 'results.json').write_text(json.dumps(rows, indent=2)+'\n')
metadata = dict(command=command, observations=len(rows),
                hashes={str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                        for path in [args.root/'analyzer/include/ikos/analyzer/analysis/pair_exclusion.hpp',source/'test_pair_exclusion.cpp']},
                proof_scope='query and native arithmetic versus concrete tiny-address enumeration; not source-to-AR or AI fact certification',
                performance_scope='initial uncontrolled cost sample, not end-to-end speedup')
(args.output / 'metadata.json').write_text(json.dumps(metadata, indent=2)+'\n')
print('Passed',len(rows),'recorded core observations')
