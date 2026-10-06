"""Bounded JSON-line driver and conservative synthetic Program evidence judge."""

import json
import math
import os
import subprocess
import threading
import time


class Core:
    """One outstanding request; no event backlog or retained response history."""

    def __init__(self, path, env, stderr_path):
        self.condition = threading.Condition()
        self.pending = None
        self.response = None
        self.closed = False
        self.n = 0
        self.events = 0
        with open(stderr_path, "wb") as stderr_file:
            self.proc = subprocess.Popen(
                [path],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=stderr_file,
                text=True,
                bufsize=1,
                env={
                    **{
                        k: v
                        for k, v in os.environ.items()
                        if not k.upper().startswith("COREVIDEO_")
                    },
                    **env,
                },
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            )
        # Popen duplicates the stderr handle; the context closes our copy.
        # stderr is redirected to disk so verbose diagnostics cannot consume RAM.
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        try:
            while True:
                # A malformed peer cannot allocate an unbounded single line.
                line = self.proc.stdout.readline(16 * 1024 * 1024 + 1)
                if not line:
                    break
                if len(line) > 16 * 1024 * 1024:
                    break
                try:
                    value = json.loads(line)
                except ValueError:
                    continue
                if not isinstance(value, dict):
                    continue
                with self.condition:
                    if value.get("id") == self.pending and self.pending is not None:
                        self.response = value
                        self.condition.notify_all()
                    else:
                        self.events += 1
        finally:
            with self.condition:
                self.closed = True
                self.condition.notify_all()

    def request(self, body, timeout=8):
        with self.condition:
            if self.pending is not None:
                raise RuntimeError("only one concurrent request is supported")
            self.n += 1
            self.pending = f"monitor-qa-{self.n}"
            self.response = None
            try:
                self.proc.stdin.write(json.dumps({**body, "id": self.pending}) + "\n")
                self.proc.stdin.flush()
                deadline = time.monotonic() + timeout
                while self.response is None and not self.closed:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError(f'{body.get("type")} timed out')
                    self.condition.wait(remaining)
                if self.response is None:
                    raise RuntimeError("core output closed before response")
                return self.response
            finally:
                self.pending = None
                self.response = None

    def sync(self, commands, elapsed):
        return self.request(
            {"type": "media-core-sync", "commands": commands, "elapsedMs": elapsed}
        )

    def close(self):
        self.proc.stdin.close()
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()  # Only the process created by this driver.
            self.proc.wait(timeout=5)
        self.reader.join(timeout=2)
        self.proc.stdout.close()


COUNTERS = {
    "programBuffer": ("underruns", "deadlineMisses", "outputSequenceGaps", "delivered"),
    "render": ("deadlineMisses", "skippedSlots", "workTotalNs", "completedSlots"),
    "monitorWorker": ("submitted", "completed", "failed", "superseded"),
    "monitorShed": ("shedTicks",),
    "audio": ("audioLostSamples",),
}


def counters(snapshot):
    result = {}
    for block, fields in COUNTERS.items():
        node = (
            snapshot[block]
            if block == "programBuffer"
            else snapshot["realtimeEvidence"][block]
        )
        for field in fields:
            value = node[field]
            if (
                isinstance(value, bool)
                or not isinstance(value, (int, float))
                or not math.isfinite(value)
                or value < 0
            ):
                raise ValueError(f"invalid {block}.{field}")
            result[f"{block}.{field}"] = value
    return result


def judge(snapshots, isolated):
    """A buffer-level pass is NOT a receiver/display/per-frame qualification."""
    if len(snapshots) < 2:
        raise ValueError("insufficient snapshots")
    rows = [counters(s) for s in snapshots]
    for previous, current in zip(rows, rows[1:]):
        if any(current[k] < previous[k] for k in previous):
            raise ValueError("counter reset during trial")
    delta = {k: rows[-1][k] - rows[0][k] for k in rows[0]}
    if delta["programBuffer.delivered"] <= 0 or delta["render.completedSlots"] <= 0:
        raise ValueError("no production progress")
    if isolated and delta["monitorWorker.completed"] <= 0:
        raise ValueError("isolated worker made no progress")
    failures = [
        key
        for key in (
            "programBuffer.underruns",
            "programBuffer.deadlineMisses",
            "programBuffer.outputSequenceGaps",
            "render.skippedSlots",
            "audio.audioLostSamples",
        )
        if delta[key] != 0
    ]
    return {
        "delta": delta,
        "programBufferVerdict": "FAIL" if failures else "PASS",
        "failedCounters": failures,
        "renderMeanWorkMs": delta["render.workTotalNs"]
        / delta["render.completedSlots"]
        / 1e6,
        "releaseQualification": "MISSING_EVIDENCE",
        "missingEvidence": [
            "per-frame output identities",
            "GPU readiness",
            "display presentation",
            "camera receiver",
            "decoded A/V",
            "real SDK and capture acquisition",
            "resource qualification",
        ],
    }


def judge_recording(first, last):
    """Recording proof counters include startup; compare only measured deltas."""
    fields = (
        "programMissingFrames",
        "encoderQueueDroppedVideoFrames",
        "encoderQueueDroppedAudioPackets",
        "failureCount",
        "programFrameCount",
    )
    before, after = first["recording"]["proof"], last["recording"]["proof"]
    delta = {}
    for field in fields:
        a, b = before[field], after[field]
        if (
            isinstance(a, bool)
            or isinstance(b, bool)
            or not isinstance(a, int)
            or not isinstance(b, int)
            or a < 0
            or b < a
        ):
            raise ValueError("invalid/reset recording counter " + field)
        delta[field] = b - a
    if delta["programFrameCount"] <= 0:
        raise ValueError("recording made no progress")
    failed = [k for k, value in delta.items() if k != "programFrameCount" and value]
    return {
        "recordingVerdict": "FAIL" if failed else "PASS",
        "recordingDelta": delta,
        "recordingFailedCounters": failed,
    }
