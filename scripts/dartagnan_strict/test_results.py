#!/usr/bin/env python3
"""Do not confuse pinned Dat3M BOUNDED_RESULT=1 with a launcher failure."""
from calibrate import classify_result

cases = [
    ('Result: UNKNOWN\nReason: Not fully unrolled loops\n', 1, 'SOURCE_UNKNOWN'),
    ('REFERENCE_CERTIFICATE_UNSAT_AT_BOUND\nResult: UNKNOWN\n', 1, 'UNSAT_AT_BOUND'),
    ('REFERENCE_CERTIFICATE_UNSAT_AT_BOUND\nResult: PASS\n', 0, 'UNSAT_AT_BOUND'),
    ('REFERENCE_CERTIFICATE_SAT\nResult: FAIL\n', 13, 'SAT'),
    ('Result: FAIL\n', 13, 'SOURCE_RACE'),
    ('Result: FAIL\nResult: ERROR\n', 30, 'ERROR'),
    ('Result: FAIL\n', 1, 'ERROR'),
    ('REFERENCE_CERTIFICATE_SAT\nResult: UNKNOWN\n', 1, 'ERROR'),
    ('Result: UNKNOWN\n', 30, 'ERROR'),
    ('REFERENCE_CERTIFICATE_UNSUPPORTED: bad identity\n', 30, 'UNSUPPORTED'),
    ('Result: PASS\n', 0, 'OTHER'),
]
for log, returncode, expected in cases:
    actual = classify_result(log, returncode)
    assert actual == expected, (log, returncode, expected, actual)
print(f'{len(cases)} reference-result assertions passed')
