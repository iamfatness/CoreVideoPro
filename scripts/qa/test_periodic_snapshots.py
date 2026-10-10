import unittest
from periodic_snapshots import periodic_snapshots
from monitor_evidence import SourceAdmissionJudge


class PeriodicSnapshotTests(unittest.TestCase):
    def test_waits_before_each_observation_including_first_and_uses_final_observation(self):
        now = [0.0]
        calls = []
        def wait(seconds):
            now[0] += seconds
        def sync():
            calls.append(now[0])
            return now[0]
        samples = list(periodic_snapshots(sync, .6, clock=lambda: now[0], wait=wait))
        self.assertEqual(samples, [.25, .5, .75])
        self.assertEqual(calls, samples)
        self.assertTrue(all(b-a >= .25 for a, b in zip([0.0]+samples, samples)))

    def test_preparation_can_finish_before_advancement_is_judged(self):
        now = [0.0]
        def wait(seconds):
            now[0] += seconds
        def sync():
            # At t=0 the latest arrival is still being prepared. Holding it
            # for one frame is valid; judging that observation as a 250ms stall is not.
            frame = 10 + int(now[0]*60)
            return {"programSourceAdmission": {"version": 1, "readyOnlyRequested": True, "sources": [{"sourceId": "capture:test", "state": "ready",
                "requestedEpoch": 1, "actualEpoch": 1, "requestedFrameId": frame,
                "actualFrameId": frame, "requestedCapture100ns": frame*100, "actualCapture100ns": frame*100}]}}
        # Reproduce the old duplicate observation: a new arrival within 3.5ms,
        # while the previously completed image is still correctly held.
        early = SourceAdmissionJudge(["capture:test"], True)
        early.observe(sync())
        arrival = sync()
        arrival["programSourceAdmission"]["sources"][0].update(
            requestedFrameId=11, requestedCapture100ns=1100, state="held")
        early.observe(arrival)
        self.assertEqual(early.result()["sampledSourceAdmissionVerdict"], "FAIL")
        judge = SourceAdmissionJudge(["capture:test"], True)
        judge.observe(sync())
        for snapshot in periodic_snapshots(sync, .6, clock=lambda: now[0], wait=wait):
            judge.observe(snapshot)
        self.assertEqual(judge.result()["sampledSourceAdmissionVerdict"], "PASS")
