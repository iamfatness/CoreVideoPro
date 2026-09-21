#!/usr/bin/env python3
"""media-take-ab: does a clip CUED in Preview roll from its FIRST FRAME the
instant it is Taken to Program, with no placeholder / slate frame in between?

The media twin of `scripts/qa/zoom-gap-hold-ab.py`, and for the same reason:
nothing on the wire carries per-layer pixels (`RenderedProgramSources` publishes
ids only, `programPixelSignature` is 0 on the display tick), so the ONLY honest
judge of "what did Program show on the take tick" is the recorded Program MP4
read back frame by frame with `ffmpeg signalstats` YAVG.

#535 slice 3b moved media play/pause/cue state into the core (`MediaTransports`,
one entry per `media:<assetId>`; a go-live is an in-place Resume on the SAME
decoder). The operator-visible promise that buys is: a cued clip does not
cold-start when it is taken. This script is the oracle for that promise.

Flow (phases, all logged into <label>.jsonl):
  configure  101 on Program (fake engine), the clip CUED on Preview as one
             fixed full-canvas media route -> `mediaSources` expects `cued`
  recording  start-program-output + set-recording-targets + start-recording-session
  take       begin-take-transition (cut, 0ms) + load-scene-graph <clip> +
             set-preview-scene <101> -> `mediaSources` expects `live`
  judge      find the TAKE FRAME in Program.mp4 = the first frame whose YAVG
             leaves the 101 fixture's range, then assert what is actually there.

The judge (every threshold is separated from every other on purpose):
  * 101 fixture (the animated fake-engine pattern) .... YAVG ~188-205
  * the clip's first second (a flat neutral grey) ...... YAVG measured from the
    generated clip file itself, independently of the product; required to sit at
    least `--min-separation` away from every confounder below, or the run is a
    HARNESS failure, because a judge that cannot tell the answers apart judges
    nothing
  * bus-health warming slate (`kWarmingSlateRgba`) .... YAVG ~42 as recorded
  * bus-health failed slate (`kFailedSlateRgba`) ...... YAVG ~39 as recorded
  * dropout black (`kDropoutBlackRgba`) ............... YAVG ~16
    (CLAUDE.md quotes the slates' FULL-range lumas, 30 and 23; a limited-range
     recording carries them as 16 + 219/255*Y - see CONFUSERS below)
A cold start (the #449 flash the slice-3b hand-off exists to stop) lands the
take frame on a slate/black, which is ~70+ YAVG away from the clip's colour: it
CANNOT pass. A take frame that was never found is a FAIL, never a quiet pass.

Usage:
  python scripts/qa/media-take-ab.py --core native/build-dev/corevideo-native.exe \
      --fake native/build-dev/corevideo-zoom-engine-fake.exe --label s3b
Writes <label>.jsonl (one snapshot slice per poll, incl. every mediaSources row),
<label>.stderr.log, <label>-clip.mp4, <label>.yavg.txt and
rec-<label>/<session>/Program.mp4, prints a phase summary and PASS/FAIL, archives
the artifacts under artifacts/qa/slice3b/, and exits non-zero on FAIL.
"""
import argparse, glob, json, os, re, shutil, subprocess, sys, threading, time

# ---------------------------------------------------------------- core driver
# (Core / fixed / rec / the phase-summary printer are the zoom-gap-hold-ab.py
#  helpers; keep them in step with that file.)
class Core:
    def __init__(self, path, env):
        self.proc = subprocess.Popen([path], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, text=True, bufsize=1,
                                     env={**os.environ, **env})
        self.responses, self.events, self.stderr = {}, [], []
        self.n = 1
        threading.Thread(target=self._out, daemon=True).start()
        threading.Thread(target=self._err, daemon=True).start()
    def _out(self):
        for line in self.proc.stdout:
            try: o = json.loads(line)
            except json.JSONDecodeError: continue
            if o.get("id"): self.responses[o["id"]] = o
            elif o.get("type"): self.events.append(o)
    def _err(self):
        for line in self.proc.stderr: self.stderr.append(line.rstrip())
    def request(self, body, timeout=8.0):
        rid = f"ab-{self.n}"; self.n += 1
        self.proc.stdin.write(json.dumps(dict(body, id=rid), separators=(",", ":")) + "\n")
        self.proc.stdin.flush()
        dl = time.time() + timeout
        while time.time() < dl:
            if rid in self.responses: return self.responses[rid]
            time.sleep(0.002)
        return None
    def sync(self, commands, elapsed): return self.request({"type": "media-core-sync", "elapsedMs": elapsed, "commands": commands})
    def spine(self, subs, elapsed):
        payload = {"subscriptions": [{"participantId": p, "kind": "participant-video", "purpose": pu, "priority": i}
                                     for i, (p, pu) in enumerate(subs)],
                   "sourceParticipantIds": [p for p, _ in subs], "summary": "ab"}
        return self.request({"type": "zoom-media-spine-sync", "elapsedMs": elapsed, "payload": payload})
    def kill(self): self.proc.kill()

