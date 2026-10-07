import pathlib
import sys
import tempfile
import unittest

from monitor_evidence import COUNTERS, Core, judge, judge_recording, SourceAdmissionJudge, NativeCaptureInputJudge


class NativeCaptureInputTests(unittest.TestCase):
    def sample(self, count=10, **changes):
        row = dict(sourceId="capture:camera", kind="capture", hasVideo=True, health="producing",
                   width=1920, height=1080, framesIngested=count, droppedFrames=0)
        row.update(changes)
        return {"sources": [row]}

    def test_progress_only_claims_cpu_ingress(self):
        evidence = NativeCaptureInputJudge("capture:camera")
        evidence.observe(self.sample()); evidence.observe(self.sample(25))
        self.assertEqual("PASS", evidence.result()["captureInputVerdict"])
        self.assertIn("not GPU pixels", evidence.result()["captureInputScope"])

    def test_disabled_gpu_admission_cannot_hide_failed_or_frozen_capture(self):
        for final in [{"sources": []}, self.sample(), self.sample(25, health="stalled"), self.sample(25, hasVideo=False)]:
            evidence = NativeCaptureInputJudge("capture:camera")
            evidence.observe(self.sample()); evidence.observe(final)
            self.assertEqual("FAIL", evidence.result()["captureInputVerdict"])

    def test_unknown_partial_lower_quality_or_dropped_input_fails(self):
        for final in [self.sample(None), self.sample(True), self.sample(25, width=1280), self.sample(25, droppedFrames=1), self.sample(25, droppedFrames=None)]:
            evidence = NativeCaptureInputJudge("capture:camera")
            evidence.observe(self.sample()); evidence.observe(final)
            self.assertEqual("FAIL", evidence.result()["captureInputVerdict"])

    def test_explicit_portrait_reference_still_rejects_resize_and_dropped_input(self):
        evidence = NativeCaptureInputJudge("capture:camera", 1440, 2560)
        evidence.observe(self.sample(10, width=1440, height=2560))
        evidence.observe(self.sample(25, width=1440, height=2560))
        self.assertEqual("PASS", evidence.result()["captureInputVerdict"])
        self.assertEqual({"width": 1440, "height": 2560}, evidence.result()["captureInputReferenceFormat"])
        for changes in ({"width": 1280}, {"height": 1440}, {"droppedFrames": 1}):
            evidence = NativeCaptureInputJudge("capture:camera", 1440, 2560)
            evidence.observe(self.sample(10, width=1440, height=2560))
            final = {"width": 1440, "height": 2560}; final.update(changes)
            evidence.observe(self.sample(25, **final))
            self.assertEqual("FAIL", evidence.result()["captureInputVerdict"])

    def test_invalid_reference_geometry_cannot_be_inferred_or_waived(self):
        for width, height in ((None, 1080), (True, 1080), (0, 1080), (8000, 1080), (1920, 4321)):
            with self.assertRaises(ValueError):
                NativeCaptureInputJudge("capture:camera", width, height)


class SourceAdmissionTests(unittest.TestCase):
    def row(self, requested=11, actual=10, **changes):
        row = dict(sourceId="capture:screen", state="held", requestedEpoch=1, actualEpoch=1,
                   requestedFrameId=requested, actualFrameId=actual,
                   requestedCapture100ns=requested * 1000, actualCapture100ns=actual * 1000)
        row.update(changes)
        return {"programSourceAdmission": {"version": 1, "readyOnlyRequested": True, "sources": [row]}}

    def test_progressing_held_images_are_only_a_sampled_pass(self):
        evidence = SourceAdmissionJudge(["capture:screen"], True)
        evidence.observe(self.row())
        evidence.observe(self.row(12, 11))
        self.assertEqual("PASS", evidence.result()["sampledSourceAdmissionVerdict"])
        self.assertIn("not every composed frame", evidence.result()["sourceAdmissionScope"])

    def test_frozen_image_fails_despite_advancing_requested_identity(self):
        evidence = SourceAdmissionJudge(["capture:screen"], True)
        evidence.observe(self.row(12, 4))
        evidence.observe(self.row(12000, 4))
        self.assertEqual("FAIL", evidence.result()["sampledSourceAdmissionVerdict"])

    def test_missing_unknown_or_future_evidence_never_passes(self):
        for sample in ({}, self.row(actualFrameId=None), self.row(actual=12), self.row(actualEpoch=2), self.row(actualFrameId=10.5)):
            evidence = SourceAdmissionJudge(["capture:screen"], True)
            evidence.observe(sample)
            evidence.observe(self.row(12, 11))
            self.assertEqual("FAIL", evidence.result()["sampledSourceAdmissionVerdict"])

    def test_disabled_preparation_is_not_claimed_as_a_source_pass(self):
        evidence = SourceAdmissionJudge(["capture:screen"], False)
        evidence.observe({})
        self.assertEqual("NOT_REQUESTED", evidence.result()["sampledSourceAdmissionVerdict"])


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
