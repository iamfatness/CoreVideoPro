import pathlib
import importlib.util
import json
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
from types import SimpleNamespace
from camera_serving_evidence import ServeLogCapture, file_digest, judge_serving_identity, read_correlation


class CameraServingEvidenceTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        self.log, self.saved = self.root / "source.log", self.root / "saved.log"
        self.expected = self.root / "installed" / "corevideo-virtualcam.dll"
        self.expected.parent.mkdir()
        self.expected.write_bytes(b"candidate dll")
        self.sha = file_digest(self.expected)
        self.service = {"name": "FrameServer", "state": "Running", "pid": 123, "created": "creation-1"}

    def append(self, path=None, pid=123):
        with self.log.open("ab") as stream:
            stream.write(f"[16:00:00.123 pid={pid}] Serving module path={path or self.expected}\n".encode())

    def judge(self, capture, after=None):
        capture.poll()
        return judge_serving_identity(capture, self.service, after or self.service, self.expected, self.sha)

    def test_matching_actual_service_is_verified_and_old_bytes_are_excluded(self):
        self.log.write_bytes(b"old history\n")
        capture = ServeLogCapture(self.log, self.saved)
        self.append()
        result = self.judge(capture)
        self.assertTrue(result["servingIdentityVerified"], result)
        self.assertNotIn(b"old history", self.saved.read_bytes())
        self.assertEqual(result["serveLog"]["initialOffset"], 12)

    def test_transient_source_sharing_denial_captures_all_bytes_after_recovery(self):
        self.log.write_bytes(b"history\n")
        capture = ServeLogCapture(self.log, self.saved)
        self.append()
        original = pathlib.Path.open
        def busy(path, *args, **kwargs):
            if path == self.log: raise PermissionError(13, "sharing violation", str(self.log))
            return original(path, *args, **kwargs)
        with patch.object(pathlib.Path, "open", autospec=True, side_effect=busy):
            capture.poll()
            self.assertFalse(capture.metadata()["captureComplete"])
        self.append()
        result = self.judge(capture)
        self.assertTrue(result["servingIdentityVerified"], result)
        self.assertEqual(result["serveLog"]["sharingRetries"], 1)
        self.assertEqual(self.saved.read_bytes(), self.log.read_bytes()[len(b"history\n"):])

    def test_persistent_source_sharing_denial_cannot_qualify(self):
        self.log.write_bytes(b"history\n")
        capture = ServeLogCapture(self.log, self.saved)
        self.append()
        original = pathlib.Path.open
        def busy(path, *args, **kwargs):
            if path == self.log: raise PermissionError(13, "sharing violation", str(self.log))
            return original(path, *args, **kwargs)
        with patch.object(pathlib.Path, "open", autospec=True, side_effect=busy):
            capture.poll()
            self.assertFalse(capture.metadata()["captureComplete"])
            self.assertEqual(capture.offset, len(b"history\n"))

    def test_core_pid_activation_cannot_stand_in_for_frame_server(self):
        capture = ServeLogCapture(self.log, self.saved)
        self.append(pid=999)
        self.assertFalse(self.judge(capture)["servingIdentityVerified"])

    def test_stale_loaded_dll_invalidates_even_if_current_dll_also_logged(self):
        old = self.root / "old" / self.expected.name
        old.parent.mkdir(); old.write_bytes(b"old dll")
        capture = ServeLogCapture(self.log, self.saved)
        self.append(old); self.append()
        result = self.judge(capture)
        self.assertFalse(result["servingIdentityVerified"])
        self.assertEqual(len(result["observedModules"]), 2)

    def test_only_historical_matching_line_cannot_qualify(self):
        self.append()
        capture = ServeLogCapture(self.log, self.saved)
        self.assertFalse(self.judge(capture)["servingIdentityVerified"])

    def test_service_restart_or_pid_reuse_invalidates(self):
        capture = ServeLogCapture(self.log, self.saved)
        self.append()
        for changed in ({**self.service, "pid": 456}, {**self.service, "created": "creation-2"}):
            self.assertFalse(self.judge(capture, changed)["servingIdentityVerified"])

    def test_missing_log_cannot_qualify(self):
        self.assertFalse(self.judge(ServeLogCapture(self.log, self.saved))["servingIdentityVerified"])

    def test_truncation_invalidates(self):
        self.log.write_bytes(b"history history history\n")
        capture = ServeLogCapture(self.log, self.saved)
        self.log.write_bytes(b"short\n")
        self.assertFalse(self.judge(capture)["servingIdentityVerified"])
        self.assertIn("truncated", capture.errors[0])

    def test_truncate_regrow_prefix_change_invalidates(self):
        self.log.write_bytes(b"old\n")
        capture = ServeLogCapture(self.log, self.saved)
        self.log.write_bytes(b"new\n")
        self.append()
        self.assertFalse(self.judge(capture)["servingIdentityVerified"])
        self.assertIn("preceding bytes changed", capture.errors[0])

    def test_replacement_invalidates(self):
        self.log.write_bytes(b"history\n")
        capture = ServeLogCapture(self.log, self.saved)
        self.log.rename(self.root / "retired.log")
        self.append()
        self.assertFalse(self.judge(capture)["servingIdentityVerified"])
        self.assertIn("replaced", capture.errors[0])

    def test_bound_is_enforced_without_copying_excess(self):
        capture = ServeLogCapture(self.log, self.saved, limit=4)
        self.append()
        self.assertFalse(self.judge(capture)["servingIdentityVerified"])
        self.assertLessEqual(self.saved.stat().st_size, 4)

    def test_expected_candidate_change_invalidates(self):
        capture = ServeLogCapture(self.log, self.saved)
        self.append()
        self.expected.write_bytes(b"replacement")
        self.assertFalse(self.judge(capture)["servingIdentityVerified"])

    def test_partial_line_cannot_qualify(self):
        capture = ServeLogCapture(self.log, self.saved)
        self.append()
        with self.log.open("ab") as stream: stream.write(b"partial")
        self.assertFalse(self.judge(capture)["servingIdentityVerified"])

    def test_saved_evidence_rewrite_cannot_qualify(self):
        capture = ServeLogCapture(self.log, self.saved)
        self.append(); capture.poll()
        self.saved.write_bytes(b"replacement\n")
        self.assertFalse(self.judge(capture)["servingIdentityVerified"])

    def test_correlation_retains_timestamps_and_rejects_incomplete_records(self):
        path = self.root / "correlation.shm"
        values = [0x43565431, 1, 2, 100, 1, 2, 49, 51, 1000, 1100, 99, 7, 80]
        path.write_bytes(struct.pack("<IIIIQQQqqqQII", *values))
        result = read_correlation(path)
        self.assertTrue(result["observed"])
        self.assertEqual(result["deliveredAt100ns"], 1000)
        self.assertEqual(result["publishedAt100ns"], 1100)
        values[2] = 3
        path.write_bytes(struct.pack("<IIIIQQQqqqQII", *values))
        self.assertFalse(read_correlation(path)["observed"])
        self.assertFalse(read_correlation(self.root / "missing")["observed"])

    def test_real_harness_cannot_pass_perfect_pixels_with_wrong_serving_module(self):
        spec = importlib.util.spec_from_file_location("camera_reference", pathlib.Path(__file__).with_name("native-camera-continuity.py"))
        harness = importlib.util.module_from_spec(spec); spec.loader.exec_module(harness)
        core_path, receiver_path = self.root / "core.exe", self.root / "receiver.exe"
        core_path.write_bytes(b"core"); receiver_path.write_bytes(b"receiver")
        source = self.root / "CoreVideoPro" / "vcam-serve.log"
        source.parent.mkdir()
        output = self.root / "trial"
        old = self.root / "old" / self.expected.name
        old.parent.mkdir(); old.write_bytes(b"old DLL")
        class FakeCore:
            launcher_context = {"packaged": False, "status": 15700, "executable": "unpacked-test-python"}
            def __init__(self, *unused):
                source.write_text(f"[16:00:00.123 pid=123] Serving module path={old}\n")
            def sync(self, *unused):
                return {"ok": True, "snapshot": {"virtualCamera": {"enabled": True, "status": "live"}}}
            def close(self): pass
        class FakeReceiver:
            returncode = 0
            def __init__(self): self.calls = 0
            def poll(self):
                self.calls += 1
                return None if self.calls == 1 else 0
        args = ["native-camera-continuity.py", "--core", str(core_path), "--receiver", str(receiver_path),
            "--expected-camera-dll", str(self.expected), "--source-commit", "candidate", "--output", str(output), "--duration", "65"]
        verdict = SimpleNamespace(returncode=0, stdout=json.dumps({"osCameraContinuityVerified": True}), stderr="")
        with patch.object(sys, "argv", args), patch.object(harness.sys, "platform", "win32"), \
             patch.object(harness.shutil, "which", return_value="node"), patch.object(harness, "Core", FakeCore), \
             patch.object(harness, "service_identity", return_value=self.service), \
             patch.dict(harness.os.environ, {"ProgramData": str(self.root)}), \
             patch.object(harness.subprocess, "Popen", return_value=FakeReceiver()), \
             patch.object(harness.subprocess, "run", return_value=verdict), \
             patch.object(harness.time, "monotonic", side_effect=[0, 1, 32]), patch.object(harness.time, "sleep"), \
             patch.object(harness, "judge", return_value={"programBufferVerdict": "PASS"}):
            self.assertEqual(harness.main(), 1)
        report = json.loads((output / "report.json").read_text())
        self.assertEqual(report["qaLauncherContext"]["executable"], "unpacked-test-python")
        self.assertTrue(report["cameraPixels"]["osCameraContinuityVerified"])
        self.assertFalse(report["servingIdentityVerified"])
        self.assertEqual(report["result"], "INVALID")
        self.assertEqual(report["flags"]["COREVIDEO_DELIVERY_TRACE"], "1")


if __name__ == "__main__":
    unittest.main()
