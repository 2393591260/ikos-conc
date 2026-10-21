#!/usr/bin/env python3
"""Property protocol must survive report filtering and reject stale evidence."""
import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from analyzer.python.ikos.race_result import validate_race_result
from svcomp_witness import first_race


def report(verdict='SAFE', obligations=(), pairs=0):
    result = dict(schema=1, policy='strict-coverage-v1', analysis_complete=True,
                  checks_complete=True, coverage='Blocked' if any(not o['contract'] for o in obligations) else 'Checked',
                  obligation_count=len(obligations), unresolved_count=sum(not o['contract'] for o in obligations),
                  safety_only_count=sum(o['safety_only'] for o in obligations), obligations=list(obligations),
                  verdict=verdict, remaining_pairs=pairs, search_performed=verdict == 'FALSE')
    return dict(race_policy=dict(schema=1, policy='strict-coverage-v1'), race_result=result, reports=[])


def confirmed_report():
    data = report('FALSE', pairs=1)
    data['reports'] = [dict(checker=17, status=2, info=dict(
        confirmed_pair_id=0, bmc_result=dict(status='CONFIRMED'),
        access_a={}, access_b={}, counterexample=dict(program={}, traces=[dict(steps=[{}])] )))]
    return data


class RaceResultTests(unittest.TestCase):
    def test_safe_and_unknown_with_no_statement_anchor(self):
        self.assertIsNotNone(validate_race_result(report()))
        obligation = dict(source='required_entry', reason='extern constructor', contract='', safety_only=False)
        data = report('UNKNOWN', [obligation])
        self.assertIsNone(first_race(data))
        data['race_result']['verdict'] = 'SAFE'
        with self.assertRaises(ValueError):
            validate_race_result(data)

    def test_safety_coverage_is_not_execution_permission(self):
        obligation = dict(source='effect', reason='unknown destination',
                          contract='coverage-bucket-ownership-v1', safety_only=True)
        self.assertIsNotNone(validate_race_result(report(obligations=[obligation])))
        data = report('UNKNOWN', [obligation], pairs=1)
        self.assertIsNone(first_race(data))
        data['race_result'].update(verdict='FALSE', search_performed=True)
        with self.assertRaises(ValueError):
            validate_race_result(data, check_rows=False)

    def test_confirmed_requires_metadata_and_nonempty_execution(self):
        data = confirmed_report()
        self.assertIsNotNone(first_race(data))
        for change in ('missing_result', 'empty_rows', 'empty_execution', 'empty_traces', 'may_row', 'unconfirmed', 'unknown'):
            broken = copy.deepcopy(data)
            if change == 'missing_result': del broken['race_result']
            elif change == 'empty_rows': broken['reports'] = []
            elif change == 'empty_execution': broken['reports'][0]['info']['counterexample'] = {}
            elif change == 'empty_traces': broken['reports'][0]['info']['counterexample']['traces'] = []
            elif change == 'may_row': broken['reports'][0]['status'] = 1
            elif change == 'unconfirmed': broken['reports'][0]['info']['bmc_result']['status'] = 'UNSUPPORTED'
            else: broken['race_result']['verdict'] = 'UNKNOWN'
            with self.subTest(change=change), self.assertRaises(ValueError):
                first_race(broken)

    def test_selector_skips_nonconfirmation_rows(self):
        data = confirmed_report()
        selected = data['reports'][0]['info']
        data['reports'].insert(0, dict(status=2, info=dict(access_a={}, access_b={})))
        self.assertIs(first_race(data), selected)

    def test_counts_and_state_corruption(self):
        for key, value in [('schema', True), ('unresolved_count', 1), ('checks_complete', False),
                           ('coverage', 'Pending'), ('remaining_pairs', 1), ('analysis_complete', 1)]:
            data = report()
            data['race_result'][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                validate_race_result(data)

    def test_forged_contract_and_orphan_confirmation(self):
        data = report()
        data['race_policy']['schema'] = True
        with self.assertRaises(ValueError):
            validate_race_result(data)
        for source, contract in [('effect', 'ar-accept'), ('thread_entry', 'coverage-readonly-heap-v1')]:
            data = report(obligations=[dict(source=source, reason='gap', contract=contract, safety_only=True)])
            with self.subTest(source=source), self.assertRaises(ValueError):
                validate_race_result(data)
        data = report('UNKNOWN', pairs=1)
        data['reports'] = [dict(checker=17, status=1, info=dict(bmc_result=dict(status='CONFIRMED')))]
        with self.assertRaises(ValueError):
            validate_race_result(data)
        data = confirmed_report()
        data['reports'][0]['info']['verdict'] = 'unknown'
        with self.assertRaises(ValueError):
            validate_race_result(data)

    def test_legacy_is_viewable_but_not_publishable(self):
        self.assertIsNone(validate_race_result({'reports': []}, required=False))
        with self.assertRaises(ValueError):
            first_race({'reports': []})
        with self.assertRaises(ValueError):
            validate_race_result({'race_policy': {}, 'reports': []}, required=False)

    def test_pending_checks_and_late_block(self):
        obligation = dict(source='completion', reason='checks disabled', contract='', safety_only=False)
        data = report('UNKNOWN', [obligation])
        data['race_result'].update(coverage='Pending', checks_complete=False)
        self.assertIsNotNone(validate_race_result(data))
        data['race_result'].update(coverage='Blocked', checks_complete=True, search_performed=True)
        self.assertIsNone(first_race(data))


if __name__ == '__main__':
    unittest.main()
