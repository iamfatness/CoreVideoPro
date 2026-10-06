import pathlib
import sys
import tempfile
import unittest

from monitor_evidence import COUNTERS, Core, judge, judge_recording


def snapshot():
    s = {"programBuffer": {}, "realtimeEvidence": {}}
    for block, fields in COUNTERS.items():
        node = {key: 0 for key in fields}
        if block == "programBuffer":
            s[block] = node
        else:
            s["realtimeEvidence"][block] = node
    return s


class EvidenceTests(unittest.TestCase):
    def setUp(self):
        self.first = snapshot()
        self.last = snapshot()
        self.last["programBuffer"]["delivered"] = 120
        self.last["realtimeEvidence"]["render"]["completedSlots"] = 120
        self.last["realtimeEvidence"]["render"]["workTotalNs"] = 600000000
        self.last["realtimeEvidence"]["monitorWorker"]["completed"] = 120

    def test_absorbed_render_overrun_does_not_claim_output_failure_or_qualification(
        self,
    ):
        self.last["realtimeEvidence"]["render"]["deadlineMisses"] = 10
        result = judge([self.first, self.last], True)
        self.assertEqual("PASS", result["programBufferVerdict"])
        self.assertEqual("MISSING_EVIDENCE", result["releaseQualification"])
        self.assertEqual(5, result["renderMeanWorkMs"])

    def test_single_underrun_fails_even_when_render_progress_is_high(self):
        self.last["programBuffer"]["underruns"] = 1
        self.assertEqual(
            "FAIL", judge([self.first, self.last], True)["programBufferVerdict"]
        )

    def test_audio_loss_and_skipped_slots_fail(self):
        self.last["realtimeEvidence"]["audio"]["audioLostSamples"] = 1
        self.last["realtimeEvidence"]["render"]["skippedSlots"] = 1
        self.assertEqual(2, len(judge([self.first, self.last], True)["failedCounters"]))

    def test_missing_counter_and_reset_cannot_pass(self):
        del self.last["programBuffer"]["underruns"]
        with self.assertRaises(KeyError):
            judge([self.first, self.last], True)
        self.last = snapshot()
        self.first["programBuffer"]["delivered"] = 5
        with self.assertRaises(ValueError):
            judge([self.first, self.last], False)

    def test_nonfinite_and_boolean_counters_are_invalid(self):
        for value in [float("nan"), float("inf"), True]:
            self.last["programBuffer"]["delivered"] = value
            with self.assertRaises(ValueError):
                judge([self.first, self.last], False)

    def test_no_worker_progress_invalidates_isolated_evidence(self):
        self.last["realtimeEvidence"]["monitorWorker"]["completed"] = 0
        with self.assertRaises(ValueError):
            judge([self.first, self.last], True)

    def test_recorder_loss_fails_independently_of_buffer(self):
        proof = {
            "programMissingFrames": 14,
            "encoderQueueDroppedVideoFrames": 0,
            "encoderQueueDroppedAudioPackets": 0,
            "failureCount": 0,
            "programFrameCount": 300,
        }
        first = {"recording": {"proof": dict(proof)}}
        proof.update(programMissingFrames=15, programFrameCount=420)
        last = {"recording": {"proof": proof}}
        result = judge_recording(first, last)
        self.assertEqual("FAIL", result["recordingVerdict"])
        self.assertEqual(1, result["recordingDelta"]["programMissingFrames"])
        proof["programMissingFrames"] = 13
        with self.assertRaises(ValueError):
            judge_recording(first, last)

    def test_startup_recording_loss_is_reported_without_charging_measurement(self):
        proof = {
            "programMissingFrames": 14,
            "encoderQueueDroppedVideoFrames": 0,
            "encoderQueueDroppedAudioPackets": 0,
            "failureCount": 0,
            "programFrameCount": 300,
        }
        first = {"recording": {"proof": dict(proof)}}
        proof["programFrameCount"] = 420
        self.assertEqual(
            "PASS",
            judge_recording(first, {"recording": {"proof": proof}})["recordingVerdict"],
        )


class DriverTests(unittest.TestCase):
    def test_real_child_event_flood_timeout_recovery_and_owned_cleanup(self):
        # Use the current interpreter with a tiny JSON-line peer. No core/GPU required.
        with tempfile.TemporaryDirectory() as directory:
            peer = pathlib.Path(directory) / "peer.py"
            peer.write_text("""import json,sys
for line in sys.stdin:
    m=json.loads(line)
    for i in range(200): print(json.dumps({'type':'event','n':i}))
    if m['type'] != 'timeout': print(json.dumps({'id':m['id'],'ok':True}))
    sys.stdout.flush()
""")
            # Core accepts an executable only; use a wrapper argument for the test peer.
            import unittest.mock
            import subprocess

            original = subprocess.Popen
            with unittest.mock.patch(
                "monitor_evidence.subprocess.Popen",
                side_effect=lambda args, **kw: original(
                    [sys.executable, str(peer)], **kw
                ),
            ):
                core = Core(sys.executable, {}, pathlib.Path(directory) / "stderr.log")
            try:
                for _ in range(10):
                    self.assertTrue(core.request({"type": "echo"})["ok"])
                    self.assertIsNone(core.response)
                    self.assertIsNone(core.pending)
                with self.assertRaises(TimeoutError):
                    core.request({"type": "timeout"}, timeout=0.1)
                self.assertTrue(core.request({"type": "echo"})["ok"])
                self.assertGreaterEqual(core.events, 2400)
            finally:
                core.close()
            self.assertIsNotNone(core.proc.poll())
            self.assertFalse(core.reader.is_alive())


if __name__ == "__main__":
    unittest.main()
