"""Live acceptance check for #478 (sources-only Zoom feeds, no talk-driven churn) and #481 (no app mutes).

Reads ONLY the control API (http://127.0.0.1:8011). Never sends input. Usage:
    python scripts/qa/live-check-sources-audio.py [seconds]
Prints PASS/FAIL per invariant with the evidence behind each.
"""
import json, sys, time, urllib.request

BASE = "http://127.0.0.1:8011"
SECONDS = int(sys.argv[1]) if len(sys.argv) > 1 else 90


def get(path):
    return json.load(urllib.request.urlopen(BASE + path, timeout=3))


def snapshot():
    d = get("/snapshot")
    return d, d["snapshot"]


def sources_from_state(state):
    """Routed sources as the SHELL knows them: in-show wall slots (Zoom) + Program video sources."""
    wall = {(i.get("sourceId") or "").split(":", 1)[-1] for i in state.get("inputs", [])
            if i.get("inShow") and (i.get("sourceId") or "").startswith("zoom:")}
    program = {(s.get("participantId") or "") for s in state.get("nativeProgramVideoSources", []) if s.get("participantId")}
    return wall, program


results = []


def check(name, ok, evidence):
    results.append((name, ok))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {evidence}")


d0, s0 = snapshot()
state = get("/state")
wall, program = sources_from_state(state)
parts = {p["userId"]: p for p in s0.get("participants", [])}
subs = {x["sourceUuid"]: x for x in s0["zoomSubscriptionChurn"]["sources"]}
live_video = {x["participantId"] for x in subs.values() if x["sourceUuid"].startswith("participant-video") and x.get("subscribed")}
live_audio = {x["participantId"] for x in subs.values() if x["sourceUuid"].startswith("participant-audio") and x.get("subscribed")}
print(f"meeting={s0.get('meetingState')} participants={len(parts)} wall={sorted(wall)} program={sorted(program)}")

# 1. Every camera-on wall guest has a live video feed (no click needed).
missing = [p for p in wall if parts.get(p, {}).get("videoOn") and p not in live_video]
check("wall guests with camera on have video", not missing, f"missing={missing}")

# 2. Nobody outside the known source set holds video or audio (Preview/Tiles/ISO add to the set;
#    anything left over is reported, not auto-failed, since this script cannot see every source kind).
known = wall | program
extra_v = sorted(p for p in live_video if p not in known)
extra_a = sorted(p for p in live_audio if p not in known)
check("no video feed for camera-off participants",
      not [p for p in live_video if parts.get(p, {}).get("videoOn") is False],
      f"camera-off with video={[p for p in live_video if parts.get(p, {}).get('videoOn') is False]}")
print(f"[INFO] video feeds outside wall/program (should be Preview/Tiles/ISO only): {extra_v}")
print(f"[INFO] audio feeds outside wall/program (should be Preview/Tiles/ISO only): {extra_a}")

# 3. No guest channel muted by the app: a muted channel whose guest is NOT Zoom-muted and was not
#    A1-muted is the #481 defect. The script cannot see A1 intent, so it lists muted source channels.
aud = {a["sourceId"]: a for a in state.get("audioSources", [])}
muted_sources = sorted(p for p in wall if aud.get(p, {}).get("muted") and not parts.get(p, {}).get("muted"))
check("no wall guest muted while unmuted in Zoom (unless the A1 muted them)", not muted_sources, f"muted={muted_sources}")

# 4. Over the window: no talk-driven churn, meters alive for talkers, snapshot fresh.
c0 = s0["zoomSubscriptionChurn"]
t_end = time.time() + SECONDS
max_age = 0
talk_seen, metered_talkers = set(), set()
speaker_changes, prev_talkers = 0, None
while time.time() < t_end:
    d, s = snapshot()
    max_age = max(max_age, d.get("ageMs") or 0)
    talkers = frozenset(p["userId"] for p in s["participants"] if p.get("talking"))
    if prev_talkers is not None and talkers != prev_talkers:
        speaker_changes += 1
    prev_talkers = talkers
    chans = {c["participantId"]: c for c in s["audioMixSession"]["participants"]}
    for t in talkers & wall:
        talk_seen.add(t)
        c = chans.get(t, {})
        level = max(c.get("rmsDbfs", -120), c.get("inputRmsDbfs", -120) if c.get("inputRmsDbfs") is not None else -120)
        if level > -90:
            metered_talkers.add(t)
    time.sleep(0.5)
c1 = snapshot()[1]["zoomSubscriptionChurn"]
res_changes = (c1.get("lastResolutionChanges") or 0) - (c0.get("lastResolutionChanges") or 0) if isinstance(c1.get("lastResolutionChanges"), int) else "n/a"
churn = (c1.get("totalChurn") or 0) - (c0.get("totalChurn") or 0)
check("snapshot stays fresh (meters live)", max_age < 2000, f"max ageMs={round(max_age)}")
check("no subscription churn from talking", churn == 0, f"churn delta={churn} over {SECONDS}s with {speaker_changes} speaker changes; resolutionChanges delta={res_changes}")
check("talking wall guests move a meter", talk_seen <= metered_talkers or not talk_seen,
      f"talkers seen={sorted(talk_seen)} metered={sorted(metered_talkers)}")

print(f"\nSUMMARY: {sum(ok for _, ok in results)}/{len(results)} pass")
