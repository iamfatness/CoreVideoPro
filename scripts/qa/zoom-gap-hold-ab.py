#!/usr/bin/env python3
"""zoom-gap-hold-ab: does PROGRAM hold a routed Zoom participant's last picture
across a subscription gap, or fall to the slate/black per its dropout policy?

Drives the headless core with the fake engine: 101 on program / 102 on preview,
cue a brand-new participant (103), Take, then keep 103 routed on program while
the subscription list omits it for ~450 ms (a video-budget eviction or spine
churn around a Take) and restores it. The program is RECORDED to MP4 around the
gap, because nothing on the wire carries per-layer pixels or geometry: judge it
with ffmpeg signalstats (YAVG per frame). Found the #535 slice-1 regression on
2026-09-19 (slate for the whole gap; the pre-bus store held the frame).

Slice 4a (2026-09-19) retired the old pink-tile dropout signature. The two
possible dropout renders now are the bus-health slate — solid
`kWarmingSlateRgba` (0xff1b1f27, BT.709 luma of R=0x1b/G=0x1f/B=0x27 ≈ 30) — or,
with `--policy black`, solid black (`kDropoutBlackRgba`, luma ≈ 16). Neither is
the old pink placeholder. `--policy hold` (the default) leaves 103's dropout
policy at its default `hold`, so the compositor keeps compositing the bus's
held last frame through the gap (luma stays near the pre-gap level, ~188 in
this fixture) — the stalled+black rule never engages because the policy is not
`black`. `--policy black` sends `set-source-policy` for `zoom:103` with
`dropoutPolicy:"black"` before the DROPOUT phase, so once bus health for 103
reads "stalled" (200 ms with no new frameId) the compositor paints solid black
for the gap and recovers to the held/live frame after RESTORE.

Usage:
  python scripts/qa/zoom-gap-hold-ab.py --core native/build-dev/corevideo-native.exe       --fake native/build-dev/corevideo-zoom-engine-fake.exe --label fixed [--policy hold|black]
  ffmpeg -i rec-fixed/*/Program.mp4 -vf signalstats,metadata=print:key=lavfi.signalstats.YAVG:file=fixed.yavg.txt -f null -
Writes <label>.jsonl (one snapshot slice per poll), <label>.stderr.log and
rec-<label>/<session>/Program.mp4, and prints a phase summary (including each
polled tick's `bus=` source health tuples, so a stuck "producing" read during
the gap is visible without re-running).
"""
import argparse, json, os, subprocess, sys, threading, time

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

