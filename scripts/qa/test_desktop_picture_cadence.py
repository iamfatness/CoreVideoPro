import copy
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('cadence', Path(__file__).with_name('desktop-picture-cadence.py'))
cadence = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cadence)


def marker(identity):
    bits = [cell in (0, 35) if cell in (0, 1, 34, 35) else bool(identity & (1 << (cell - 2))) for cell in range(36)]
    return bytes([235 if bit else 16 for bit in bits] + [16 if bit else 235 for bit in bits])


class DesktopPictureCadence(unittest.TestCase):
    def setUp(self):
        self.groups = [dict(category='markers', name=name, start=0, length=72)
                       for name in ('previewCamera', 'programCamera', 'inputCamera')]
        # A body with a colliding name must never overwrite marker evidence.
        self.groups.append(dict(category='bodies', name='inputCamera', start=0, length=16))
        self.rows = [dict(lastPresentQpc=i, accumulatedFrames=1, protectedContent=0, pixels=marker(i).hex()) for i in range(8)]

    def run_audit(self):
        return cadence.audit(self.rows, self.groups, 1, 6, 72)

    def test_complete_sequence_and_body_name_collision(self):
        report = self.run_audit()
        self.assertTrue(report['sourceIdentityContinuityVerified'])
        self.assertEqual(report['surfaces']['inputCamera']['invalid'], 0)
        self.assertFalse(report['physicalDisplayCadenceVerified'])
        self.assertFalse(report['fullWorkloadPassed'])

    def test_known_two_picture_omission_is_detected(self):
        self.rows = [r for r in self.rows if r['lastPresentQpc'] not in (3, 4)]
        report = self.run_audit()
        self.assertTrue(report['observerCoverageComplete'])
        self.assertEqual(report['surfaces']['previewCamera']['missingSourceIdentities'], 2)
        self.assertFalse(report['sourceIdentityContinuityVerified'])

    def test_accumulated_updates_cannot_establish_a_pass(self):
        self.rows[3]['accumulatedFrames'] = 2
        self.assertFalse(self.run_audit()['observerCoverageComplete'])

    def test_interval_must_be_bracketed(self):
        self.rows = self.rows[2:]
        self.assertFalse(self.run_audit()['sourceIdentityContinuityVerified'])

    def test_torn_marker_cannot_hide_omission(self):
        self.rows[3]['pixels'] = bytes(72).hex()
        self.assertEqual(self.run_audit()['surfaces']['programCamera']['invalid'], 1)
        self.assertFalse(self.run_audit()['sourceIdentityContinuityVerified'])

    def test_reorder_and_protected_pixels_fail(self):
        self.rows[4]['pixels'] = marker(2).hex()
        self.rows[5]['protectedContent'] = 1
        report = self.run_audit()
        self.assertEqual(report['surfaces']['programCamera']['reordered'], 1)
        self.assertFalse(report['sourceIdentityContinuityVerified'])

    def test_duplicate_marker_group_is_rejected(self):
        self.groups.append(copy.deepcopy(self.groups[0]))
        with self.assertRaises(ValueError):
            self.run_audit()


if __name__ == '__main__':
    unittest.main()
