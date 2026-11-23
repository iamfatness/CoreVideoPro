#!/usr/bin/env python3
"""mac-show-drill — a headless SHOW rehearsal against the real media core.

Why this exists: the mac shell is ~6k lines of Swift driving a well-tested
core, and every regression so far (RPC floods, dead multiview tiles, preview
mirroring, lock saturation) was found by the owner in a live session because
nothing else was looking. This drives the same wire traffic the shell does,
through a whole show, and FAILS on the things that actually broke:

  * any request that times out (the class that killed joins and scene syncs)
  * coreMutex holds over budget (the class that starved every command)
  * a multiview wall without its PGM/PVW cells
  * a scene that never reaches program/preview
  * recording that reports live but writes nothing

Usage:  python3 scripts/mac-show-drill.py [--seconds 40] [--core PATH]
Exit 0 = the rehearsal passed.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Cross-platform: the drill gates SHARED core code, so it must run on every
# platform that ships it (a macOS-motivated change in MediaCore.cpp would
# otherwise silently degrade the Windows multiview, and vice versa). Windows
# builds into native/build-dev with .exe suffixes; macOS into native/build-metal.
_EXE = ".exe" if os.name == "nt" else ""
_BUILD_DIR = os.environ.get("COREVIDEO_BUILD_DIR") or os.path.join(
    REPO, "native", "build-dev" if os.name == "nt" else "build-metal")
DEFAULT_CORE = os.path.join(_BUILD_DIR, "corevideo-native" + _EXE)
FAKE_ENGINE = os.environ.get("COREVIDEO_FAKE_ENGINE_PATH") or os.path.join(
    _BUILD_DIR, "corevideo-zoom-engine-fake" + _EXE)

# The core's own budget for a render tick (LockHoldGuardrail::kRenderTickBudgetUs
# is half a frame). A show that exceeds it on most ticks starves the RPC queue.
MAX_OVER_BUDGET_RATIO = 0.35

# Average ms the per-tick Zoom engine tap may cost under --load. It is a
# shared_ptr handoff by design, so this is generous; it exists to catch a
# reintroduced per-frame copy, which costs ~7ms at 10x1080p.
MAX_ENGINE_TAP_MS = 2.0

# Professional-switcher spec, not "close enough". A production switcher is judged
# on whether it holds the CONFIGURED output rate with ZERO frames dropped to
# render lag — vMix/OBS-class tools surface dropped-frame counts and any sustained
# deficit is treated as a fault to fix, not a tolerance. A 60.0 average can still
# hide judder, so fps alone is not the gate: dropped frames are.
#   * 59.5 fps floor  = holding 60 (allows measurement edge, not a lost frame)
#   * 0 dropped       = no frame ran past 1.5x the 16.67ms budget
#   * 10ms hold       = ~60% of budget, leaving headroom for transients
# Source->program latency. A hardware switcher (ATEM class) processes in ~1 frame;
# software switchers are typically a few. Fail loudly on regressions (an 8ms
# ingest poll put p99 at ~20ms AND silently dropped ~30% of decoded frames before
# the compositor ever saw them).
#
# These budgets were RAISED by one frame on 2026-08-06 — not to make a red gate
# green, but because the architecture deliberately changed underneath them: the
# ingest path now runs a frame synchronizer (one-frame cushion, owner decision)
# so a jittery source cannot drop frames or repeat them. That frame is spent on
# purpose and shows up here by construction, so the old p50 <= 1 frame budget was
# measuring an architecture that no longer exists. Everything ABOVE the cushion is
# still real latency and still gated. If frame sync is ever disabled by default,
# put these back to 16.7/33.3.
# A Take rides a media-core-sync round trip. Anything the operator can feel as
# lag between pressing Take and program changing is a defect for a switcher.
# The muxed file must actually carry the configured rate. 0.95 allows startup
# and finalisation edges, not a whole 10fps of missing pictures.
MIN_RECORDED_FPS_RATIO = 0.95
MAX_COMMAND_P99_MS = 100.0
#
# RAISED AGAIN on 2026-10-01, for the same kind of reason, and this time the budget is
# DERIVED so it cannot go stale the same way. Since the #579 lip-sync fix (0c91afd0,
# 2026-09-21) Zoom video is deliberately held for kZoomVideoReserve (20 ms x
# kZoomAudioPrimeTicks = 60 ms, native/src/modules/ZoomPlayoutTiming.h) so it plays out
# with its primed audio. That hold replaced the one-frame cushion above. The drill kept
# its 33.3/50 ms budget and FAILED on every build for ten days (p50 65-73 ms measured on
# the 09-26, 09-28, 09-30 and 10-01 betas), which taught everyone to read the line as
# noise. What is gated now is the latency ABOVE the deliberate hold: a frame waits out the
# reserve and then the next render tick, so p50 should sit about half a frame over the
# reserve and p99 about one frame over. One extra frame of real latency fails both.
def _zoom_video_reserve_ms():
    header = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "native", "src",
                          "modules", "ZoomPlayoutTiming.h")
    try:
        with open(header, "r", encoding="utf-8") as handle:
            match = re.search(r"kZoomAudioPrimeTicks\s*=\s*(\d+)", handle.read())
        if match:
            return 20.0 * int(match.group(1))
    except OSError:
        pass
    return 60.0  # the value when this was written; the header is the source of truth


ZOOM_VIDEO_RESERVE_MS = _zoom_video_reserve_ms()
MAX_LATENCY_P50_MS = ZOOM_VIDEO_RESERVE_MS + 20.0   # reserve + ~1 frame at 60p
MAX_LATENCY_P99_MS = ZOOM_VIDEO_RESERVE_MS + 33.3   # reserve + 2 frames at 60p
# Fraction of DECODED frames that must actually reach the compositor.
# CAVEAT, learned the hard way: this counts distinct frameIds the render thread
# fetched, so it CANNOT see the judder the frame synchronizer exists to prevent.
# A frame destroyed unseen because two landed in one render interval still reads
# as delivered. Measured with COREVIDEO_FRAME_SYNC=0 this metric reports 100%
# while 6-10% of frames are doubled or dropped. Never read a green delivery
# number as "motion is smooth". This is the
# sharpest regression signal in the whole drill: with an 8ms ingest poll, ~30% of
# decoded frames were overwritten before the render thread fetched them — the
# operator silently loses a third of every source's motion while fps still reads
# a healthy 60. Latency percentiles barely moved; delivery collapsed.
MIN_FRAME_DELIVERY = 0.95
TARGET_OUTPUT_FPS = 60.0
MIN_LOAD_FPS = 59.5
MAX_LOAD_RENDER_MS = 10.0
MAX_DROPPED_FRAMES = 0
NATIVE_TIMING_PREFIXES = ("[request-timing]", "[response-timing]", "[writer-timing]",
                          "[req]", "[cmd]", "[response]", "[writer]", "[command-lock]",
                          "[render]", "[lock-guardrail]", "[lock-hold-attribution]")


class Core:
    def __init__(self, path, env=None, evidence_path=None):
        self.proc = subprocess.Popen(
            [path], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, bufsize=1,
            env={**os.environ, **(env or {})})
        self.responses = {}
        self.response_times = {}
        self.evidence_path = evidence_path
        self.started_at = time.monotonic()
        self.request_evidence = []
        self.evidence_truncated = 0
        self.events = []
        self.stderr = []
        self.native_timing = []
        self.native_timing_omitted = 0
        self.next_id = 1
        self.round_trips = []  # (type, seconds) per answered request
        self.stdout_reader = threading.Thread(target=self._read_stdout, daemon=True)
        self.stdout_reader.start()
        self.stderr_reader = threading.Thread(target=self._read_stderr, daemon=True)
        self.stderr_reader.start()

    def _read_stdout(self):
        for line in self.proc.stdout:
            received_at = time.monotonic()
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue
            if obj.get("id"):
                self.response_times[obj["id"]] = (received_at, time.monotonic())
                self.responses[obj["id"]] = obj
            elif obj.get("type"):
                self.events.append(obj)

    def _read_stderr(self):
        for line in self.proc.stderr:
            self.stderr.append(line.rstrip())
            if line.startswith(NATIVE_TIMING_PREFIXES):
                if len(self.native_timing) < 512:
                    self.native_timing.append({"observedMs": (time.monotonic() - self.started_at) * 1000,
                                               "line": line.rstrip()})
                else:
                    self.native_timing_omitted += 1

    def request(self, body, timeout=6.0, step=None):
        rid = f"drill-{self.next_id}"
        self.next_id += 1
        body = dict(body, id=rid)
        started = time.monotonic()
        diagnostic_start = len(self.stderr)
        self.proc.stdin.write(json.dumps(body, separators=(",", ":")) + "\n")
        self.proc.stdin.flush()
        deadline = started + timeout
        # 1ms poll: this now measures OPERATOR RESPONSIVENESS (the round trip a
        # Take rides), so a 10ms poll would be most of the number being measured.
        while time.monotonic() < deadline:
            if rid in self.responses:
                finished = time.monotonic()
                self.round_trips.append((body.get("type", "?"), finished - started))
                self._observe_request(rid, body, step, started, finished, diagnostic_start, False)
                return self.responses[rid]
            time.sleep(0.001)
        self._observe_request(rid, body, step, started, time.monotonic(), diagnostic_start, True)
        return None

    def _observe_request(self, rid, body, step, started, finished, diagnostic_start, timed_out):
        if len(self.request_evidence) >= 4096:
            self.evidence_truncated += 1
            return
        reader = self.response_times.get(rid)
        diagnostics = [line for line in self.stderr[diagnostic_start:]
                       if line.startswith(("[render]", "[lock-guardrail]", "[video-out]") + NATIVE_TIMING_PREFIXES)]
        snapshot = self.responses.get(rid, {}).get("snapshot", {})
        native_log = snapshot.get("nativeDiagnostics") if isinstance(snapshot, dict) else None
        log_keys = ("logDropped", "logTruncated", "logSinkFailures", "logQueued")
        native_log = {key: native_log.get(key) for key in log_keys} if isinstance(native_log, dict) else None
        self.request_evidence.append({
            "id": rid, "type": body.get("type", "?"), "step": step,
            "commandTypes": [command.get("type", "?") for command in body.get("commands", [])],
            "startMs": (started - self.started_at) * 1000,
            "durationMs": (finished - started) * 1000, "timedOut": timed_out,
            "readerReceivedMs": (reader[0] - started) * 1000 if reader else None,
            "readerParseMs": (reader[1] - reader[0]) * 1000 if reader else None,
            "callerAfterReaderMs": (finished - reader[1]) * 1000 if reader else None,
            "diagnostics": diagnostics[-12:], "diagnosticsOmitted": max(0, len(diagnostics) - 12),
            "nativeLog": native_log,
        })

    def sync(self, commands, elapsed_ms, step=None):
        return self.request({"type": "media-core-sync", "elapsedMs": elapsed_ms,
                             "commands": commands}, step=step)

    def kill(self):
        self.proc.kill()
        self.proc.wait(timeout=10)
        self.stdout_reader.join(timeout=1)
        self.stderr_reader.join(timeout=1)
        self.proc.stdin.close()
        # Do not close a pipe underneath a still-running reader or wait forever
        # on its stream lock. Preserve an incomplete-drain result instead.
        if not self.stdout_reader.is_alive():
            self.proc.stdout.close()
        if not self.stderr_reader.is_alive():
            self.proc.stderr.close()
        if self.evidence_path:
            os.makedirs(os.path.dirname(os.path.abspath(self.evidence_path)), exist_ok=True)
            with open(self.evidence_path, "w", encoding="utf-8") as output:
                json.dump({"clock": "monotonic", "commandP99LimitMs": MAX_COMMAND_P99_MS,
                           "requestsOmitted": self.evidence_truncated,
                           "requests": self.request_evidence,
                           "nativeTiming": self.native_timing,
                           "nativeTimingOmitted": self.native_timing_omitted,
                           "stderrDrained": not self.stderr_reader.is_alive(),
                           "stdoutDrained": not self.stdout_reader.is_alive(),
                           "scope": "Caller/reader timing plus opt-in native phase durations; observedMs is stderr receipt time, not a native timestamp or request correlation; no command arguments"}, output, indent=2)
            print(f"Command timing evidence: {self.evidence_path} ({self.evidence_truncated} omitted)")
        for request in sorted(self.request_evidence, key=lambda row: row["durationMs"], reverse=True)[:5]:
            print("Command timing: " + json.dumps(request, separators=(",", ":")))


def route(route_id, index=0, capture=None):
    entry = {"routeId": route_id, "mode": "active-speaker", "audioRole": "mix",
             "fitMode": "fill", "opacity": 1.0, "zIndex": index,
             "rect": {"x": 0, "y": 0, "width": 1, "height": 1}}
    if capture:
        entry["mode"] = "capture-input"
        entry["captureDeviceId"] = capture
    return entry


def load_env(sources):
    """Drive the REAL ingest path under a full production wall.

    The fake Zoom engine emits N animated 1080p I420 participants over the same
    IPC the real engine uses, so the core does genuine per-source ingest, texture
    upload and compositing — the only way to reproduce the render-tick cost the
    owner sees on a live show (the product targets up to 10 x 1080p feeds).
    """
    return {
        "COREVIDEO_ZOOM_ENGINE_PATH": FAKE_ENGINE,
        "COREVIDEO_FAKE_ENGINE_PARTICIPANTS": str(sources),
        # 1080p by default; overridable so a run can compare buffer sizes.
        "COREVIDEO_FAKE_ENGINE_RES": os.environ.get("COREVIDEO_FAKE_ENGINE_RES", "2"),
        # Zoom delivers up to 1080p60 and the product targets it, so the drill
        # drives 60 — a 30fps rig exercises only half the real ingest rate.
        "COREVIDEO_FAKE_ENGINE_FPS": os.environ.get("COREVIDEO_FAKE_ENGINE_FPS", "60"),
        "COREVIDEO_FAKE_NO_CHURN": "1",     # steady roster: perf, not churn
    }


def _counter(node, *path):
    """Read one numeric counter out of a snapshot, or None if it is absent."""
    for key in path:
        if not isinstance(node, dict):
            return None
        node = node.get(key)
    return node if isinstance(node, (int, float)) and not isinstance(node, bool) else None


# Deltas over the record window, per pipeline stage. Each entry is
# (label, snapshot path, "rate" per second or "count" over the window).
_STAGE_COUNTERS = (
    ("compositor render", ("realtimeEvidence", "render", "completedSlots"), "rate"),
    ("video-out tick", ("realtimeEvidence", "videoOutput", "completedTicks"), "rate"),
    ("audio worker tick", ("realtimeEvidence", "audio", "completedTicks"), "rate"),
    ("encoder program-video written", ("encoderEvidence", "programVideoWritten"), "rate"),
    ("render skipped slots", ("realtimeEvidence", "render", "skippedSlots"), "count"),
    ("render deadline misses", ("realtimeEvidence", "render", "deadlineMisses"), "count"),
    ("encoder dropped video", ("encoderEvidence", "droppedVideo"), "count"),
    ("program-buffer underruns", ("programBuffer", "underruns"), "count"),
    ("program-buffer gpuNotReady", ("programBuffer", "gpuNotReady"), "count"),
)


def stage_evidence(before, after, seconds):
    """What each stage actually did over the record window.

    Returns an ordered list of (label, formatted value) so a reader can locate the
    stage that lost the frames instead of being handed one asserted cause. A
    counter missing from either end is reported as such rather than guessed at:
    an absent number is not a zero.
    """
    rows = []
    for label, path, kind in _STAGE_COUNTERS:
        a, b = _counter(before, *path), _counter(after, *path)
        if a is None or b is None:
            rows.append((label, "unavailable"))
        elif b < a:
            rows.append((label, "counter reset (%g -> %g)" % (a, b)))
        elif kind == "rate":
            rows.append((label, "%.1f/s (%g over %.1fs)" % (
                (b - a) / seconds if seconds > 0 else 0.0, b - a, seconds)))
        else:
            rows.append((label, "%g" % (b - a)))
    return rows


# ── --media: a clip on Program and a looping video background ────────────────
# The media decoder copies every decoded frame into CPU BGRA (8.3 MB at 1080p)
# and the compositor uploads it again, so a clip on air is the load the plain
# drill never applied. The fixtures are generated once and cached by name.
MEDIA_CLIP_ID, MEDIA_BG_ID = "drill-clip", "drill-bg"
MEDIA_CLIP_FPS, MEDIA_BG_FPS = 60.0, 30.0
MIN_MEDIA_RATE_RATIO = 0.95


def find_ffmpeg(explicit):
    """--ffmpeg <dir-or-exe>, else COREVIDEO_FFMPEG_BIN_DIR, else PATH (the
    scripts/qa/media-take-ab.py order). None when there is no ffmpeg."""
    exe = "ffmpeg" + _EXE
    candidates = []
    if explicit:
        candidates.append(explicit if os.path.isfile(explicit) else os.path.join(explicit, exe))
    if os.environ.get("COREVIDEO_FFMPEG_BIN_DIR"):
        candidates.append(os.path.join(os.environ["COREVIDEO_FFMPEG_BIN_DIR"], exe))
    if shutil.which("ffmpeg"):
        candidates.append(shutil.which("ffmpeg"))
    return next((c for c in candidates if c and os.path.isfile(c)), None)


def media_fixtures(ffmpeg, folder):
    """(clip path, background path), generating whichever is missing."""
    os.makedirs(folder, exist_ok=True)
    clip = os.path.join(folder, "media-clip-1080p60.mp4")
    background = os.path.join(folder, "media-bg-1080p30.mp4")
    # 70 s outlasts every phase of the drill, so the clip is live at both ends.
    jobs = (
        (clip, ["-f", "lavfi", "-i", "testsrc2=s=1920x1080:r=60", "-f", "lavfi", "-i",
                "sine=f=440:r=48000", "-t", "70", "-c:v", "libx264", "-preset", "veryfast",
                "-pix_fmt", "yuv420p", "-c:a", "aac", "-ar", "48000", "-ac", "2"]),
        (background, ["-f", "lavfi", "-i", "testsrc2=s=1920x1080:r=30", "-t", "10", "-c:v", "libx264",
                      "-preset", "veryfast", "-pix_fmt", "yuv420p", "-an"]),
    )
    for path, arguments in jobs:
        if os.path.exists(path) and os.path.getsize(path) > 1024:
            continue
        partial = path + ".partial.mp4"
        result = subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-y"] + arguments + [partial],
                                capture_output=True, text=True)
        if result.returncode != 0 or not os.path.exists(partial):
            raise RuntimeError("ffmpeg could not write %s: %s" % (path, (result.stderr or "").strip()[:200]))
        os.replace(partial, path)
    return clip, background


def media_rows(snapshot):
    """mediaSources rows keyed by mediaAssetId, or None when the node is absent."""
    rows = (snapshot or {}).get("mediaSources")
    if not isinstance(rows, list):
        return None
    return {row.get("mediaAssetId"): row for row in rows if isinstance(row, dict)}


def media_report(first, last, seconds):
    """The 'Media sources (steady window)' block and its gates.

    `first` / `last` are the snapshots at the two ends of the operator-traffic
    window. Returns (lines to print, failures). Every failure names its numbers.
    """
    lines, failures = [], []
    before, after = media_rows(first), media_rows(last)
    if before is None or after is None:
        return lines, ["core publishes no mediaSources (the node is unconditional)"]
    lines.append("Media sources (steady window, %.1fs):" % seconds)
    measured = {}
    for asset_id, expected_fps in ((MEDIA_CLIP_ID, MEDIA_CLIP_FPS), (MEDIA_BG_ID, MEDIA_BG_FPS)):
        a, b = before.get(asset_id), after.get(asset_id)
        if a is None or b is None:
            failures.append("%s has no mediaSources row (start=%s end=%s)" % (
                asset_id, "present" if a else "absent", "present" if b else "absent"))
            continue
        delta = {key: (b.get(key) or 0) - (a.get(key) or 0)
                 for key in ("decodedVideoFrames", "presentedVideoFrames", "decoderRestarts")}
        rate = {key: delta[key] / seconds if seconds > 0 else 0.0
                for key in ("decodedVideoFrames", "presentedVideoFrames")}
        measured[asset_id] = (a, b, delta, rate, expected_fps)
        lines.append(
            "  %s: state %s -> %s, decoded +%d (%.1f/s), presented +%d (%.1f/s), restarts +%d, "
            "videoQueued %s" % (asset_id, a.get("state"), b.get("state"), delta["decodedVideoFrames"],
                                rate["decodedVideoFrames"], delta["presentedVideoFrames"],
                                rate["presentedVideoFrames"], delta["decoderRestarts"], b.get("videoQueued")))
    if MEDIA_CLIP_ID in measured:
        a, b, delta, rate, fps = measured[MEDIA_CLIP_ID]
        if a.get("state") != "live" or b.get("state") != "live":
            failures.append("%s is not live at both ends of the window (start=%s end=%s)" % (
                MEDIA_CLIP_ID, a.get("state"), b.get("state")))
        for key in ("presentedVideoFrames", "decodedVideoFrames"):
            if rate[key] < MIN_MEDIA_RATE_RATIO * fps:
                failures.append("%s %s ran at %.1f/s, below %.1f/s (%.0f%% of its %g fps)" % (
                    MEDIA_CLIP_ID, key, rate[key], MIN_MEDIA_RATE_RATIO * fps, MIN_MEDIA_RATE_RATIO * 100, fps))
    if MEDIA_BG_ID in measured:
        a, b, delta, rate, fps = measured[MEDIA_BG_ID]
        if rate["presentedVideoFrames"] < MIN_MEDIA_RATE_RATIO * fps:
            failures.append("%s presentedVideoFrames ran at %.1f/s, below %.1f/s (%.0f%% of its %g fps)" % (
                MEDIA_BG_ID, rate["presentedVideoFrames"], MIN_MEDIA_RATE_RATIO * fps,
                MIN_MEDIA_RATE_RATIO * 100, fps))
    for asset_id, (a, b, delta, rate, fps) in measured.items():
        if delta["decoderRestarts"] != 0:
            failures.append("%s decoder restarted %d time(s) in the window" % (asset_id, delta["decoderRestarts"]))
    return lines, failures


def tiles_wall(scene_id, members, style_name=None):
    """The `tiles` node the Mac/Windows shell sends for a CoreVideo Tiles scene
    (MediaCoreCommandBuilder.SerializeTilesLayer keys and defaults verbatim)."""
    style = {"tileAspect": "16:9", "customAspectRatio": 1.7777777777777777,
             "gutterPercent": 0.741, "marginPercent": 0.741, "backgroundColor": "#000000",
             "backgroundSourceId": "", "borderShape": "square", "borderColor": "#000000",
             "borderThickness": 0, "cornerRadius": 16, "glowColor": "#FFFFFF", "glowSize": 0,
             "glowIntensity": 100, "glowSoftness": 0, "animateLayout": False,
             "animationDurationMs": 350, "fillMode": "auto", "nameEachTile": False}
    if style_name == "rounded-glow":
        style.update({"borderShape": "rounded", "cornerRadius": 24, "borderThickness": 2,
                      "borderColor": "#657D87", "glowSize": 12, "glowColor": "#8AE0CA"})
    return {"layerId": f"tiles:{scene_id}", "order": 0,
            "rect": {"x": 0, "y": 0, "w": 1, "h": 1}, "opacity": 1,
            "transparentBackground": False, "members": list(members), "overrides": {},
            "style": style}


def tiles_verdict(snapshot, load):
    """FAIL unless the run actually drew a full wall: a drill that drew no wall
    can never pass (plan Task 4 step 3)."""
    node = (snapshot or {}).get("tiles")
    if not isinstance(node, dict):
        return "tiles: the snapshot has no tiles node (no wall was drawn)"
    members = node.get("members") or []
    held = node.get("heldCount")
    if len(members) != load or held != 0:
        return f"tiles: {len(members)} members drawn (want {load}), heldCount={held} (want 0)"
    return None


def wall_sources(sources):
    return [{"sourceId": f"zoom:{101 + i}", "kind": "zoom",
             "participantId": str(101 + i), "slot": i,
             "label": f"CAM {i + 1}"} for i in range(sources)]


CRAWL_MANIFEST = {
    "schemaVersion": 1, "templateId": "drill-crawl", "templateVersion": 1, "name": "Drill crawl", "family": "title",
    "canvas": {"width": 1920, "height": 1080}, "bounds": {"width": 1920, "height": 96}, "brandTokens": [],
    "fields": [{"id": "crawl-items", "type": "string", "maxLength": 4096,
                "default": json.dumps(["CoreVideo show drill", "Graphics on air under load", "Crawl boundary adoption"])}],
    "animations": {"in": {"type": "none"}, "out": {"type": "none"}},
    "layers": [{"id": "crawl", "type": "crawl", "x": 0, "y": 0, "width": 1920, "height": 96, "speed": 240,
                "mode": "loop", "font": "Arial", "fontSize": 48, "color": "#FFFFFF",
                "items": {"field": "crawl-items"}}]}


def clock_configuration():
    """The clock row's configuration from the Windows names/clock vectors (real Windows output)."""
    with open(os.path.join(REPO, "contracts", "graphics-shell", "names-clock.vectors.json"), encoding="utf-8") as f:
        rows = json.load(f)["rows"]
    row = next(r for r in rows if r.get("operation") == "clockConfiguration")
    return row["expected"]["configuration"]


