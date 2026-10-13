#!/usr/bin/env python3
"""Check isolated factor controls and reject unsupported FALSE evidence."""
import contextlib
import io
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import ablate_bmc as ablation


class AblationTests(unittest.TestCase):
    def test_inherited_controls_cannot_contaminate_a_pair(self):
        flags = {ablation.LEGACY_FLAG, *ablation.FACT_FLAGS.values()}
        inherited = {flag: '0' for flag in flags}
        inherited.update(IKOS_BMC_DIAGNOSTICS='1', IKOS_BMC_RECOVER='0', PATH='/bin')
        expected_by_factor = {
            'numerical': {'IKOS_BMC_DISABLE_AI_BOUNDS', 'IKOS_BMC_DISABLE_AI_NO_WRAP'},
            'bounds': {'IKOS_BMC_DISABLE_AI_BOUNDS'},
            'no-wrap': {'IKOS_BMC_DISABLE_AI_NO_WRAP'},
            'single-execution': {'IKOS_BMC_DISABLE_AI_SINGLE_EXECUTION'},
            'all': {'IKOS_BMC_DISABLE_AI_FACTS'},
        }
        for factor, expected in expected_by_factor.items():
            with self.subTest(factor=factor):
                enabled = ablation.fact_environment(inherited, factor, True)
                self.assertFalse(flags.intersection(enabled))
                self.assertNotIn('IKOS_BMC_DIAGNOSTICS', enabled)
                self.assertEqual(enabled['IKOS_BMC_RECOVER'], '1')
                self.assertEqual(enabled['PATH'], '/bin')
                disabled = ablation.fact_environment(inherited, factor, False)
                self.assertEqual(flags.intersection(disabled), expected)
                self.assertTrue(all(disabled[flag] == '1' for flag in expected))
        self.assertEqual(inherited['IKOS_BMC_RECOVER'], '0')

    def test_false_requires_nonempty_accepted_executions(self):
        self.assertIsNotNone(ablation.evidence_error('Race Found', []))
        for status in ('REJECT', 'UNSUPPORTED'):
            self.assertIsNotNone(ablation.evidence_error('Race Found', [{'status': status}]))
        self.assertIsNotNone(ablation.evidence_error('Race Found', [{}]))
        self.assertIsNone(ablation.evidence_error('Race Found', [{'status': 'ACCEPT'}]))
        self.assertIsNone(ablation.evidence_error('Unknown', []))

    def test_incomplete_or_different_outcomes_are_not_speed_comparisons(self):
        def row(enabled, verdict='Unknown', timed_out=False):
            return dict(input='example.c', repeat=0, ai_facts=enabled, verdict=verdict,
                        timed_out=timed_out, evidence_error=None, seconds=1,
                        maxrss_kib=None, metrics={})
        for other in (row(False, 'Safe'), row(False, 'Error (timeout)', True)):
            summary = ablation.compare_samples([row(True), other])
            self.assertFalse(summary['cost_comparable'])
            self.assertFalse(summary['transitions'][0]['same_verdict'])
            self.assertIsNone(summary['without_facts']['median_maxrss_kib'])
        summary = ablation.compare_samples([row(True), row(False)])
        self.assertTrue(summary['cost_comparable'])

    def test_missing_false_evidence_is_saved_and_fails_run(self):
        with tempfile.TemporaryDirectory() as temp:
            work = Path(temp)
            source = work / 'example.c'
            source.write_text('int main(void) { return 0; }\n')
            binary = work / 'analyzer'
            binary.write_bytes(b'unit-test fingerprint only')
            output = work / 'ablation'
            argv = ['ablate_bmc.py', '--input', str(source), '--ikos', str(binary),
                    '--analyzer', str(binary), '--factor', 'single-execution', '--output', str(output)]

            def spawn(command, **kwargs):
                disabled = ablation.FACT_FLAGS['single-execution'] in kwargs['env']
                text = b'The program is UNKNOWN\n' if disabled else b'The program is definitely UNSAFE\n'
                return SimpleNamespace(returncode=0, communicate=lambda timeout: (text, None))

            with patch.object(ablation.sys, 'argv', argv), \
                    patch.object(ablation.subprocess, 'Popen', side_effect=spawn), \
                    contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(ablation.main(), 1)
            summary = json.loads((output / 'summary.json').read_text())
            rows = json.loads((output / 'results.json').read_text())
            self.assertEqual(summary['repeat'], 5)
            self.assertEqual(summary['evidence_failures'], 5)
            self.assertEqual(summary['factor'], 'single-execution')
            self.assertEqual(summary['retained_facts'], ['bounds', 'no-wrap'])
            self.assertFalse(summary['comparisons'][0]['cost_comparable'])
            self.assertEqual(len(rows), 10)
            for row in rows:
                self.assertEqual(row['factor'], 'single-execution')
                expected = [] if row['ai_facts'] else ['single-execution']
                self.assertEqual(row['disabled_facts'], expected)
                self.assertEqual(ablation.FACT_FLAGS['single-execution'] in row['environment'],
                                 not row['ai_facts'])
                self.assertEqual(row['evidence_error'] is not None, row['ai_facts'])

    def test_report_failures_are_retained_and_empty_info_is_allowed(self):
        with tempfile.TemporaryDirectory() as temp:
            work = Path(temp)
            source = work / 'example.c'
            source.write_text('int main(void) { return 0; }\n')
            cases = (
                ('{broken json', b'The program is definitely UNSAFE\n', 1),
                (json.dumps({'reports': [{'info': {'confirmed_pair_id': 0, 'counterexample': {}}}]}),
                 b'The program is definitely UNSAFE\n', 1),
                (json.dumps({'reports': [{'info': None}, {}]}), b'The program is UNKNOWN\n', 0),
            )
            for index, (report, text, expected) in enumerate(cases):
                with self.subTest(case=index):
                    output = work / str(index)
                    argv = ['ablate_bmc.py', '--input', str(source), '--ikos', str(source),
                            '--analyzer', str(source), '--repeat', '1', '--output', str(output)]

                    def spawn(command, **kwargs):
                        (kwargs['cwd'] / 'report.json').write_text(report)
                        return SimpleNamespace(returncode=0, communicate=lambda timeout: (text, None))

                    with patch.object(ablation.sys, 'argv', argv), \
                            patch.object(ablation.subprocess, 'Popen', side_effect=spawn), \
                            contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                        self.assertEqual(ablation.main(), expected)
                    rows = json.loads((output / 'results.json').read_text())
                    self.assertEqual(len(rows), 2)
                    self.assertTrue(all(bool(row['evidence_error']) == bool(expected) for row in rows))
                    if expected:
                        self.assertTrue(all('malformed report/execution evidence' in row['evidence_error']
                                            for row in rows))


if __name__ == '__main__':
    unittest.main()