def slice_(snap, t, phase):
    pf = snap.get("programFrame", {}) or {}
    tr = snap.get("takeRecords", {}) or {}
    churn = snap.get("zoomSubscriptionChurn", {}) or {}
    return {"t": round(t, 3), "phase": phase,
            "pgmSources": pf.get("videoSources"), "layerCount": pf.get("layerCount"),
            "renderPlanId": pf.get("renderPlanId"), "frameNumber": pf.get("frameNumber"),
            "planSig": pf.get("renderPlanSignature"), "pixSig": pf.get("programPixelSignature"),
            "health": pf.get("health"), "warnings": pf.get("warnings"),
            "sceneId": pf.get("sceneId"), "attr": pf.get("sceneIdAttribution"),
            "busSources": [(s.get("sourceId"), s.get("width"), s.get("height"), s.get("health"), s.get("framesIngested")) for s in (snap.get("sources") or [])],
            "takeCount": tr.get("count"), "takePending": tr.get("pending"),
            "lastTake": (tr.get("records") or [None])[-1],
            "churn": {k: churn.get(k) for k in ("sourceCount", "subscribedCount", "churnTotal", "total")} }

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--core", required=True); ap.add_argument("--label", required=True)
    ap.add_argument("--fake", required=True); ap.add_argument("--autosub", default="0")
    ap.add_argument("--poll-ms", type=int, default=50); ap.add_argument("--after-s", type=float, default=3.0)
    ap.add_argument("--policy", choices=("hold", "black"), default="hold",
                     help="dropout policy to set on zoom:103 before the DROPOUT phase (default hold)")
    a = ap.parse_args()
    env = {"COREVIDEO_ZOOM_ENGINE_PATH": a.fake, "COREVIDEO_FAKE_ENGINE_PARTICIPANTS": "3",
           "COREVIDEO_FAKE_ENGINE_RES": "2", "COREVIDEO_FAKE_ENGINE_FPS": "60",
           "COREVIDEO_FAKE_NO_CHURN": "1", "COREVIDEO_FAKE_ENGINE_AUTOSUBSCRIBE": a.autosub}
    core = Core(a.core, env)
    time.sleep(1.5)
    out = open(f"{a.label}.jsonl", "w")
    t0 = time.time()
    def el(): return int((time.time() - t0) * 1000) + 1000
    def rec(phase, resp):
        if resp is None: print(f"[{a.label}] {phase}: TIMEOUT"); return
        snap = resp.get("snapshot") or {}
        out.write(json.dumps(slice_(snap, time.time() - t0, phase)) + "\n"); out.flush()

    assert core.sync([{"type": "set-verbose-diagnostics", "enabled": True}], el()) is not None
    j = core.request({"type": "zoom-join", "payload": {"meetingNumber": "1234567890", "displayName": "ab"}}, timeout=20)
    assert j is not None, "join timeout"
    time.sleep(3.0)
    # steady show: 101 on program, 102 on preview
    core.spine([("101", "program"), ("102", "preview")], el())
    rec("configure", core.sync([{"type": "load-scene-graph", "sceneId": "pgm", "routes": [fixed("pgm-0", "101")]},
                                {"type": "set-preview-scene", "sceneId": "pvw", "routes": [fixed("pvw-0", "102")]}], el()))
    for _ in range(20):
        core.spine([("101", "program"), ("102", "preview")], el()); rec("steady", core.sync([], el())); time.sleep(0.1)
    # cue a BRAND-NEW participant (103) to preview: first frames ever for 103
    core.spine([("101", "program"), ("103", "preview")], el())
    rec("cue", core.sync([{"type": "set-preview-scene", "sceneId": "pvw2", "routes": [fixed("pvw-0", "103")]}], el()))
    for _ in range(30):
        core.spine([("101", "program"), ("103", "preview")], el()); rec("cued", core.sync([], el())); time.sleep(0.05)
    # TAKE
    rec("take", core.sync([{"type": "begin-take-transition", "operationId": "ab-take-1", "revision": 1,
                            "mode": "cut", "durationMs": 0, "direction": "auto", "dipColor": "#000000"},
                           {"type": "load-scene-graph", "sceneId": "pvw2", "routes": [fixed("pgm-0", "103")]},
                           {"type": "set-preview-scene", "sceneId": "pgm", "routes": [fixed("pvw-0", "101")]}], el()))
    core.spine([("103", "program"), ("101", "preview")], el())
    n = int(a.after_s * 1000 / a.poll_ms)
    for i in range(n):
        if i % 4 == 0: core.spine([("103", "program"), ("101", "preview")], el())
        rec("after", core.sync([], el())); time.sleep(a.poll_ms / 1000)
    # RECORD the program around the dropout so pixels can be judged offline.
    folder = os.path.abspath(f"rec-{a.label}")
    os.makedirs(folder, exist_ok=True)
    for f in os.listdir(folder): os.remove(os.path.join(folder, f))
    rec("record-start", core.sync([
        {"type": "start-program-output", "destinations": ["recording"], "isoSourceIds": [], "isoParticipantIds": []},
        {"type": "set-recording-targets", "targetFolder": folder, "filenamePrefix": "ab", "format": "mp4",
         "quality": "high", "isoSourceIds": [], "isoParticipantIds": []},
        {"type": "start-recording-session", "sessionId": "ab"}], el()))
    for i in range(30):
        if i % 4 == 0: core.spine([("103", "program"), ("101", "preview")], el())
        rec("recording", core.sync([], el())); time.sleep(0.05)
    # DROPOUT: 103 stays routed on program, but the subscription list omits it
    # for ~400ms (what a video-budget eviction or a transient spine does), then
    # restores it. Old path held the last frame; the bus removes the source.
    if a.policy == "black":
        rec("set-policy", core.sync([{"type": "set-source-policy", "sourceId": "zoom:103",
                                       "dropoutPolicy": "black", "displayName": "Guest 103"}], el()))
    core.spine([("101", "preview")], el())
    rec("dropout", core.sync([], el()))
    for i in range(8):
        core.spine([("101", "preview")], el()); rec("dropped", core.sync([], el())); time.sleep(0.05)
    core.spine([("103", "program"), ("101", "preview")], el())
    rec("restore", core.sync([], el()))
    for i in range(40):
        if i % 4 == 0: core.spine([("103", "program"), ("101", "preview")], el())
        rec("restored", core.sync([], el())); time.sleep(0.05)
    rec("record-stop", core.sync([{"type": "stop-recording-session", "reason": "ab done"}], el()))
    time.sleep(1.0)
    rec("encoder-stop", core.sync([{"type": "stop-encoder-session", "reason": "ab done"}], el()))
    time.sleep(2.0)
    out.close()
    core.request({"type": "zoom-leave", "payload": {}}, timeout=5)
    core.kill()
    # summary
    rows = [json.loads(l) for l in open(f"{a.label}.jsonl")]
    print(f"\n=== {a.label}: {len(rows)} slices ===")
    last = None
    for r in rows:
        bus_nocount = tuple((b[0], b[1], b[2], b[3]) for b in r["busSources"])
        key = (r["phase"], tuple(json.dumps(x, sort_keys=True) for x in (r["pgmSources"] or [])), r["layerCount"], bus_nocount, r["health"], tuple(r["warnings"] or []), r["sceneId"], r["attr"])
        if key != last:
            print(f"{r['t']:7.3f} {r['phase']:9} scene={r['sceneId']}/{r['attr']} pgm={[x.get('participantId') for x in (r['pgmSources'] or [])]} layers={r['layerCount']} health={r['health']} "
                  f"bus={bus_nocount} warn={r['warnings']} takes={r['takeCount']}/{r['takePending']} frame={r['frameNumber']}")
            last = key
    lt = rows[-1]["lastTake"]
    if lt: print("last take record:", json.dumps({k: lt.get(k) for k in ("verdict", "mode", "fromSourceIds", "toLayerIds", "restartedSources", "missingSources", "subscriptionsChurned", "sources")}, indent=None))
    open(f"{a.label}.stderr.log", "w").write("\n".join(core.stderr))
    errs = [l for l in core.stderr if "error" in l.lower() or "warn" in l.lower()]
    print(f"stderr lines: {len(core.stderr)} (errors/warnings: {len(errs)})")
    for l in errs[:10]: print("  ", l[:200])

if __name__ == "__main__":
    main()