class GraphicsDrill:
    """The graphics-* wire GraphicsGateway.swift sends:
    {"type": "graphics-...", "graphics": {schemaVersion, requestId, clientId, coreEpoch, ...}}."""

    INSTANCES = ("drill-lt", "drill-clock", "drill-crawl")

    def __init__(self, core, epoch):
        self.core = core
        self.epoch = epoch
        self.sequence = 0
        self.failures = []
        self.drawn = set()
        self.sent = {}          # instanceId -> (revision, sent_at) of the last Program update
        self.ready_ms = []      # command-to-ready per lower-third update, at poll granularity
        self.samples = 0
        self.high_water = 0
        self.limit = None
        self.crawl_cold_ms = None
        self.crawl_created_at = None
        self.last = {}

    def request(self, kind, instance=None, retry_on=False, **payload):
        self.sequence += 1
        payload.update({"schemaVersion": 1, "requestId": f"drill-{self.sequence}", "clientId": "show-drill",
                        "coreEpoch": self.epoch})
        if instance:
            payload["instanceId"] = instance
        reply = self.core.request({"type": kind, "graphics": payload}, step=kind) or {}
        snapshot = reply.get("snapshot", {})
        result = (snapshot.get("graphics") or {}).get("lastResult") or {}
        if snapshot:
            self.observe(snapshot, poll=False)
        if not reply:
            self.fail(f"{kind} {instance or ''}: request timed out")
            return {}
        if result.get("requestId") != payload["requestId"]:
            # lastResult belongs to an earlier request: this one never reached apply().
            self.fail(f"{kind} {instance or ''}: no result for {payload['requestId']} (last was {result.get('requestId')})")
            return {}
        if not result.get("accepted") and not retry_on:
            self.fail(f"{kind} {instance or ''}: refused ({result.get('reason')})")
        return result

    def fail(self, message):
        if message not in self.failures:
            self.failures.append(message)

    def request_until(self, kind, instance=None, retry=("clock-controller-busy", "required-field-empty"),
                      payload=None, seconds=5.0):
        """For setup steps the core admits asynchronously: retry the named refusals, bounded."""
        deadline = time.time() + seconds
        while True:
            body = dict(payload() if callable(payload) else (payload or {}))
            result = self.request(kind, instance, retry_on=True, **body)
            if result.get("accepted") or result.get("reason") not in retry or time.time() >= deadline:
                if result and not result.get("accepted"):
                    self.fail(f"{kind} {instance or ''}: refused ({result.get('reason')})")
                return result
            self.poll()
            time.sleep(0.05)

    def poll(self):
        reply = self.core.sync([], 0, step="graphics-poll") or {}
        self.observe(reply.get("snapshot", {}), poll=False)

    def row(self, instance):
        for row in (self.last.get("graphics") or {}).get("instances", []):
            if row.get("instanceId") == instance:
                return row
        return {}

    def revision(self, instance):
        return int(self.row(instance).get("revision", 0))

    def setup(self):
        self.request("graphics-template-install", manifest=json.dumps(CRAWL_MANIFEST))
        # Install only queues: wait (bounded) until drill-crawl is in the catalog.
        deadline = time.time() + 5
        while time.time() < deadline and not any(
                r.get("templateId") == "drill-crawl" for r in (self.last.get("graphics") or {}).get("templates", [])):
            self.poll()
            time.sleep(0.05)
        for instance, template, fields in (
                ("drill-lt", "transparent-name", {"name": "DRILL GUEST", "title": "CoreVideo show drill"}),
                ("drill-clock", "clock-panel", None),
                ("drill-crawl", "drill-crawl", None)):
            body = {"scope": "global", "templateId": template, "templateVersion": 1}
            if fields:
                body["fields"] = fields
            if instance == "drill-crawl":
                self.crawl_created_at = time.time()
            self.request("graphics-instance-create", instance, **body)
            if instance == "drill-clock":
                self.request_until("graphics-clock-control", instance, payload=lambda: {
                    "action": "configure", "configuration": clock_configuration(),
                    "expectedClockRevision": int(self.row("drill-clock").get("clockConfigurationRevision", 0)),
                    "expectedRevision": self.revision("drill-clock")})
                self.request_until("graphics-clock-control", instance, payload=lambda: {
                    "action": "start", "expectedRevision": self.revision("drill-clock")})
            # A clock's fields are ready only after its first worker sample.
            self.request_until("graphics-instance-cue", instance,
                               payload=lambda i=instance: {"expectedRevision": self.revision(i) or 1})
            self.request_until("graphics-instance-trigger", instance,
                               payload=lambda i=instance: {"expectedRevision": self.revision(i) or 1, "action": "show"})

    def traffic(self, second, first_tick_of_second):
        """Called on each non-recue poll tick; acts once per second."""
        if not first_tick_of_second:
            return
        revision = self.revision("drill-lt")
        if revision:
            result = self.request("graphics-instance-update", "drill-lt", expectedRevision=revision, target="program",
                                  fields={"name": f"DRILL GUEST {second}", "title": "CoreVideo show drill"})
            if result.get("accepted"):
                self.sent["drill-lt"] = (int(result.get("currentRevision", revision + 1)), time.time())
        if second % 5 == 4:
            self.request("graphics-instance-trigger", "drill-lt", expectedRevision=self.revision("drill-lt"),
                         action="hide")
        elif second % 5 == 0 and second:
            self.request("graphics-instance-trigger", "drill-lt", expectedRevision=self.revision("drill-lt"),
                         action="show")
        if second % 10 == 0 and second and self.revision("drill-crawl"):
            items = json.dumps([f"Crawl update {second}", "Boundary adoption under load"])
            self.request("graphics-instance-update", "drill-crawl", expectedRevision=self.revision("drill-crawl"),
                         target="program", fields={"crawl-items": items})

    def observe(self, snapshot, poll=True):
        """Every snapshot the drill sees (2 Hz polls plus command replies): snapshots, not frames."""
        if not snapshot.get("graphics"):
            return
        self.last = snapshot
        if poll:
            self.samples += 1
        graphics = snapshot["graphics"]
        for row in graphics.get("instances", []):
            instance = row.get("instanceId")
            if (row.get("firstDrawSubmittedFrame") or 0) > 0:
                self.drawn.add(instance)
            for field in ("preparation", "cuePreparation"):
                if row.get(field) == "failed":
                    self.fail(f"{instance}: {field} failed ({row.get('warning') or row.get('cueWarning')})")
            if instance == "drill-crawl" and self.crawl_cold_ms is None and row.get("cuePreparation") == "ready":
                self.crawl_cold_ms = (time.time() - self.crawl_created_at) * 1000
            sent = self.sent.get(instance)
            if sent and row.get("preparation") == "ready" and int(row.get("revision", 0)) >= sent[0]:
                if instance == "drill-lt":
                    self.ready_ms.append((time.time() - sent[1]) * 1000)
                self.sent.pop(instance, None)
        warnings = (snapshot.get("programFrame") or {}).get("warnings") or []
        if "graphics-preparation-failed" in warnings:
            self.fail("programFrame.warnings carried graphics-preparation-failed")
        budget = graphics.get("resourceBudgetAtStatePublication") or {}
        if budget:
            self.high_water = max(self.high_water, int(budget.get("residentHighWater") or 0))
            self.limit = int(budget.get("residentLimit") or 0)
            if (budget.get("residentHighWater") or 0) > (budget.get("residentLimit") or 0):
                self.fail(f"resident high water {budget.get('residentHighWater')} > limit {budget.get('residentLimit')}")

    def finish(self):
        """Remove every instance, then prove a new graphic still reaches Program (the limit is not wedged)."""
        # A visible instance cannot be removed: clear Program, wait (bounded) until
        # every instance is hidden, then remove them.
        self.request("graphics-program-clear", channel="all")
        deadline = time.time() + 5
        while time.time() < deadline and any(self.row(i).get("visibility") not in (None, "hidden") for i in self.INSTANCES):
            reply = self.core.sync([], 0, step="graphics-clear") or {}
            self.observe(reply.get("snapshot", {}))
            time.sleep(0.1)
        for instance in self.INSTANCES:
            if self.row(instance):
                self.request("graphics-instance-remove", instance, expectedRevision=self.revision(instance))
        self.request("graphics-instance-create", "drill-after", scope="global", templateId="transparent-name",
                     templateVersion=1, fields={"name": "AFTER THE DRILL", "title": ""})
        self.request("graphics-instance-cue", "drill-after", expectedRevision=self.revision("drill-after") or 1)
        self.request("graphics-instance-trigger", "drill-after", expectedRevision=self.revision("drill-after") or 1,
                     action="show")
        deadline = time.time() + 5
        while time.time() < deadline and "drill-after" not in self.drawn:
            reply = self.core.sync([], 0, step="graphics-after") or {}
            self.observe(reply.get("snapshot", {}))
            time.sleep(0.1)

    def report(self):
        print(f"GRAPHICS sampled {self.samples} snapshots at 2 Hz (snapshots, not every frame)")
        missing = [i for i in self.INSTANCES if i not in self.drawn]
        if missing:
            self.failures.append(f"never reached firstDrawSubmittedFrame > 0: {', '.join(missing)}")
        if "drill-after" not in self.drawn:
            self.failures.append("a new graphic after removing every instance never drew: the resident limit is wedged")
        if self.ready_ms:
            ordered = sorted(self.ready_ms)
            print(f"GRAPHICS lower-third command-to-ready (poll granularity, ~500 ms): n={len(ordered)} "
                  f"p50={ordered[len(ordered) // 2]:.0f}ms max={ordered[-1]:.0f}ms")
        if self.crawl_cold_ms is not None:
            print(f"GRAPHICS crawl first cold preparation: {self.crawl_cold_ms:.0f}ms (poll granularity)")
        budget = (self.last.get("graphics") or {}).get("resourceBudgetAtStatePublication") or {}
        if not self.limit:
            self.fail("no resourceBudgetAtStatePublication was ever observed: the resident-limit gate saw nothing")
        elif self.high_water < 1:
            self.fail("resident high water stayed 0: no graphic was ever resident")
        print(f"GRAPHICS resident high water {self.high_water} of {self.limit}; final budget "
              f"{json.dumps({k: budget.get(k) for k in ('residentResources', 'residentHighWater', 'residentLimit', 'refused')})}"
              " (removed revisions stay resident until cache pressure: plan correction 5)")
        return self.failures


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--seconds", type=float, default=40.0)
    parser.add_argument("--core", default=DEFAULT_CORE)
    parser.add_argument("--evidence", default=os.path.join(REPO, "artifacts", "show-drill", "command-timing.json"))
    # The recorded-rate gate measures the MACHINE as much as the code, and a
    # shared CI runner cannot hold 60 Hz: measured on macos-14 across three
    # commits with identical code paths, the COMPOSITOR itself rendered 48.7,
    # 47.1 and 60.2 fps with worst ticks up to 123.7 ms, and the muxed rate
    # followed it at 26.4, 29.5 and 34.6. Left blocking there it is permanent
    # red, which trains everyone to ignore CI -- the same argument that already
    # made the loaded step advisory, and exactly how a real compile break hid on
    # main for six commits on 2026-09-12.
    #
    # It stays BLOCKING by default, so a local run and real hardware still
    # enforce it, and it still PRINTS its full verdict either way. Only an
    # explicit caller may downgrade it, and it says so loudly when it does.
    parser.add_argument("--advisory-recorded-rate", action="store_true",
                        help="print the recorded-rate verdict but do not fail on it "
                             "(for shared CI runners that cannot source 60 Hz)")
    parser.add_argument("--load", type=int, default=0,
                        help="synthesize N 1080p Zoom feeds (the production wall)")
    parser.add_argument("--tiles", action="store_true",
                        help="with --load N: put a CoreVideo Tiles wall of all N feeds on Program and "
                             "Preview (routes []), re-cue the Preview wall, and FAIL unless the "
                             "snapshot's tiles node has N members and heldCount 0")
    parser.add_argument("--tiles-style", choices=["plain", "rounded-glow"], default="plain",
                        help="with --tiles: rounded-glow = radius 24, border 2, glow 12")
    parser.add_argument("--media", action="store_true",
                        help="put a 1080p60 clip and a looping 1080p30 video background on Program "
                             "and gate their decode/presentation rates")
    parser.add_argument("--grade-previews", type=int, default=0,
                        help="open N private grade-preview editors over stdio for the whole run (Metal G5; max 3)")
    parser.add_argument("--scopes", action="store_true",
                        help="with --grade-previews: also demand the three scopes (Metal G6)")
    parser.add_argument("--source-grades", action="store_true",
                        help="after configure, apply a per-source grade (warm-film, exposure +2) to every "
                             "wall participant (query-source-grade, then set-source-grade) and gate as usual")
    parser.add_argument("--lower-third", action="store_true",
                        help="keep a legacy lower third on Program for the whole run: new text every "
                             "second (a fresh overlay raster preparation each time) and a hide/re-show "
                             "every five seconds, so the render tick is measured with the overlay "
                             "raster cycling (Metal graphics backend G1, spec 7.4)")
    parser.add_argument("--graphics-cue", action="store_true",
                        help="put three template graphics instances on Program for the whole run (a "
                             "transparent-name lower third updated every second, a clock-panel clock and a "
                             "drill crawl) over the graphics-* wire the Mac shell sends, and gate that no "
                             "preparation fails and the 16-revision resident limit is never wedged (G4)")
    parser.add_argument("--source-preview-demand", action="store_true",
                        help="hold the Sources tab's preview-demand lease (renewed every poll, as the "
                             "shell renews it every second) so the core renders the source sheet "
                             "(sources Phase 2, S6), then gate that the sheet was actually rendered")
    parser.add_argument("--ffmpeg", default=None,
                        help="ffmpeg bin dir or exe for --media fixtures "
                             "(default: COREVIDEO_FFMPEG_BIN_DIR, then PATH)")
    args = parser.parse_args()

    media_clip = media_background = None
    if args.media:
        ffmpeg = find_ffmpeg(args.ffmpeg)
        if not ffmpeg:
            print("FAIL harness: --media needs ffmpeg")
            return 1
        try:
            media_clip, media_background = media_fixtures(
                ffmpeg, os.path.join(REPO, "artifacts", "show-drill"))
        except RuntimeError as error:
            print(f"FAIL harness: {error}")
            return 1

    if not os.path.exists(args.core):
        print(f"FAIL core binary not found: {args.core}")
        return 1
    if args.load and not os.path.exists(FAKE_ENGINE):
        print(f"FAIL fake engine not built: {FAKE_ENGINE}")
        return 1

    failures = []
    core = Core(args.core, env=load_env(args.load) if args.load else None, evidence_path=args.evidence)
    time.sleep(1.5)

    # Production mode intentionally suppresses the frame/timing lines this
    # performance harness evaluates. Opt in through the same live command as
    # the Health switch so a green drill still proves the diagnostic path.
    diagnostics = core.sync(
        [{"type": "set-verbose-diagnostics", "enabled": True}], 0, step="enable-diagnostics")
    if diagnostics is None:
        print("FAIL enabling verbose diagnostics timed out")
        core.kill()
        return 1

    if args.load:
        joined = core.request({"type": "zoom-join", "payload": {
            "meetingNumber": "1234567890", "displayName": "show-drill"}}, timeout=20.0, step="synthetic-join")
        if joined is None:
            print("FAIL zoom-join timed out")
            core.kill()
            return 1
        print(f"LOAD  {args.load} x 1080p synthetic feeds joining…")
        time.sleep(4.0)  # engine spin-up + first frames on every source

    timeouts = 0

    def step(name, commands, elapsed):
        nonlocal timeouts
        response = core.sync(commands, elapsed, step=name)
        if response is None:
            timeouts += 1
            failures.append(f"{name}: request TIMED OUT")
        return response

    # 1. Arm the wall and put scenes on both buses (the shell's connect path).
    #    Under load this is the owner's real wall: PGM + PVW large, N source
    #    tiles below, every tile a live 1080p feed.
    configure = [
        {"type": "configure-multiviewer", "layoutMode": "pgmPvwTop",
         "tileCount": max(10, args.load)},
    ]
    if args.load:
        configure.append({"type": "set-multiview-layout", "canvasWidth": 1920,
                          "canvasHeight": 1080, "sources": wall_sources(args.load)})
        # Program and preview each carry a distinct real participant, so the two
        # bus composites do genuine per-source work (not an empty-scene no-op).
        configure.append({"type": "load-scene-graph", "sceneId": "pgm", "routes": [
            dict(route("pgm-0"), mode="fixed", participantId="101")]})
        configure.append({"type": "set-preview-scene", "sceneId": "pvw", "routes": [
            dict(route("pvw-0"), mode="fixed", participantId="102")]})
    else:
        configure.append({"type": "load-scene-graph", "sceneId": "pgm",
                          "routes": [route("pgm-0")]})
        configure.append({"type": "set-preview-scene", "sceneId": "pvw",
                          "routes": [route("pvw-0")]})
    if args.tiles:
        if not args.load:
            print("FAIL  --tiles needs --load N (the wall's members are the N synthetic feeds)")
            core.kill()
            return 1
        tiles_members = [f"zoom:{101 + i}" for i in range(args.load)]
        tiles_style = args.tiles_style if args.tiles_style != "plain" else None
        for command in configure:
            if command["type"] in ("load-scene-graph", "set-preview-scene"):
                command["sceneId"] = "tiles-a"
                command["routes"] = []
                command["tiles"] = tiles_wall("tiles-a", tiles_members, tiles_style)
        print(f"TILES {args.load}-member wall on Program and Preview (style {args.tiles_style})")
    if args.media:
        # The same Program scene plus a clip in the top-right quarter and a
        # looping video background behind everything.
        program = next(c for c in configure if c["type"] == "load-scene-graph")
        program["routes"] = program["routes"] + [{
            "routeId": "pgm-clip", "mode": "fixed", "audioRole": "mix", "fitMode": "fill",
            "opacity": 1.0, "zIndex": 1, "mediaAssetId": MEDIA_CLIP_ID, "mediaAssetName": MEDIA_CLIP_ID,
            "mediaAssetKind": "stinger", "mediaAssetPath": media_clip, "mediaAssetLoop": False,
            "rect": {"x": 0.5, "y": 0, "width": 0.5, "height": 0.5}}]
        program["background"] = {"mediaAssetId": MEDIA_BG_ID, "mediaAssetName": MEDIA_BG_ID,
                                 "mediaAssetKind": "stinger", "mediaAssetPath": media_background}
    step("configure", configure, 500)

    graphics = None
    if args.graphics_cue:
        handshake = core.request({"type": "handshake"}, step="handshake") or {}
        if "graphics-instances" not in ((handshake.get("profile") or {}).get("capabilities") or []):
            print("FAIL core does not advertise graphics-instances")
            core.kill()
            return 1
        epoch = (((core.sync([], 600, step="graphics-epoch") or {}).get("snapshot") or {}).get("graphics") or {}).get("coreEpoch")
        if not epoch:
            print("FAIL the snapshot carries no graphics.coreEpoch")
            core.kill()
            return 1
        graphics = GraphicsDrill(core, os.environ.get("COREVIDEO_DRILL_STALE_EPOCH") or epoch)
        graphics.setup()
        print(f"GRAPHICS three instances cued and shown (coreEpoch {graphics.epoch})")

    if args.source_grades:
        # Per-source grading (grading plan 2026-11-16, Task 7): the same requests the Mac
        # store sends. Program carries 101, so at least one routed source is graded on air.
        time.sleep(1.0)  # let a render tick observe every wall source
        participants = [str(101 + i) for i in range(args.load)] if args.load else []
        accepted = 0
        for pid in participants:
            query = core.request({"type": "query-source-grade", "sourceId": pid}, step="grade-query") or {}
            if not query.get("present"):
                print(f"GRADE {pid}: no frame this tick (present=false); not applied")
                continue
            applied = core.request({"type": "set-source-grade", "sourceId": pid,
                                    "sourceEpoch": query.get("sourceEpoch", 0),
                                    "expectedRevision": query.get("revision", 0),
                                    "grade": {"lut": "warm-film", "exposure": 2}}, step="grade-set") or {}
            if applied.get("accepted"):
                accepted += 1
            else:
                print(f"GRADE {pid}: refused ({applied.get('reason') or applied.get('error')})")
        print(f"GRADE {accepted}/{len(participants)} per-source grades accepted "
              f"(warm-film, exposure +2; Program route = 101)")
        if participants and accepted == 0:
            failures.append("--source-grades: no per-source grade was accepted, so the run proved nothing")

    # Private grade previews (grading Phase 2, Task 10): the requests the Mac shell's
    # GradePreviewSession sends — one full set-grade-preview per editor, then a renewOnly every
    # 500 ms (below) — so the run measures Program with N editors open on the shared GPU.
    preview_editors = []
    if args.grade_previews:
        participants = [str(101 + i) for i in range(max(1, args.load))]
        for i in range(min(3, args.grade_previews)):
            editor = {"instanceId": f"drill-editor-{i}", "sourceId": participants[i % len(participants)], "revision": 1}
            answer = core.request(dict(editor, type="set-grade-preview", enabled=True, scopesEnabled=args.scopes,
                                       scopesOriginal=False, histogramMode=1, waveformMode=1, scopeView=0,
                                       compareOriginal=False,
                                       scopeRoi={"enabled": False, "x": 0, "y": 0, "width": 1, "height": 1,
                                                 "revision": 0, "shape": "rectangle"},
                                       grade={"lut": "warm-film", "exposure": 2, "contrast": 0,
                                              "saturation": 0, "temperature": 0}), step="grade-preview") or {}
            if answer.get("accepted"):
                preview_editors.append(editor)
            else:
                print(f"GRADE-PREVIEW {editor['instanceId']}: refused ({answer.get('error') or answer})")
        print(f"GRADE-PREVIEW {len(preview_editors)} editors open (scopes {'on' if args.scopes else 'off'})")

    # 2. Operator traffic at the cadence the shell produces, for the whole run:
    #    2Hz snapshot syncs plus scene re-cues, while the render loop is live.
    start = time.time()
    tick = 0
    graphics_second = -1
    media_first = media_last = None
    media_first_at = media_last_at = 0.0
    while time.time() - start < args.seconds:
        tick += 1
        elapsed = int((time.time() - start) * 1000) + 1000
        if graphics:
            second = int(time.time() - start)
            graphics.traffic(second, second != graphics_second)
            graphics_second = second
        if tick % 5 == 0:
            # Re-cue preview (the solo-scene path) — the traffic that flooded.
            solo = "mv-solo-a" if tick % 10 == 0 else "mv-solo-b"
            if args.tiles:
                # Under --tiles the re-cue re-sends the same Preview wall.
                response = step("recue", [{"type": "set-preview-scene", "sceneId": "tiles-a",
                                           "routes": [],
                                           "tiles": tiles_wall("tiles-a", tiles_members, tiles_style)}],
                                elapsed)
            else:
                response = step("recue", [{"type": "set-preview-scene", "sceneId": solo,
                                           "routes": [route("solo-0")]}], elapsed)
        elif args.lower_third:
            # Every poll tick (2 Hz) carries the overlay; the text changes once a
            # second and every fifth second the lower third is taken out, so the
            # raster both re-prepares and builds in/out under load.
            second = tick // 2
            shown = second % 5 != 4
            response = step("lower-third", [{"type": "set-overlay-asset",
                                             "overlayId": "overlay:lower-third",
                                             "enabled": shown,
                                             "text": f"DRILL GUEST {second}",
                                             "title": "CoreVideo show drill"}], elapsed)
        else:
            response = step("poll", [{"type": "set-source-preview-demand", "enabled": True}]
                            if args.source_preview_demand else [], elapsed)
            if graphics and response is not None:
                graphics.observe(response.get("snapshot", {}))
        for editor in preview_editors:
            core.request(dict(editor, type="set-grade-preview", renewOnly=True), step="grade-preview-renew")
        # Both ends of the operator-traffic window, for the --media block. The
        # first sample is taken one second in so a cold decoder open is not
        # counted against the steady rate.
        if response is not None and time.time() - start >= 1.0:
            if media_first is None:
                media_first, media_first_at = response.get("snapshot", {}), time.time()
            media_last, media_last_at = response.get("snapshot", {}), time.time()
        time.sleep(0.5)

    # 3. Record a short session and confirm the artifact is real.
    folder = os.path.join(REPO, "artifacts", "show-drill")
    os.makedirs(folder, exist_ok=True)
    record_started_at = time.time()
    record_start = step("record-start", [
        {"type": "start-program-output", "destinations": ["recording"],
         "isoSourceIds": [], "isoParticipantIds": []},
        {"type": "set-recording-targets", "targetFolder": folder,
         "filenamePrefix": "drill", "format": "mp4", "quality": "high",
         "isoSourceIds": [], "isoParticipantIds": []},
        {"type": "start-recording-session", "sessionId": "drill"},
    ], 60000)
    if graphics:
        # Graphics stay on air with traffic through the recorded window, so the
        # recorded-rate and render-window gates measure Program WITH graphics.
        recording_until = time.time() + 6
        while time.time() < recording_until:
            second = int(time.time() - start)
            graphics.traffic(second, second != graphics_second)
            graphics_second = second
            graphics.poll()
            time.sleep(0.5)
    else:
        time.sleep(6)
    # Stop the SESSION first so the writer flushes its moov box, then let the
    # file settle before probing. Killing the core (below) is a hard kill: an
    # unfinalised mp4 has no moov atom and is unplayable, no matter how many
    # bytes are on disk.
    stop = step("record-stop", [{"type": "stop-recording-session",
                                 "reason": "drill complete"}], 66000)
    record_stopped_at = time.time()
    step("encoder-stop", [{"type": "stop-encoder-session",
                           "reason": "drill complete"}], 67000)
    time.sleep(2.5)
    # The last [render] window the gates read below was rendered with the
    # graphics still on air; only now clear Program and prove the limit is not wedged.
    last_render_with_graphics = next(
        (l for l in reversed(core.stderr) if l.startswith("[render] ") and "dropped=" in l), "")
    if graphics:
        graphics.finish()
        failures.extend(f"--graphics-cue: {f}" for f in graphics.report())
        print(f"GRAPHICS last render window with graphics on air: {last_render_with_graphics or '(none)'}")

    # ── assertions ───────────────────────────────────────────────────────────
    snapshot = (stop or {}).get("snapshot", {})
    if args.tiles:
        verdict = tiles_verdict(snapshot, args.load)
        if verdict:
            failures.append(verdict)
        else:
            node = snapshot.get("tiles") or {}
            print(f"TILES {len(node.get('members') or [])} members drawn, heldCount 0, "
                  f"blackCount {node.get('blackCount')}")
    if preview_editors:
        ready = [e for e in core.events if e.get("type") == "grade-preview" and e.get("status") == "ready"
                 and (e.get("texture") or {}).get("iosurfaceId")]
        scoped = [e for e in ready if ((e.get("scopes") or {}).get("texture") or {}).get("iosurfaceId")]
        diag = snapshot.get("gradePreview") or {}
        print(f"GRADE-PREVIEW ready events {len(ready)} (with scopes {len(scoped)}); worker submitted="
              f"{diag.get('submitted')} completed={diag.get('completed')} superseded={diag.get('superseded')} "
              f"failed={diag.get('failed')} lastWorkMs={diag.get('lastWorkMs')}")
        if not ready:
            failures.append("--grade-previews: no ready grade-preview surface arrived, so the run proved nothing")
        if args.scopes and not scoped:
            failures.append("--scopes: no scope surface arrived, so the run proved nothing")
    if args.source_grades:
        rows = snapshot.get("sourceGrades") or []
        print(f"GRADE end of run: {sum(1 for r in rows if r.get('active'))}/{len(rows)} "
              f"sourceGrades rows active")

    # STAGE EVIDENCE for the recorded-rate gate. The muxed rate on its own cannot
    # tell "the compositor never produced 60" from "the compositor produced 60 and
    # the encoder path lost them", and this drill used to ASSERT the second one
    # (blaming the ~50Hz audio worker) in a message it printed either way. That
    # attribution is stale for Program video: the submit moved to the signalled
    # video tick (MediaCore.cpp renderVideoOutputTick), and the audio-worker
    # submit is guarded by videoOutputTickRunning_, which JsonRpcServer sets true
    # unconditionally in any real run. The gate is unchanged; only the diagnosis
    # is now printed as numbers instead of asserted. These come from the core's
    # own realtimeEvidence/encoderEvidence nodes sampled at both ends of the
    # record window, so they are deltas over that window, not lifetime totals.
    record_window_s = max(0.0, record_stopped_at - record_started_at)
    stage_rates = stage_evidence((record_start or {}).get("snapshot", {}),
                                 snapshot, record_window_s)
    # Prefer a full render-rate WINDOW (it carries dropped=/worst=); the bare
    # "displayTick #N" line also matches "[render] ... fps" and says much less.
    last_render_window = next(
        (l for l in reversed(core.stderr) if l.startswith("[render] ") and "dropped=" in l),
        next((l for l in reversed(core.stderr)
              if l.startswith("[render] ") and "fps" in l), ""))

    recording = snapshot.get("recording", {})
    queue_dropped = (recording.get("proof") or {}).get("encoderQueueDroppedVideoFrames")
    # Printed on the PASS path too. A green recorded rate whose video-out tick sits
    # at 50 is a different fact from one that sits at 60, and only one of them is
    # safe to keep believing.
    print("")
    print(f"Recording-window stage rates (over {record_window_s:.1f}s):")
    for label, value in stage_rates:
        print(f"  {label}: {value}")
    print(f"  encoder queue dropped video frames: "
          f"{queue_dropped if queue_dropped is not None else 'unavailable'}")
    if last_render_window:
        print(f"  last render window: {last_render_window}")
    artifact = recording.get("artifactPath", "")
    if not os.path.isabs(artifact or ""):
        artifact = os.path.join(REPO, artifact) if artifact else ""
    # BYTES ARE NOT PROOF. This drill previously passed on a file with no moov
    # atom — megabytes on disk that no player can open. A recording assertion
    # has to open the artifact and find a real video stream, at the resolution
    # the show was configured for (a 320x180 preview upscaled into a 1080p
    # container is the failure the RTMP path shipped for months).
    if artifact and os.path.exists(artifact) and os.path.getsize(artifact) > 1024:
        try:
            probe = subprocess.run(
                ["ffprobe", "-hide_banner", "-v", "error", "-select_streams", "v:0",
                 "-show_entries", "stream=width,height,nb_frames",
                 "-of", "default=nw=1:nk=1", artifact],
                capture_output=True, text=True)
        except FileNotFoundError:
            # LOUD, not a traceback and NEVER a silent skip: without ffprobe the
            # playability gate cannot run, so the drill must not report success.
            # It failed this way on macos-14 runners, which ship no ffmpeg.
            failures.append(
                "ffprobe is not installed, so the recording could not be checked "
                "for playability — install ffmpeg on this machine/runner")
            probe = None
        fields = [line for line in probe.stdout.split() if line] if probe else []
        if probe is None:
            pass  # already reported above
        elif probe.returncode != 0 or len(fields) < 2:
            failures.append(
                f"recording is UNPLAYABLE ({os.path.getsize(artifact)} bytes on "
                f"disk, ffprobe: {(probe.stderr or '').strip()[:120]}) — an "
                f"unfinalised mp4 has no moov atom")
        else:
            width, height = int(fields[0]), int(fields[1])
            if width < 1280 or height < 720:
                failures.append(
                    f"recording is {width}x{height} — the program was composited "
                    f"at a higher resolution, so this is a downscaled proxy")
            else:
                print(f"PASS recording playable, {width}x{height}, "
                      f"{os.path.getsize(artifact)} bytes")
                # RECORDED FRAME RATE. "Playable at 1920x1080" and "60fps
                # sustained" are both true while the FILE runs at 50 — the
                # compositor rate is not the muxed rate. encoder->submit is
                # called from the ~50Hz audio/output worker, whose 20ms period
                # is an AUDIO constant (960 samples at 48k), so program video is
                # muxed at the AUDIO cadence. A container that DECLARES 60 while
                # holding 50 is exactly the kind of true-but-misleading pair this
                # drill exists to catch.
                probe_rate = subprocess.run(
                    ["ffprobe", "-hide_banner", "-v", "error", "-count_frames",
                     "-select_streams", "v:0", "-show_entries",
                     "stream=nb_read_frames,duration", "-of", "default=nw=1",
                     artifact], capture_output=True, text=True)
                # Parse BY NAME: ffprobe emits fields in its own order, not the
                # order requested, so positional parsing silently reads the
                # duration as a frame count.
                fields = {}
                for line in probe_rate.stdout.splitlines():
                    if "=" in line:
                        name, _, value = line.partition("=")
                        fields[name.strip()] = value.strip()
                if fields:
                    try:
                        frames = int(fields.get("nb_read_frames", "0"))
                        seconds = float(fields.get("duration", "0"))
                    except ValueError:
                        frames, seconds = 0, 0.0
                    if seconds > 0.5 and frames > 0:
                        actual = frames / seconds
                        ok_rate = actual >= TARGET_OUTPUT_FPS * MIN_RECORDED_FPS_RATIO
                        print(f"{'PASS' if ok_rate else 'FAIL'} recorded rate "
                              f"{actual:.1f}fps of {TARGET_OUTPUT_FPS:.0f} "
                              f"({frames} frames / {seconds:.2f}s)")
                        if not ok_rate and args.advisory_recorded_rate:
                            print("  ADVISORY: the recorded-rate gate is downgraded for "
                                  "this run (--advisory-recorded-rate). This is a real "
                                  "measurement and it did NOT pass; it is not gating "
                                  "because a shared runner cannot source 60 Hz. Run the "
                                  "drill on real hardware before shipping perf work.")
                        if not ok_rate and not args.advisory_recorded_rate:
                            # NAME THE EVIDENCE, DO NOT ASSERT THE CAUSE. The old
                            # message blamed the ~50Hz audio worker; Program video
                            # no longer leaves from there, so that pointed every
                            # future reader at the wrong stage. The gate is
                            # unchanged — this is still a failure — but the reader
                            # now gets the numbers that separate the candidates.
                            failures.append(
                                f"recording muxed {actual:.1f}fps while the drill "
                                f"configured {TARGET_OUTPUT_FPS:.0f} ({frames} frames / "
                                f"{seconds:.2f}s), which is off-spec no matter what the "
                                f"container declares. Stage evidence over the "
                                f"{record_window_s:.1f}s record window: "
                                + "; ".join(f"{label} {value}" for label, value in stage_rates)
                                + f"; encoder queue dropped video frames "
                                f"{queue_dropped if queue_dropped is not None else 'unavailable'}"
                                + (f"; last render window '{last_render_window}'"
                                   if last_render_window else "")
                                + ". Read it this way: if the compositor render rate is "
                                f"itself below {TARGET_OUTPUT_FPS:.0f} then this machine "
                                f"never produced 60 and the file is honest about it; if "
                                f"the compositor held {TARGET_OUTPUT_FPS:.0f} but the "
                                f"video-out tick, the encoder written rate or the muxed "
                                f"rate is lower, the frames were lost downstream of the "
                                f"compositor — the queue-drop and dropped-video counters "
                                f"say whether the encoder path shed them. A muxed rate "
                                f"near 50 with a healthy 60 compositor is the historic "
                                f"audio-worker alias, but Program submit now rides the "
                                f"signalled video tick, so check the video-out tick rate "
                                f"before assuming that is what happened.")
    else:
        failures.append(f"recording produced no artifact (path={artifact or 'none'})")

    if args.media:
        print("")
        media_lines, media_failures = media_report(media_first, media_last, media_last_at - media_first_at)
        for line in media_lines:
            print(line)
        copy_lines = [l for l in core.stderr if l.startswith("[media-decoder] copy ")]
        for line in copy_lines[-6:]:
            print(f"  {line}")
        if not copy_lines:
            print("  (no [media-decoder] copy lines: a reader logs one per 600 copied frames)")
        if media_failures:
            failures.extend("media: " + failure for failure in media_failures)
        else:
            print("PASS media sources held their rates with no decoder restart")

    # Sources Phase 2 (S6): the monitor shed state is the judge of the sheet's cost, so it is
    # printed on every run (demand on AND off) for the A/B; with demand on, a sheet that was
    # never rendered means the run measured nothing.
    shed = (snapshot.get("realtimeEvidence") or {}).get("monitorShed") or {}
    print(f"MONITOR SHED divisor={shed.get('divisor')} level={shed.get('level')} "
          f"entered={shed.get('enteredCount')} shedTicks={shed.get('shedTicks')} "
          f"lastReason={shed.get('lastReason')}")
    # The sheet is judged on the LAST operator-window poll: the lease is 3 s, and the record
    # segment after the window renews nothing, so the final snapshot has (correctly) let it go.
    sheet = (media_last or {}).get("sourceSheet") or {}
    sheet_texture = sheet.get("texture") or {}
    live_cells = sum(1 for cell in sheet.get("cells", []) if cell.get("state") in ("live", "held"))
    print(f"SOURCE SHEET active={sheet.get('active')} surface={sheet_texture.get('iosurfaceId')} "
          f"frame={sheet_texture.get('frameNumber')} liveCells={live_cells}")
    if args.source_preview_demand:
        if sheet.get("active") and live_cells > 0:
            print("PASS source sheet rendered under the demand lease")
        else:
            failures.append(f"--source-preview-demand: no source sheet with a live cell "
                            f"(active={sheet.get('active')}, liveCells={live_cells}); the run measured nothing")

    multiview = snapshot.get("multiviewSharedTexture") or {}
    roles = {tile.get("role") for tile in multiview.get("tiles", [])}
    if {"pgm", "pvw"} <= roles:
        print("PASS multiview wall has PGM and PVW cells")
    else:
        failures.append(f"multiview missing bus cells (roles={sorted(roles)})")

    # PER SITE. Several sites emit this line, so taking whichever logged last
    # conflated them — a render-tick ratio could be reported as an audio one and
    # vice versa. Keep the latest counters for each site and gate on the WORST,
    # naming it, so the number points at something actionable.
    per_site = {}
    for line in core.stderr:
        if "lock-guardrail" not in line or "over-budget" not in line:
            continue
        try:
            site = line.split(" at '")[1].split("'")[0]
            parts = line.split("over-budget ")[1].split(" of ")
            per_site[site] = (int(parts[0]), int(parts[1].split(" ")[0]))
        except (IndexError, ValueError):
            continue
    worst_site = ""
    over = 0
    total = 0
    for site, (site_over, site_total) in per_site.items():
        if site_total and site_over / site_total >= (over / total if total else 0):
            worst_site, over, total = site, site_over, site_total
    if total:
        ratio = over / total
        verdict = "PASS" if ratio <= MAX_OVER_BUDGET_RATIO else "FAIL"
        print(f"{verdict} coreMutex over-budget {over}/{total} ({ratio:.0%}) "
              f"worst site '{worst_site}'")
        if ratio > MAX_OVER_BUDGET_RATIO:
            failures.append(
                f"render tick over budget on {ratio:.0%} of holds "
                f"(max {MAX_OVER_BUDGET_RATIO:.0%}) — this starves the RPC queue")
    else:
        print("PASS coreMutex never exceeded its budget")

    # OPERATOR RESPONSIVENESS. Every frame-path metric here measures
    # source->program; none of them can see the command path a Take rides. A
    # comment in the core claims media-core-sync holds the core lock 50-100ms,
    # which would be felt as a laggy cut, so measure it rather than assume.
    syncs = sorted(ms for name, ms in core.round_trips if name == "media-core-sync")
    if syncs:
        p50 = syncs[len(syncs) // 2] * 1000
        p99 = syncs[min(len(syncs) - 1, int(len(syncs) * 0.99))] * 1000
        worst = syncs[-1] * 1000
        ok = p99 <= MAX_COMMAND_P99_MS
        print(f"{'PASS' if ok else 'FAIL'} operator command round-trip p50 {p50:.1f}ms / "
              f"p99 {p99:.1f}ms / worst {worst:.1f}ms ({len(syncs)} syncs)")
        if not ok:
            failures.append(
                f"media-core-sync p99 {p99:.1f}ms exceeds {MAX_COMMAND_P99_MS:.0f}ms — "
                f"a Take that takes this long is felt by the operator")

    if timeouts:
        print(f"FAIL {timeouts} request(s) timed out")
    else:
        print("PASS every request answered")

    # Where did the render tick's lock hold actually go? `render` is the whole
    # hold, `drain` the event serialize/enqueue that sits under the same lock but
    # outside MediaCore's stage timers — the previously unaccounted remainder.
    rate_lines = [l for l in core.stderr if l.startswith("[render] ") and "fps" in l]
    stage_lines = [l for l in core.stderr if "stages avg-ms" in l]

    # The engine tap hands the compositor already-decoded I420 buffers by
    # shared_ptr, so it must be a pointer store — NOT a per-participant memcpy.
    # At 10x1080p a copy under the engine mutex costs ~7-10ms and the render
    # thread inherits it while holding coreMutex (the repo's "no pixel work
    # under shared locks" law). Budget it so that can never come back.
    # Sustained render rate + lock hold under the full wall. Read the LAST window
    # so startup/join transients don't count.
    if args.load:
        windows = [l for l in rate_lines if "lockWait=" in l]
        if windows:
            # With --graphics-cue, judge the window rendered while the graphics
            # were on air, not the one after finish() cleared Program.
            last = last_render_with_graphics if (graphics and last_render_with_graphics) else windows[-1]
            fps = float(last.split("] ")[1].split("fps")[0])
            render_ms = float(last.split("render=")[1].split("ms")[0])
            dropped = int(last.split("dropped=")[1].split(" ")[0]) if "dropped=" in last else -1
            worst = float(last.split("worst=")[1].split("ms")[0]) if "worst=" in last else -1.0
            ok = (fps >= MIN_LOAD_FPS and render_ms <= MAX_LOAD_RENDER_MS
                  and 0 <= dropped <= MAX_DROPPED_FRAMES)
            print(f"{'PASS' if ok else 'FAIL'} {args.load}x1080p60 sustained "
                  f"{fps:.1f}fps of {TARGET_OUTPUT_FPS:.0f}, {render_ms:.1f}ms hold, "
                  f"{dropped} dropped, worst frame {worst:.1f}ms")
            if fps < MIN_LOAD_FPS:
                failures.append(
                    f"{args.load}x1080p60 held only {fps:.1f}fps of {TARGET_OUTPUT_FPS:.0f} — "
                    f"a switcher that cannot hold its configured output rate is off-spec")
            if dropped > MAX_DROPPED_FRAMES:
                failures.append(
                    f"{dropped} frame(s) dropped to render lag (worst {worst:.1f}ms) — "
                    f"professional switchers are judged on dropped frames, not mean fps")
            if render_ms > MAX_LOAD_RENDER_MS:
                failures.append(
                    f"render tick {render_ms:.1f}ms leaves too little headroom in the "
                    f"16.7ms budget for transients")
        else:
            failures.append("no render-rate windows logged — cannot verify load perf")

    if args.load and stage_lines:
        taps = []
        for line in stage_lines:
            if "tap=" in line:
                taps.append(float(line.split("tap=")[1].split(" ")[0]))
        if taps:
            worst = max(taps)
            verdict = "PASS" if worst <= MAX_ENGINE_TAP_MS else "FAIL"
            print(f"{verdict} engine tap worst {worst:.2f}ms "
                  f"(budget {MAX_ENGINE_TAP_MS:.1f}ms at {args.load}x1080p)")
            if worst > MAX_ENGINE_TAP_MS:
                failures.append(
                    f"engine tap {worst:.2f}ms exceeds {MAX_ENGINE_TAP_MS:.1f}ms — pixel "
                    f"work under the engine mutex stalls the render thread")
    if rate_lines:
        print("\nRender tick (last 3 windows):")
        for line in rate_lines[-3:]:
            print(f"  {line}")
    lat_lines = [l for l in core.stderr if "[zoom-latency]" in l]
    # Print EVERY window: a latency spike that only happens at join is a very
    # different fact from one that recurs, and showing only the tail hides which.
    if lat_lines:
        print(f"Source->program latency ({len(lat_lines)} windows):")
        for line in lat_lines:
            print(f"  {line}")
    # DID THE HARNESS ACTUALLY SOURCE THE LOAD IT WAS ASKED FOR? The delivery
    # ratio below is (frames the compositor saw) / (frames we ASKED the fake
    # engine to produce). If the engine could not produce them — a CI VM cannot
    # decode 8x1080p60, ~1.5GB/s — the ratio collapses and reads as "the core
    # lost your frames", which is a false accusation against the core. Measured
    # on a macos-14 runner: ~250 of 480 frames/s sourced, reported as "51%
    # delivered". Parse the rate the core actually received and say so.
    ingest_rates = []
    for line in [l for l in core.stderr if "[zoom-ingest]" in l][-3:]:
        try:
            ingest_rates.append(float(line.split("]")[1].strip().split()[0]))
        except (IndexError, ValueError):
            continue
    sourced_fps = sorted(ingest_rates)[len(ingest_rates) // 2] if ingest_rates else None
    requested_fps = args.load * TARGET_OUTPUT_FPS
    harness_short = (sourced_fps is not None and requested_fps > 0
                     and sourced_fps < requested_fps * 0.9)

    if args.load and lat_lines:
        # Each window covers ~2s; a partial first window (join still settling) is
        # warm-up, not steady state, so judge on windows that saw a full window's
        # worth of frames.
        expected = args.load * TARGET_OUTPUT_FPS * 2.0
        windows = []
        for l in lat_lines:
            try:
                n = int(l.split("n=")[1].split(",")[0])
                windows.append((float(l.split("p50=")[1].split("ms")[0]),
                                float(l.split("p99=")[1].split("ms")[0]), n))
            except (IndexError, ValueError):
                continue
        steady = [w for w in windows if w[2] >= expected * 0.5]
        if steady:
            worst_p50 = max(w[0] for w in steady)
            worst_p99 = max(w[1] for w in steady)
            # MEDIAN, not min. A single window can dip when the machine is busy
            # (a concurrent build was enough to read 52% once), and a gate that
            # fails on one hiccup gets ignored — which is worse than no gate.
            # The systemic failure this exists to catch (the 8ms ingest poll
            # losing ~30% of frames on EVERY window) moves the median, so this
            # still catches it. The worst window is printed either way so a real
            # excursion stays visible instead of being averaged away.
            counts = sorted(w[2] for w in steady)
            delivery = counts[len(counts) // 2] / expected
            worst_delivery = counts[0] / expected
            ok = (worst_p50 <= MAX_LATENCY_P50_MS and worst_p99 <= MAX_LATENCY_P99_MS
                  and delivery >= MIN_FRAME_DELIVERY)
            print(f"{'PASS' if ok else 'FAIL'} source->render p50 {worst_p50:.1f}ms / "
                  f"p99 {worst_p99:.1f}ms (budget {MAX_LATENCY_P50_MS:.1f}/{MAX_LATENCY_P99_MS:.1f}, "
                  f"of which {ZOOM_VIDEO_RESERVE_MS:.0f} is the lip-sync hold), "
                  f"{delivery:.0%} of decoded frames delivered "
                  f"(worst window {worst_delivery:.0%}, {len(steady)} steady windows)")
            if worst_p50 > MAX_LATENCY_P50_MS:
                failures.append(
                    f"source->render p50 {worst_p50:.1f}ms exceeds {MAX_LATENCY_P50_MS:.1f}ms "
                    f"(the deliberate {ZOOM_VIDEO_RESERVE_MS:.0f}ms lip-sync hold plus one frame) — "
                    f"the path above the hold has gained latency")
            if worst_p99 > MAX_LATENCY_P99_MS:
                failures.append(
                    f"source->render p99 {worst_p99:.1f}ms exceeds {MAX_LATENCY_P99_MS:.1f}ms "
                    f"(the {ZOOM_VIDEO_RESERVE_MS:.0f}ms lip-sync hold plus two frames)")
            if delivery < MIN_FRAME_DELIVERY:
                # Still a FAILURE either way — this run proved nothing and must
                # not read green. Only the attribution changes.
                if harness_short:
                    failures.append(
                        f"the HARNESS could not source the load: {sourced_fps:.0f} of "
                        f"{requested_fps:.0f} frames/s reached the core "
                        f"({sourced_fps / requested_fps:.0%}), so the {delivery:.0%} "
                        f"delivery figure is NOT attributable to the core — re-run on "
                        f"hardware that can sustain {args.load}x1080p60")
                else:
                    failures.append(
                        f"only {delivery:.0%} of decoded frames reached the compositor — "
                        f"sources lose motion even though fps still reads 60")
    ingest_lines = [l for l in core.stderr if "[zoom-ingest]" in l]
    if ingest_lines:
        print("Zoom ingest (last 3 windows):")
        for line in ingest_lines[-3:]:
            print(f"  {line}")
    # Slot accounting closes the books on the delivery number: `overwritten` is
    # real lost motion (a decoded frame destroyed before the compositor took it),
    # `starved` is a render tick that re-served a frame it already had. A delivery
    # miss is one, the other, or both — and they have different fixes.
    slot_lines = [l for l in core.stderr if "[zoom-slot]" in l]
    if slot_lines:
        print("Ingest->render slot accounting (last 3 windows):")
        for line in slot_lines[-3:]:
            print(f"  {line}")
    if stage_lines:
        print("Stage attribution (last 3 windows):")
        for line in stage_lines[-3:]:
            print(f"  {line}")

    core.kill()

    if failures:
        print("\nNative pipeline diagnostics:")
        for line in core.stderr:
            if "[compositor]" in line or "shader compile failed" in line:
                print(f"  {line}")
        print("\nSHOW DRILL FAILED")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("\nSHOW DRILL PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
