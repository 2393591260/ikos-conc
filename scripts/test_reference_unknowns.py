#!/usr/bin/env python3
"""Guard against treating crashed/timed-out reference runs as confirmation."""
from reference_unknowns import classify

assert classify("Result: FAIL\nReason: SVCOMP data race found\n", 0, False) == "FAIL"
assert classify("Result: FAIL\n", 1, False) == "ERROR"
assert classify("Result: FAIL\n", -9, True) == "TIMEOUT"
assert classify("Result: UNKNOWN\n", 0, False) == "UNKNOWN"
assert classify("Result: PASS\n", 0, False) == "PASS"
assert classify("model says FAIL, result not printed", 0, False) == "ERROR"
print("reference verdict checks passed")