def fixed(route_id, pid):
    return {"routeId": route_id, "mode": "fixed", "participantId": pid, "audioRole": "mix",
            "fitMode": "fill", "opacity": 1.0, "zIndex": 0, "rect": {"x": 0, "y": 0, "width": 1, "height": 1}}

def media_route(route_id, asset_id, path):
    """A slice-3b media route: the asset fields + a full-canvas rect, and
    DELIBERATELY no `mediaPlaybackKey` / `mediaAssetPlaying` - those are retired
    (the core ignores them; sending them would misrepresent what a 3b shell does)."""
    return {"routeId": route_id, "mode": "fixed", "audioRole": "mix",
            "mediaAssetId": asset_id, "mediaAssetName": "QA clip",
            "mediaAssetKind": "video", "mediaAssetPath": path, "mediaAssetLoop": False,
            "fitMode": "fill", "opacity": 1.0, "zIndex": 0,
            "rect": {"x": 0, "y": 0, "width": 1, "height": 1}}

def slice_(snap, t, phase):
    pf = snap.get("programFrame", {}) or {}
    tr = snap.get("takeRecords", {}) or {}
    return {"t": round(t, 3), "phase": phase,
            "pgmSources": pf.get("videoSources"), "layerCount": pf.get("layerCount"),
            "renderPlanId": pf.get("renderPlanId"), "frameNumber": pf.get("frameNumber"),
            "health": pf.get("health"), "warnings": pf.get("warnings"),
            "sceneId": pf.get("sceneId"), "attr": pf.get("sceneIdAttribution"),
            "busSources": [(s.get("sourceId"), s.get("width"), s.get("height"), s.get("health"))
                           for s in (snap.get("sources") or [])],
            "mediaSources": [(m.get("sourceId"), m.get("state"), m.get("onProgram"), m.get("onPreview"),
                              m.get("loop"), m.get("positionMs"), m.get("durationMs"))
                             for m in (snap.get("mediaSources") or [])],
            "takeCount": tr.get("count"), "takePending": tr.get("pending"),
            "lastTake": (tr.get("records") or [None])[-1]}

# -------------------------------------------------------------------- ffmpeg
def resolve_tool(explicit, name):
    """--ffmpeg <dir-or-exe>, else COREVIDEO_FFMPEG_BIN_DIR, else PATH. Fails
    LOUDLY with the paths it tried (the #473 lesson: a missing tool must name
    itself, never degrade into an unexplained failure downstream)."""
    exe = name + (".exe" if os.name == "nt" else "")
    cands = []
    if explicit:
        cands.append(explicit if explicit.lower().endswith(".exe") else os.path.join(explicit, exe))
    bin_dir = os.environ.get("COREVIDEO_FFMPEG_BIN_DIR")
    if bin_dir:
        cands.append(os.path.join(bin_dir, exe))
    found = shutil.which(name)
    if found:
        cands.append(found)
    for c in cands:
        if c and os.path.isfile(c):
            return c
    raise SystemExit("FAIL: %s not found. Tried: %s (pass --ffmpeg <dir>, or set "
                     "COREVIDEO_FFMPEG_BIN_DIR, currently %r)" % (name, cands or "<nothing>", bin_dir))

def run(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True)
    return p.returncode, p.stdout, p.stderr

