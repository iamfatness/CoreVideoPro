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
MAX_COMMAND_P99_MS = 100.0
MAX_LATENCY_P50_MS = 33.3   # 2 frames at 60p (1 of them is the frame-sync cushion)
MAX_LATENCY_P99_MS = 50.0   # 3 frames at 60p
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


class Core:
    def __init__(self, path, env=None):
        self.proc = subprocess.Popen(
            [path], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, bufsize=1,
            env={**os.environ, **(env or {})})
        self.responses = {}
        self.events = []
        self.stderr = []
        self.next_id = 1
        self.round_trips = []  # (type, seconds) per answered request
        threading.Thread(target=self._read_stdout, daemon=True).start()
        threading.Thread(target=self._read_stderr, daemon=True).start()

    def _read_stdout(self):
        for line in self.proc.stdout:
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue
            if obj.get("id"):
                self.responses[obj["id"]] = obj
            elif obj.get("type"):
                self.events.append(obj)

    def _read_stderr(self):
        for line in self.proc.stderr:
            self.stderr.append(line.rstrip())

    def request(self, body, timeout=6.0):
        rid = f"drill-{self.next_id}"
        self.next_id += 1
        body = dict(body, id=rid)
        started = time.time()
        self.proc.stdin.write(json.dumps(body, separators=(",", ":")) + "\n")
        self.proc.stdin.flush()
        deadline = started + timeout
        # 1ms poll: this now measures OPERATOR RESPONSIVENESS (the round trip a
        # Take rides), so a 10ms poll would be most of the number being measured.
        while time.time() < deadline:
            if rid in self.responses:
                self.round_trips.append((body.get("type", "?"), time.time() - started))
                return self.responses[rid]
            time.sleep(0.001)
        return None

    def sync(self, commands, elapsed_ms):
        return self.request({"type": "media-core-sync", "elapsedMs": elapsed_ms,
                             "commands": commands})

    def kill(self):
        self.proc.kill()


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


def wall_sources(sources):
    return [{"sourceId": f"zoom:{101 + i}", "kind": "zoom",
             "participantId": str(101 + i), "slot": i,
             "label": f"CAM {i + 1}"} for i in range(sources)]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--seconds", type=float, default=40.0)
    parser.add_argument("--core", default=DEFAULT_CORE)
    parser.add_argument("--load", type=int, default=0,
                        help="synthesize N 1080p Zoom feeds (the production wall)")
    args = parser.parse_args()

    if not os.path.exists(args.core):
        print(f"FAIL core binary not found: {args.core}")
        return 1
    if args.load and not os.path.exists(FAKE_ENGINE):
        print(f"FAIL fake engine not built: {FAKE_ENGINE}")
        return 1

    failures = []
    core = Core(args.core, env=load_env(args.load) if args.load else None)
    time.sleep(1.5)

    if args.load:
        joined = core.request({"type": "zoom-join", "payload": {
            "meetingNumber": "1234567890", "displayName": "show-drill"}}, timeout=20.0)
        if joined is None:
            print("FAIL zoom-join timed out")
            core.kill()
            return 1
        print(f"LOAD  {args.load} x 1080p synthetic feeds joining…")
        time.sleep(4.0)  # engine spin-up + first frames on every source

    timeouts = 0

    def step(name, commands, elapsed):
        nonlocal timeouts
        response = core.sync(commands, elapsed)
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
    step("configure", configure, 500)

    # 2. Operator traffic at the cadence the shell produces, for the whole run:
    #    2Hz snapshot syncs plus scene re-cues, while the render loop is live.
    start = time.time()
    tick = 0
    while time.time() - start < args.seconds:
        tick += 1
        elapsed = int((time.time() - start) * 1000) + 1000
        if tick % 5 == 0:
            # Re-cue preview (the solo-scene path) — the traffic that flooded.
            solo = "mv-solo-a" if tick % 10 == 0 else "mv-solo-b"
            step("recue", [{"type": "set-preview-scene", "sceneId": solo,
                            "routes": [route("solo-0")]}], elapsed)
        else:
            step("poll", [], elapsed)
        time.sleep(0.5)

    # 3. Record a short session and confirm the artifact is real.
    folder = os.path.join(REPO, "artifacts", "show-drill")
    os.makedirs(folder, exist_ok=True)
    step("record-start", [
        {"type": "start-program-output", "destinations": ["recording"],
         "isoSourceIds": [], "isoParticipantIds": []},
        {"type": "set-recording-targets", "targetFolder": folder,
         "filenamePrefix": "drill", "format": "mp4", "quality": "high",
         "isoSourceIds": [], "isoParticipantIds": []},
        {"type": "start-recording-session", "sessionId": "drill"},
    ], 60000)
    time.sleep(6)
    # Stop the SESSION first so the writer flushes its moov box, then let the
    # file settle before probing. Killing the core (below) is a hard kill: an
    # unfinalised mp4 has no moov atom and is unplayable, no matter how many
    # bytes are on disk.
    stop = step("record-stop", [{"type": "stop-recording-session",
                                 "reason": "drill complete"}], 66000)
    step("encoder-stop", [{"type": "stop-encoder-session",
                           "reason": "drill complete"}], 67000)
    time.sleep(2.5)

    # ── assertions ───────────────────────────────────────────────────────────
    snapshot = (stop or {}).get("snapshot", {})

    recording = snapshot.get("recording", {})
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
    else:
        failures.append(f"recording produced no artifact (path={artifact or 'none'})")

    multiview = snapshot.get("multiviewSharedTexture") or {}
    roles = {tile.get("role") for tile in multiview.get("tiles", [])}
    if {"pgm", "pvw"} <= roles:
        print("PASS multiview wall has PGM and PVW cells")
    else:
        failures.append(f"multiview missing bus cells (roles={sorted(roles)})")

    over = 0
    total = 0
    for line in core.stderr:
        if "lock-guardrail" in line and "over-budget" in line:
            parts = line.split("over-budget ")[1].split(" of ")
            over = int(parts[0])
            total = int(parts[1].split(" ")[0])
    if total:
        ratio = over / total
        verdict = "PASS" if ratio <= MAX_OVER_BUDGET_RATIO else "FAIL"
        print(f"{verdict} coreMutex over-budget {over}/{total} ({ratio:.0%})")
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
            last = windows[-1]
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
                  f"p99 {worst_p99:.1f}ms, {delivery:.0%} of decoded frames delivered "
                  f"(worst window {worst_delivery:.0%}, {len(steady)} steady windows)")
            if worst_p50 > MAX_LATENCY_P50_MS:
                failures.append(
                    f"source->render p50 {worst_p50:.1f}ms exceeds two frames "
                    f"(one is the deliberate frame-sync cushion) — the internal "
                    f"path is no longer hardware-competitive")
            if worst_p99 > MAX_LATENCY_P99_MS:
                failures.append(
                    f"source->render p99 {worst_p99:.1f}ms exceeds three frames")
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
        print("\nSHOW DRILL FAILED")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("\nSHOW DRILL PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
