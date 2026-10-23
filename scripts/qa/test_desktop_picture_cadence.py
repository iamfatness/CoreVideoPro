import copy
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('cadence', Path(__file__).with_name('desktop-picture-cadence.py'))
cadence = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cadence)

# Test clocks tick at 60 Hz: one QPC unit per desktop update.
HZ = 60


def marker(identity):
    bits = [cell in (0, 35) if cell in (0, 1, 34, 35) else bool(identity & (1 << (cell - 2))) for cell in range(36)]
    return bytes([235 if bit else 16 for bit in bits] + [16 if bit else 235 for bit in bits])


def rows_for(identities):
    return [dict(lastPresentQpc=i, accumulatedFrames=1, protectedContent=0, pixels=marker(v).hex())
            for i, v in enumerate(identities)]


class DesktopPictureCadence(unittest.TestCase):
    def setUp(self):
        self.groups = [dict(category='markers', name=name, start=0, length=72)
                       for name in ('previewCamera', 'programCamera', 'inputCamera')]
        # A body with a colliding name must never overwrite marker evidence.
        self.groups.append(dict(category='bodies', name='inputCamera', start=0, length=16))
        self.rows = rows_for(range(8))
        self.start, self.end = 1, 6

    def run_audit(self):
        return cadence.audit(self.rows, self.groups, self.start, self.end, 72, qpc_frequency=HZ)

    def test_complete_sequence_and_body_name_collision(self):
        report = self.run_audit()
        self.assertTrue(report['sourceIdentityContinuityVerified'])
        self.assertTrue(report['studioPicturesWithinTolerance'])
        self.assertEqual(report['surfaces']['inputCamera']['invalid'], 0)
        self.assertFalse(report['releaseGate'])
        self.assertFalse(report['physicalDisplayCadenceVerified'])
        self.assertFalse(report['fullWorkloadPassed'])

    def test_known_two_picture_omission_is_detected(self):
        self.rows = [r for r in self.rows if r['lastPresentQpc'] not in (3, 4)]
        report = self.run_audit()
        self.assertTrue(report['observerCoverageComplete'])
        self.assertEqual(report['surfaces']['previewCamera']['missingSourceIdentities'], 2)
        self.assertFalse(report['sourceIdentityContinuityVerified'])
        # Two of five expected pictures is a 40 % omission: outside tolerance.
        self.assertFalse(report['studioPicturesWithinTolerance'])

    def test_beat_omission_is_observed_but_within_tolerance(self):
        # 600 pictures (10 s at 60 Hz) with one skipped identity every 20 s is
        # the mixed-refresh composition beat; it is reported, not failed.
        identities = [i for i in range(601) if i != 300]
        self.rows = rows_for(identities)
        self.start, self.end = 1, len(self.rows) - 2
        report = self.run_audit()
        surface = report['surfaces']['programCamera']
        self.assertEqual(surface['missingSourceIdentities'], 1)
        self.assertFalse(report['sourceIdentityContinuityVerified'])
        self.assertTrue(report['studioPicturesWithinTolerance'])
        self.assertTrue(surface['withinTolerance'])
        self.assertEqual(len(report['observations']), 3)
        self.assertEqual(report['errors'], [])

    def test_ten_percent_omission_fails_tolerance(self):
        # Issue 997's original defect: roughly one picture in ten never shown.
        identities = [i for i in range(601) if i % 10 != 5]
        self.rows = rows_for(identities)
        self.start, self.end = 1, len(self.rows) - 2
        report = self.run_audit()
        self.assertGreater(report['surfaces']['previewCamera']['omissionRatio'], 0.09)
        self.assertFalse(report['studioPicturesWithinTolerance'])
        self.assertTrue(any('tolerance exceeded' in e for e in report['errors']))

    def test_visible_hold_fails_tolerance_even_with_no_omission(self):
        # One identity held for seven updates (117 ms at 60 Hz) then the
        # sequence resumes without skipping: a freeze the operator can see.
        identities = list(range(0, 100)) + [100] * 7 + list(range(101, 200))
        self.rows = rows_for(identities)
        self.start, self.end = 1, len(self.rows) - 2
        report = self.run_audit()
        surface = report['surfaces']['inputCamera']
        self.assertEqual(surface['missingSourceIdentities'], 0)
        self.assertGreater(surface['longestHoldMs'], 100.0)
        self.assertFalse(surface['withinTolerance'])
        self.assertFalse(report['studioPicturesWithinTolerance'])

    def test_rate_conversion_repeat_is_within_tolerance(self):
        # A 59.94 fps camera on a 60 Hz observer repeats one picture (33 ms).
        identities = list(range(0, 100)) + [100] + list(range(100, 200))
        self.rows = rows_for(identities)
        self.start, self.end = 1, len(self.rows) - 2
        report = self.run_audit()
        self.assertTrue(report['studioPicturesWithinTolerance'])
        self.assertLess(report['surfaces']['programCamera']['longestHoldMs'], 50.0)

    def test_accumulated_updates_cannot_establish_a_pass(self):
        self.rows[3]['accumulatedFrames'] = 2
        report = self.run_audit()
        self.assertFalse(report['observerCoverageComplete'])
        self.assertFalse(report['studioPicturesWithinTolerance'])

    def test_interval_must_be_bracketed(self):
        self.rows = self.rows[2:]
        report = self.run_audit()
        self.assertFalse(report['sourceIdentityContinuityVerified'])
        self.assertFalse(report['studioPicturesWithinTolerance'])

    def test_torn_marker_cannot_hide_omission(self):
        self.rows[3]['pixels'] = bytes(72).hex()
        report = self.run_audit()
        self.assertEqual(report['surfaces']['programCamera']['invalid'], 1)
        self.assertFalse(report['sourceIdentityContinuityVerified'])
        self.assertFalse(report['studioPicturesWithinTolerance'])

    def test_reorder_and_protected_pixels_fail(self):
        self.rows[4]['pixels'] = marker(2).hex()
        self.rows[5]['protectedContent'] = 1
        report = self.run_audit()
        self.assertEqual(report['surfaces']['programCamera']['reordered'], 1)
        self.assertFalse(report['sourceIdentityContinuityVerified'])
        self.assertFalse(report['studioPicturesWithinTolerance'])

    def test_duplicate_marker_group_is_rejected(self):
        self.groups.append(copy.deepcopy(self.groups[0]))
        with self.assertRaises(ValueError):
            self.run_audit()

    def test_missing_marker_group_is_an_error(self):
        self.groups = self.groups[1:]
        report = self.run_audit()
        self.assertFalse(report['studioPicturesWithinTolerance'])
        self.assertTrue(any('previewCamera' in e for e in report['errors']))


if __name__ == '__main__':
    unittest.main()