CLIP_COLOUR = "0x505050"
def generate_clip(ffmpeg, out, seconds_solid=1, seconds_pattern=2):
    """3s / 1280x720 / 30fps H.264 yuv420p: the FIRST second is one flat colour
    (the take-frame signature), then testsrc2 moves (proof it ROLLS and is not a
    frozen poster). Plain H.264 yuv420p so Media Foundation decodes it without
    falling back to FFmpeg.

    The colour is a NEUTRAL GREY, and both halves of that are measurements, not
    taste. Measured on this rig 2026-09-20 with the brief's saturated
    `0x10A0F0`: the clip file reads YAVG 124.0 but the same pixels come back out
    of Program at 116.0 — an 8-unit shift, which is what a BT.601/BT.709 matrix
    difference does to a SATURATED colour and (because the errors cancel over
    many hues) does NOT do to an averaged pattern. A grey has the same luma in
    either matrix, so the clip file's own YAVG stays a usable ground truth for
    what Program must show. Second measurement, same run: testsrc2's mean YAVG
    is 126.0 — two units from 124.0 — so with that colour the "did it roll"
    check could not distinguish a rolling clip from a frozen poster at all.
    0x505050 sits ~40 from testsrc2's mean, ~55 from the slates and ~55 from the
    fake engine's participant pattern: every question this judge asks has a
    different answer."""
    # An H.264 ENCODER ladder, not a hard-coded libx264: the ffmpeg on this rig
    # (C:\ffmpeg\bin) is a build WITHOUT libx264, and a fixture generator that
    # dies on the first encoder it thought of is a harness failure dressed up as
    # a product one. Every rung produces plain H.264 yuv420p, which is what
    # Media Foundation needs to decode it without the FFmpeg fallback.
    last = ""
    for enc, extra in (("libx264", ["-preset", "veryfast"]), ("libopenh264", []),
                       ("h264_nvenc", []), ("h264_mf", []), ("h264_qsv", []), ("h264_amf", [])):
        code, _, err = run([ffmpeg, "-y", "-hide_banner", "-loglevel", "error",
                            "-f", "lavfi", "-i", "color=c=%s:s=1280x720:r=30:d=%d" % (CLIP_COLOUR, seconds_solid),
                            "-f", "lavfi", "-i", "testsrc2=s=1280x720:r=30:d=%d" % seconds_pattern,
                            "-filter_complex", "[0:v][1:v]concat=n=2:v=1:a=0",
                            "-c:v", enc] + extra + ["-pix_fmt", "yuv420p", "-r", "30", out])
        if code == 0 and os.path.isfile(out):
            print("[clip] encoded with %s" % enc)
            return os.path.abspath(out)
        last = "%s: %s" % (enc, err[-600:])
    raise SystemExit("FAIL: could not generate the fixture clip with %s - no usable H.264 encoder.\n%s"
                     % (ffmpeg, last))

def yavg_series(ffmpeg, path, metafile):
    """Per-frame YAVG, the same instrument zoom-gap-hold-ab.py judges with.

    `metafile` MUST be a plain RELATIVE filename: it is spliced into an ffmpeg
    FILTER description, where a Windows absolute path's ':' and backslash are option
    and escape syntax ("No option name near ...") rather than a path."""
    if os.path.isabs(metafile) or ":" in metafile:
        raise SystemExit("FAIL (harness): the signalstats metadata file must be a relative name, got %r" % metafile)
    if os.path.isfile(metafile):
        os.remove(metafile)
    code, _, err = run([ffmpeg, "-y", "-hide_banner", "-loglevel", "error", "-i", path,
                        "-vf", "signalstats,metadata=print:key=lavfi.signalstats.YAVG:file=%s" % metafile,
                        "-f", "null", "-"])
    if code != 0:
        raise SystemExit("FAIL: signalstats could not read %s\n%s" % (path, err[-2000:]))
    vals, times = [], []
    with open(metafile, "r", errors="replace") as f:
        for line in f:
            m = re.search(r"pts_time:([0-9.]+)", line)
            if m:
                times.append(float(m.group(1)))
            m = re.search(r"lavfi\.signalstats\.YAVG=([0-9.]+)", line)
            if m:
                vals.append(float(m.group(1)))
    n = min(len(vals), len(times))
    return vals[:n], times[:n]

