import contextlib
import importlib.util
import io
import json
import pathlib
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location("show_drill", pathlib.Path(__file__).parents[1] / "mac-show-drill.py")
drill = importlib.util.module_from_spec(spec)
spec.loader.exec_module(drill)
REAL_POPEN = subprocess.Popen
REAL_SLEEP = time.sleep

CHILD = """
import json,sys,time
print('[lock-hold-attribution] site=render.display-tick held_us=186117 phases_us=[program=185000] startup=1', file=sys.stderr, flush=True)
print('[render] STALL lockWait=0ms render=289ms startup=1', file=sys.stderr, flush=True)
for line in sys.stdin:
    request=json.loads(line)
    if request.get('type') == 'no-response': continue
    time.sleep(request.get('testDelay', 0))
    response = {'id': request['id'], 'ok': True}
    if request.get('nativeTimingCount'):
        response['snapshot'] = {'nativeDiagnostics': dict(logDropped=3, logTruncated=1,
            logSinkFailures=0, logQueued=2, secret='never-copy-native-snapshot')}
    print(json.dumps(response), flush=True)
    if request.get('nativeTimingCount'):
        time.sleep(.02)  # diagnostic logger can arrive after the response reader
        for index in range(request['nativeTimingCount']):
            print('[writer-timing] sojourn_us=7 flush_us=11 bytes=32', file=sys.stderr, flush=True)
"""


class CommandTiming(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = pathlib.Path(self.temp.name) / "timing.json"
        with mock.patch.object(drill.subprocess, "Popen", side_effect=lambda *a, **kw:
                               REAL_POPEN([sys.executable, "-u", "-c", CHILD], **kw)):
            self.core = drill.Core("controlled-child", evidence_path=str(self.path))
        self.core.request({"type": "ping"})  # exclude process startup from delay attribution
        deadline = time.monotonic() + 3
        while len(self.core.stderr) < 2 and time.monotonic() < deadline:
            REAL_SLEEP(.001)
        self.assertGreaterEqual(len(self.core.stderr), 2)

    def tearDown(self):
        with contextlib.redirect_stdout(io.StringIO()):
            self.core.kill()
        self.temp.cleanup()

    def test_delayed_response_remains_in_gate_and_keeps_step_identity_without_arguments(self):
        result = self.core.request({"type": "media-core-sync", "testDelay": .15,
                                   "commands": [{"type": "take", "secret": "never-report-this"}]}, step="operator-take")
        self.assertTrue(result["ok"])
        row = self.core.request_evidence[-1]
        self.assertEqual(row["step"], "operator-take")
        self.assertEqual(row["commandTypes"], ["take"])
        # Subtracting float monotonic timestamps can represent 140 ms as
        # 139.999999897 ms. Round only nanosecond-scale representation noise;
        # the independent 100 ms production gate below remains unchanged.
        self.assertGreaterEqual(round(row["readerReceivedMs"], 6), 140)
        syncs = [duration for name, duration in self.core.round_trips if name == "media-core-sync"]
        self.assertEqual(len(syncs), 1)
        self.assertGreater(syncs[0] * 1000, drill.MAX_COMMAND_P99_MS)
        self.assertAlmostEqual(row["durationMs"], syncs[0] * 1000)
        self.assertNotIn("never-report-this", json.dumps(row))

    def test_delayed_caller_does_not_disappear_from_gate(self):
        with mock.patch.object(drill.time, "sleep", side_effect=lambda _: REAL_SLEEP(.2)):
            self.core.request({"type": "media-core-sync", "testDelay": .02}, step="poll")
        row = self.core.request_evidence[-1]
        self.assertGreater(row["callerAfterReaderMs"], 100)
        self.assertGreater(self.core.round_trips[-1][1] * 1000, drill.MAX_COMMAND_P99_MS)

    def test_timeout_is_retained_and_not_reported_as_answered(self):
        before = len(self.core.round_trips)
        self.assertIsNone(self.core.request({"type": "no-response"}, timeout=.05, step="timeout"))
        row = self.core.request_evidence[-1]
        self.assertTrue(row["timedOut"])
        self.assertIsNone(row["readerReceivedMs"])
        self.assertEqual(len(self.core.round_trips), before)

    def test_bounded_evidence_reports_omissions_without_changing_gate_population(self):
        self.core.request_evidence = [self.core.request_evidence[0]] * 4096
        before = len(self.core.round_trips)
        self.core.request({"type": "media-core-sync"})
        self.assertEqual(len(self.core.request_evidence), 4096)
        self.assertEqual(self.core.evidence_truncated, 1)
        self.assertEqual(len(self.core.round_trips), before + 1)

    def test_late_native_phases_are_retained_with_bounded_omissions(self):
        # This test isolates the appendix cap from startup lines, which have
        # their own real-reader regression below.
        self.core.native_timing.clear()
        before = len(self.core.round_trips)
        self.core.request({"type": "media-core-sync", "nativeTimingCount": 600,
                           "secret": "never-report-native-arguments"})
        deadline = time.monotonic() + 3
        while len(self.core.native_timing) + self.core.native_timing_omitted < 600 and time.monotonic() < deadline:
            REAL_SLEEP(.001)
        with contextlib.redirect_stdout(io.StringIO()):
            self.core.kill()
        evidence = json.loads(self.path.read_text())
        self.assertTrue(evidence["stderrDrained"])
        self.assertEqual(len(evidence["nativeTiming"]), 512)
        self.assertEqual(evidence["nativeTimingOmitted"], 88)
        self.assertTrue(all(row["observedMs"] >= 0 and "flush_us=11" in row["line"]
                            for row in evidence["nativeTiming"]))
        self.assertNotIn("never-report-native-arguments", json.dumps(evidence))
        self.assertNotIn("never-copy-native-snapshot", json.dumps(evidence))
        self.assertEqual(evidence["requests"][-1]["nativeLog"],
                         dict(logDropped=3, logTruncated=1, logSinkFailures=0, logQueued=2))
        self.assertEqual(len(self.core.round_trips), before + 1)

    def test_render_attribution_before_first_request_is_retained(self):
        deadline = time.monotonic() + 3
        while not any('startup=1' in line and line.startswith('[render]')
                      for line in self.core.stderr) and time.monotonic() < deadline:
            REAL_SLEEP(.001)
        with contextlib.redirect_stdout(io.StringIO()):
            self.core.kill()
        evidence = json.loads(self.path.read_text())
        startup = [entry['line'] for entry in evidence['nativeTiming'] if 'startup=1' in entry['line']]
        self.assertEqual(len(startup), 2)
        self.assertTrue(any('phases_us=[program=185000]' in line for line in startup))
        self.assertTrue(any('render=289ms' in line for line in startup))
        self.assertEqual(evidence['nativeTimingOmitted'], 0)
        self.assertTrue(evidence['stderrDrained'])


if __name__ == "__main__":
    unittest.main()