# The confounders, AS THE RECORDING MEASURES THEM. CLAUDE.md quotes the slates
# as YAVG ~30 / ~23, which are their FULL-RANGE lumas; Program.mp4 is encoded
# limited range (16..235), so the same pixels come back 16 + 219/255 * Y:
# warming 0xff1b1f27 -> 42, failed 0xff23181c -> 39, black -> 16. Measured on
# this rig 2026-09-20: a --skip-cue control's cold-start frames read EXACTLY
# 42.00, which is what proved the arithmetic rather than the other way round.
CONFUSERS = [("a bus-health slate - warming (kWarmingSlateRgba, ~42 as recorded)", 42.0),
             ("a bus-health slate - failed (kFailedSlateRgba, ~39 as recorded)", 39.0),
             ("dropout black (kDropoutBlackRgba)", 16.0)]

def classify(y, expect, fixture_lo, fixture_hi):
    # The two slates are 3 apart as recorded, so a value near either is named as
    # "a slate" with the closer one first - never as a confident single answer.
    near = [(abs(y - v), name) for name, v in CONFUSERS if abs(y - v) <= 6]
    if near:
        return sorted(near)[0][1]
    if fixture_lo <= y <= fixture_hi:
        return "what was on Program before the take (still on air)"
    if abs(y - expect) <= 12:
        return "the clip"
    return "unknown content"

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--core", required=True)
    ap.add_argument("--fake", required=True)
    ap.add_argument("--label", required=True)
    ap.add_argument("--clip", default=None, help="an existing clip to use instead of generating one")
    ap.add_argument("--ffmpeg", default=None, help="ffmpeg bin dir or exe (default: COREVIDEO_FFMPEG_BIN_DIR, then PATH)")
    ap.add_argument("--poll-ms", type=int, default=50)
    ap.add_argument("--tolerance", type=float, default=6.0, help="+/- YAVG around the clip colour (default 6)")
    ap.add_argument("--min-separation", type=float, default=25.0,
                    help="required YAVG distance between the clip colour and every confounder (default 25)")
    ap.add_argument("--skip-cue", action="store_true",
                    help="THE FALSIFICATION CONTROL (the --force-raw of validate-gpu-encode.mjs): cue 102 "
                         "instead of the clip, so the clip is cut to Program with NO warm decoder. The cue "
                         "hand-off cannot apply, the clip cold-starts, and this oracle MUST fail - a run "
                         "that passes with --skip-cue is judging nothing.")
    ap.add_argument("--baseline-seconds", type=float, default=0.5,
                    help="how much of the recording's head is the pre-take Program baseline (default 0.5s)")
    ap.add_argument("--post-take-frames", type=int, default=15)
    a = ap.parse_args()

    # ABSOLUTE, always: a relative path with forward slashes reaches
    # CreateProcess as a command line it cannot parse ("The system cannot find
    # the file specified" for a file that plainly exists), and the fake engine
    # path is read by the CORE, whose working directory is not ours.
    for name in ("core", "fake"):
        v = getattr(a, name)
        if not os.path.isfile(v):
            raise SystemExit("FAIL: --%s not found: %s" % (name, os.path.abspath(v)))
        setattr(a, name, os.path.abspath(v))

    ffmpeg = resolve_tool(a.ffmpeg, "ffmpeg")
    print("[%s] ffmpeg: %s" % (a.label, ffmpeg))
    clip = os.path.abspath(a.clip) if a.clip else generate_clip(ffmpeg, "%s-clip.mp4" % a.label)
    if not os.path.isfile(clip):
        raise SystemExit("FAIL: clip not found: %s" % clip)
    print("[%s] clip:   %s" % (a.label, clip))

    # GROUND TRUTH, measured from the fixture file itself and never from the
    # product: what luma does the clip's first second actually carry?
    clip_y, clip_t = yavg_series(ffmpeg, clip, "%s.clip.yavg.txt" % a.label)
    first_second = [y for y, t in zip(clip_y, clip_t) if t < 0.9]
    if not first_second:
        raise SystemExit("FAIL: could not measure the clip's first second (no frames under 0.9s)")
    expect = sum(first_second) / len(first_second)
    spread = max(first_second) - min(first_second)
    print("[%s] clip first-second YAVG = %.2f (spread %.2f over %d frames)"
          % (a.label, expect, spread, len(first_second)))
    if spread > 2.0:
        raise SystemExit("FAIL (harness): the clip's first second is not flat (spread %.2f); "
                         "it cannot be a take-frame signature" % spread)
    for name, v in CONFUSERS:
        if abs(expect - v) < a.min_separation:
            raise SystemExit("FAIL (harness): the clip colour (YAVG %.1f) is only %.1f from %s (%s); "
                             "this judge could not tell them apart" % (expect, abs(expect - v), name, v))

    env = {"COREVIDEO_ZOOM_ENGINE_PATH": a.fake, "COREVIDEO_FAKE_ENGINE_PARTICIPANTS": "3",
           "COREVIDEO_FAKE_ENGINE_RES": "2", "COREVIDEO_FAKE_ENGINE_FPS": "60",
           "COREVIDEO_FAKE_NO_CHURN": "1", "COREVIDEO_FAKE_ENGINE_AUTOSUBSCRIBE": "0"}
    core = Core(a.core, env)
    time.sleep(1.5)
    out = open("%s.jsonl" % a.label, "w")
    t0 = time.time()
    def el():
        return int((time.time() - t0) * 1000) + 1000
    def rec(phase, resp):
        if resp is None:
            print("[%s] %s: TIMEOUT" % (a.label, phase))
            return None
        snap = resp.get("snapshot") or {}
        row = slice_(snap, time.time() - t0, phase)
        out.write(json.dumps(row) + "\n")
        out.flush()
        return row

    assert core.sync([{"type": "set-verbose-diagnostics", "enabled": True}], el()) is not None
    j = core.request({"type": "zoom-join", "payload": {"meetingNumber": "1234567890", "displayName": "ab"}}, timeout=20)
    assert j is not None, "join timeout"
    time.sleep(3.0)

    # CONFIGURE: 101 on Program, the clip CUED on Preview.
    core.spine([("101", "program"), ("102", "preview")], el())
    rec("configure", core.sync([
        {"type": "load-scene-graph", "sceneId": "pgm", "routes": [fixed("pgm-0", "101")]},
        {"type": "set-preview-scene", "sceneId": "pvw",
         "routes": [fixed("pvw-0", "102")] if a.skip_cue else [media_route("pvw-0", "clip", clip)]}], el()))
    if a.skip_cue:
        print("[%s] --skip-cue: the clip is NOT cued; this run is the falsification control and must FAIL" % a.label)
    cued_seen = False
    for _ in range(40):
        core.spine([("101", "program"), ("102", "preview")], el())
        row = rec("cued", core.sync([], el()))
        if row and any(m[0] == "media:clip" and m[1] == "cued" for m in (row["mediaSources"] or [])):
            cued_seen = True
        time.sleep(a.poll_ms / 1000)

    # RECORD (same three commands as the gap harness).
    folder = os.path.abspath("rec-%s" % a.label)
    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(folder, exist_ok=True)
    rec("record-start", core.sync([
        {"type": "start-program-output", "destinations": ["recording"], "isoSourceIds": [], "isoParticipantIds": []},
        {"type": "set-recording-targets", "targetFolder": folder, "filenamePrefix": "ab", "format": "mp4",
         "quality": "high", "isoSourceIds": [], "isoParticipantIds": []},
        {"type": "start-recording-session", "sessionId": "ab"}], el()))
    for i in range(20):
        if i % 4 == 0:
            core.spine([("101", "program"), ("102", "preview")], el())
        rec("recording", core.sync([], el()))
        time.sleep(a.poll_ms / 1000)

    # TAKE: the clip to Program, 101 back to Preview. One sync, exactly as the
    # shell's Take sends it.
    rec("take", core.sync([
        {"type": "begin-take-transition", "operationId": "ab-take-1", "revision": 1,
         "mode": "cut", "durationMs": 0, "direction": "auto", "dipColor": "#000000"},
        {"type": "load-scene-graph", "sceneId": "pvw", "routes": [media_route("pgm-0", "clip", clip)]},
        {"type": "set-preview-scene", "sceneId": "pgm", "routes": [fixed("pvw-0", "101")]}], el()))
    core.spine([("101", "preview")], el())
    live_seen = False
    for i in range(60):
        if i % 4 == 0:
            core.spine([("101", "preview")], el())
        row = rec("after", core.sync([], el()))
        if row and any(m[0] == "media:clip" and m[1] == "live" for m in (row["mediaSources"] or [])):
            live_seen = True
        time.sleep(a.poll_ms / 1000)

    rec("record-stop", core.sync([{"type": "stop-recording-session", "reason": "ab done"}], el()))
    time.sleep(1.0)
    rec("encoder-stop", core.sync([{"type": "stop-encoder-session", "reason": "ab done"}], el()))
    time.sleep(2.0)
    out.close()
    core.request({"type": "zoom-leave", "payload": {}}, timeout=5)
    core.kill()

    # ------------------------------------------------------------- summary
    rows = [json.loads(l) for l in open("%s.jsonl" % a.label)]
    print("\n=== %s: %d slices ===" % (a.label, len(rows)))
    last = None
    for r in rows:
        bus = tuple(tuple(b) for b in (r["busSources"] or []))
        med = tuple(tuple(m) for m in (r["mediaSources"] or []))
        key = (r["phase"], tuple(json.dumps(x, sort_keys=True) for x in (r["pgmSources"] or [])),
               r["layerCount"], bus, med, r["health"], tuple(r["warnings"] or []), r["sceneId"], r["attr"])
        if key != last:
            print("%7.3f %-10s scene=%s/%s pgm=%s layers=%s health=%s media=%s bus=%s warn=%s takes=%s/%s"
                  % (r["t"], r["phase"], r["sceneId"], r["attr"],
                     [x.get("participantId") or x.get("sourceId") for x in (r["pgmSources"] or [])],
                     r["layerCount"], r["health"], med, bus, r["warnings"], r["takeCount"], r["takePending"]))
            last = key
    open("%s.stderr.log" % a.label, "w").write("\n".join(core.stderr))
    errs = [l for l in core.stderr if "error" in l.lower() or "warn" in l.lower()]
    print("stderr lines: %d (errors/warnings: %d)" % (len(core.stderr), len(errs)))
    for l in errs[:10]:
        print("  ", l[:200])
    print("mediaSources reached cued=%s live=%s" % (cued_seen, live_seen))

    # --------------------------------------------------------------- judge
    fails = []
    if not cued_seen and not a.skip_cue:
        fails.append("mediaSources never reported the clip `cued` while it was only on Preview")
    if not live_seen:
        fails.append("mediaSources never reported the clip `live` after the Take")

    prog = sorted(glob.glob(os.path.join(folder, "*", "Program.mp4")))
    if not prog:
        fails.append("no Program.mp4 was recorded under %s" % folder)
        return verdict(a, fails, None)
    program = prog[-1]
    ys, ts = yavg_series(ffmpeg, program, "%s.yavg.txt" % a.label)
    print("program: %s (%d frames)" % (program, len(ys)))
    if len(ys) < 60:
        fails.append("the recording is too short to judge (%d frames)" % len(ys))
        return verdict(a, fails, program)
    fps = (len(ys) - 1) / (ts[-1] - ts[0]) if ts[-1] > ts[0] else 60.0
    print("recorded rate: %.2f fps; YAVG range %.2f..%.2f" % (fps, min(ys), max(ys)))

    # The pre-take Program baseline is MEASURED from the recording's own head,
    # never hard-coded: the fake engine's participant pattern is a sawtooth
    # whose level depends on which participant is on air (101 reads ~141-157
    # here; 103 reads ~188-205 in zoom-gap-hold-ab.py), and a judge carrying a
    # hard-coded band silently mis-locates the take frame when that changes.
    base = [y for y, t in zip(ys, ts) if t - ts[0] < a.baseline_seconds]
    if len(base) < 15:
        fails.append("only %d frames in the first %.2fs: no pre-take baseline to judge against"
                     % (len(base), a.baseline_seconds))
        return verdict(a, fails, program)
    margin = max(4.0, 0.2 * (max(base) - min(base)))
    fixture_lo, fixture_hi = min(base) - margin, max(base) + margin
    print("pre-take Program baseline: %.2f..%.2f over %d frames -> fixture band [%.2f, %.2f]"
          % (min(base), max(base), len(base), fixture_lo, fixture_hi))
    for edge in (fixture_lo, fixture_hi):
        if abs(expect - edge) < a.min_separation:
            fails.append("HARNESS: the clip colour (YAVG %.2f) is only %.2f from the pre-take Program band "
                         "[%.2f, %.2f]; this judge could not tell the clip from what was already on air"
                         % (expect, abs(expect - edge), fixture_lo, fixture_hi))
            return verdict(a, fails, program)

    # The take frame is the FIRST frame that leaves that band. No run-length
    # smoothing: a SINGLE flashed placeholder frame is exactly the defect this
    # exists to catch, and smoothing would hide it.
    take = None
    for i, y in enumerate(ys):
        if i < len(base):
            continue  # the baseline itself is by definition pre-take
        if not (fixture_lo <= y <= fixture_hi):
            take = i
            break
    if take is None:
        fails.append("NO TAKE FRAME FOUND: every one of the %d recorded frames stayed inside the pre-take "
                     "Program band [%.2f, %.2f] (YAVG %.2f..%.2f) - Program never changed, so the Take never "
                     "reached air" % (len(ys), fixture_lo, fixture_hi, min(ys), max(ys)))
        return verdict(a, fails, program)
    pre = ["%.1f" % p for p in ys[max(0, take - 5):take]]
    print("take frame: #%d at t=%.3fs  YAVG=%.2f (pre-take %s)" % (take, ts[take], ys[take], pre))

    window = ys[take:take + a.post_take_frames]
    print("post-take %d frames YAVG: %s" % (len(window), ["%.2f" % y for y in window]))
    print("expected the clip's first second: %.2f +/- %.1f" % (expect, a.tolerance))
    if len(window) < a.post_take_frames:
        fails.append("only %d frames were recorded after the take frame (wanted %d)"
                     % (len(window), a.post_take_frames))
    for k, y in enumerate(window):
        if abs(y - expect) > a.tolerance:
            idx = take + k
            fails.append("frame #%d (t=%.3fs, %d after the take) YAVG=%.2f, expected %.2f+/-%.1f (the clip's "
                         "first second) - that reads as %s"
                         % (idx, ts[idx], k, y, expect, a.tolerance,
                            classify(y, expect, fixture_lo, fixture_hi)))

    # ...and it must ROLL, not sit on a frozen poster: past the clip's first
    # second the moving testsrc2 pattern must leave the flat colour. Expressed
    # in TIME, not in raw frame offsets, because PROGRAM records at ~60fps while
    # the clip is 30fps - the brief's +45..+75 offsets straddle the 1s colour
    # boundary at 60fps. t+1.2..2.0s is unambiguously inside the moving pattern
    # and ends a second before the 3s clip does.
    roll = [(i, y) for i, y in enumerate(ys) if 1.2 <= ts[i] - ts[take] <= 2.0]
    moved = [(i, y) for i, y in roll if abs(y - expect) > 10.0]
    print("rolling window t+1.2..2.0s: %d frames, %d of them >10 YAVG from the flat colour (%s..%s)"
          % (len(roll), len(moved),
             "%.2f" % min([y for _, y in roll]) if roll else "-",
             "%.2f" % max([y for _, y in roll]) if roll else "-"))
    if not roll:
        fails.append("the recording ended before t+2.0s, so the rolling check could not run")
    elif len(moved) * 2 < len(roll):
        fails.append("the clip did NOT roll: only %d of %d frames in t+1.2..2.0s left the flat first-second "
                     "colour (%.2f); a clip held on its poster looks exactly like this"
                     % (len(moved), len(roll), expect))
    return verdict(a, fails, program)

def verdict(a, fails, program):
    dest = os.path.abspath(os.path.join("artifacts", "qa", "slice3b"))
    os.makedirs(dest, exist_ok=True)
    # Every archived name carries the label: two runs (a real one and a
    # --skip-cue control) must not overwrite each other's evidence.
    for f in ["%s.jsonl" % a.label, "%s.stderr.log" % a.label, "%s.yavg.txt" % a.label,
              "%s.clip.yavg.txt" % a.label, "%s-clip.mp4" % a.label, program]:
        if f and os.path.isfile(f):
            name = os.path.basename(f)
            if not name.startswith(a.label):
                name = "%s-%s" % (a.label, name)
            try:
                shutil.copy2(f, os.path.join(dest, name))
            except OSError as e:
                print("  (could not archive %s: %s)" % (f, e))
    print("artifacts: %s" % dest)
    if fails:
        print("\nFAIL (%d):" % len(fails))
        for f in fails:
            print("  - %s" % f)
        sys.exit(1)
    print("\nPASS: the cued clip was on air from its first frame, with no placeholder or slate in between.")
    sys.exit(0)

if __name__ == "__main__":
    main()
