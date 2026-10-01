"""Core-restart drill: kill the media core under a live shell and judge the recovery.

The app must already be running and in a meeting with Engine on. Uses ONLY the control API
(http://127.0.0.1:8011) plus one process kill; it never sends keyboard or mouse input.

    python scripts/qa/core-restart-drill.py [--takes N] [--record] [--settle SECONDS] [--json PATH]

What it judges, and why each exists:
  * the core comes back and rejoins                      (supervisor respawn + Zoom recovery)
  * the SHELL's roster is the new core's roster           (#725: it kept the dead core's)
  * no in-show slot names someone who is not in the meeting
  * the app's own user holds at most one slot             (the old instance lingers after a rejoin)
  * the multiviewer config survived                       (a one-shot command lost on respawn)
  * Program is delivering frames again, with no new underruns after it settles
  * Takes after the restart do not dip the roster         (launch.log: mv-roster-miss / roster-drop)
  * with --record: what happened to the recording          (reported; see RECORDING below)

Exit code 0 only when every gated check passes. INFO lines are measurements, not gates.
"""
import argparse
import json
import os
import subprocess
import sys
import time
import urllib.request

BASE = "http://127.0.0.1:8011"
LOG = os.path.join(os.environ.get("LOCALAPPDATA", ""), "CoreVideoPro", "launch.log")


def get(path):
    return json.load(urllib.request.urlopen(BASE + path, timeout=5))


def invoke(action, *args):
    body = json.dumps({"action": action, "args": list(args)}).encode()
    request = urllib.request.Request(BASE + "/invoke", data=body, headers={"Content-Type": "application/json"})
    try:
        return json.load(urllib.request.urlopen(request, timeout=60))
    except urllib.error.HTTPError as error:
        return {"ok": False, "error": error.read().decode(errors="replace")}


def core():
    return get("/snapshot").get("snapshot") or {}


def core_pid(snapshot):
    """rosterEpoch is `<process>:<meeting>:<corePid>-<spawn>`; the token names the core process."""
    token = (snapshot.get("rosterEpoch") or "").split(":")[-1]
    head = token.split("-")[0]
    return int(head) if head.isdigit() else None


def roster(snapshot):
    return {str(p.get("userId")): p for p in snapshot.get("participants", [])}


def in_show_zoom_slots(state):
    return {i["slot"]: i["sourceId"].split(":", 1)[1] for i in state.get("inputs", [])
            if i.get("inShow") and (i.get("sourceId") or "").startswith("zoom:")}


def log_size():
    try:
        return os.path.getsize(LOG)
    except OSError:
        return 0


def log_since(offset):
    try:
        with open(LOG, "r", encoding="utf-8", errors="replace") as handle:
            handle.seek(offset)
            return handle.read().splitlines()
    except OSError:
        return []


def wait_for(predicate, seconds, interval=0.5):
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            value = predicate()
            if value:
                return value
        except Exception:
            pass
        time.sleep(interval)
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--takes", type=int, default=6)
    parser.add_argument("--record", action="store_true")
    parser.add_argument("--record-seconds", type=float, default=20.0,
                        help="how long to record before the kill, so the surviving file can be compared with it")
    parser.add_argument("--settle", type=float, default=75.0,
                        help="seconds to wait for the dead engine's old participant to leave the meeting")
    parser.add_argument("--json", default="")
    options = parser.parse_args()

    results, report = [], {"checks": [], "info": {}}

    def check(name, ok, evidence):
        results.append(bool(ok))
        report["checks"].append({"name": name, "ok": bool(ok), "evidence": evidence})
        print(f"[{'PASS' if ok else 'FAIL'}] {name}: {evidence}")

    def info(name, value):
        report["info"][name] = value
        print(f"[INFO] {name}: {value}")

    state0, core0 = get("/state"), core()
    if core0.get("meetingState") != "in_meeting" or not state0.get("engineOn"):
        print("Precondition failed: the app must be in a meeting with Engine on.")
        return 2
    if "zoomParticipantIds" not in state0:
        print("Precondition failed: this build's /state has no zoomParticipantIds (needs the drill's shell change).")
        return 2
    pid0 = core_pid(core0)
    if not pid0:
        print(f"Precondition failed: could not read the core pid from rosterEpoch '{core0.get('rosterEpoch')}'.")
        return 2
    multiview0 = (core0.get("multiviewer") or {}).get("layoutMode")
    me0 = [u for u, p in roster(core0).items() if p.get("isMe")]
    print(f"before: core pid={pid0} epoch={core0.get('rosterEpoch')} roster={len(roster(core0))} "
          f"slots={in_show_zoom_slots(state0)} multiview={multiview0} me={me0}")

    recording_path, recorded_seconds = None, 0.0
    if options.record:
        invoke("transport.record.set", True)
        started = wait_for(lambda: (core().get("recording") or {}).get("active") and
                           ((core().get("recording") or {}).get("totalFramesWritten") or 0) > 60, 30)
        check("recording was writing frames before the kill", bool(started),
              {k: (core().get("recording") or {}).get(k) for k in ("status", "active", "totalFramesWritten")})
        if started:
            time.sleep(options.record_seconds)
            recording = core().get("recording") or {}
            recording_path = recording.get("programPath")
            recorded_seconds = (recording.get("elapsedMs") or 0) / 1000.0
            info("recording before kill", {"elapsedSeconds": round(recorded_seconds, 1), "programPath": recording_path,
                                           "status": recording.get("status")})

    mark = log_size()
    killed_at = time.time()
    subprocess.run(["taskkill", "/PID", str(pid0), "/F"], capture_output=True)
    print(f"killed core pid={pid0}")

    def recovered():
        snapshot = core()
        pid = core_pid(snapshot)
        return snapshot if pid and pid != pid0 and snapshot.get("meetingState") == "in_meeting" else None

    core1 = wait_for(recovered, 90)
    check("core restarted and rejoined the meeting", bool(core1),
          f"new epoch={core1.get('rosterEpoch')} after {time.time() - killed_at:.1f}s" if core1 else "no new in-meeting core within 90s")
    if not core1:
        return finish(results, report, options)

    def shell_follows():
        state, snapshot = get("/state"), core()
        return (state, snapshot) if state.get("zoomRosterEpoch") == snapshot.get("rosterEpoch") and \
            set(state.get("zoomParticipantIds", [])) == set(roster(snapshot)) else None

    followed = wait_for(shell_follows, 15)
    state1, core1 = (followed if followed else (get("/state"), core()))
    check("the shell's roster is the restarted core's roster", bool(followed),
          f"shell epoch={state1.get('zoomRosterEpoch')} n={len(state1.get('zoomParticipantIds', []))} | "
          f"core epoch={core1.get('rosterEpoch')} n={len(roster(core1))}")

    check("the multiviewer config survived the restart",
          (core1.get("multiviewer") or {}).get("layoutMode") == multiview0,
          f"before={multiview0} after={(core1.get('multiviewer') or {}).get('layoutMode')}")

    frames_a = core().get("programFrameCount") or (core().get("health") or {}).get("frameCount") or 0
    time.sleep(2)
    snapshot_b = core()
    frames_b = snapshot_b.get("programFrameCount") or (snapshot_b.get("health") or {}).get("frameCount") or 0
    check("Program is delivering frames again", frames_b - frames_a >= 100, f"{frames_b - frames_a} frames in 2s")

    # Just after the restart: who is 'me', and how many slots does the app's own user hold?
    people1 = roster(core1)
    me1 = [u for u, p in people1.items() if p.get("isMe")]
    lingering = [u for u in me0 if u in people1 and u not in me1]
    info("app user right after restart", {"me": me1, "old instance still in the meeting": lingering})
    slots1 = in_show_zoom_slots(state1)
    own_slots = [slot for slot, pid in slots1.items() if pid in me1 or pid in lingering]
    check("the app's own user holds at most one show slot right after the restart", len(own_slots) <= 1,
          f"slots held by the app's user (old or new instance): {own_slots}")

    # Takes after the restart.
    state = get("/state")
    if state.get("activeSceneId") == state.get("previewSceneId"):
        info("takes", "skipped: Preview equals Program, nothing to Take")
        accepted = 0
    else:
        accepted = 0
        for _ in range(options.takes):
            accepted += 1 if invoke("transport.take").get("ok") else 0
            time.sleep(0.7)
        info("takes accepted", f"{accepted}/{options.takes}")
    time.sleep(1.5)
    lines = log_since(mark)
    dips = [line for line in lines if "mv-roster-miss" in line and "holding" in line]
    split = []
    for line in (l for l in lines if "roster-drop:" in l):
        snap, bridge = line.split("snap(epoch=")[1].split(" ")[0], line.split("bridge(epoch=")[1].split(" ")[0]
        if snap != bridge:
            split.append(line.split("roster-drop:")[1].strip()[:160])
    check("Takes after the restart do not dip the roster", not dips and not split,
          f"mv-roster-miss holds={len(dips)} two-epoch roster-drops={len(split)}")

    # Let the dead engine's participant time out of the meeting, then judge the settled state.
    def settled():
        snapshot = core()
        return snapshot if not any(u in roster(snapshot) for u in lingering) else None

    if lingering:
        gone = wait_for(settled, options.settle, 2)
        info("old instance left the meeting", f"after {time.time() - killed_at:.0f}s" if gone else f"still present after {options.settle:.0f}s")
    time.sleep(2)
    state2, core2 = get("/state"), core()
    people2 = roster(core2)
    dead = {slot: pid for slot, pid in in_show_zoom_slots(state2).items() if pid not in people2}
    check("no show slot names someone who is not in the meeting", not dead, f"dead slots={dead}")
    check("the shell's roster still matches the core's once settled",
          set(state2.get("zoomParticipantIds", [])) == set(people2),
          f"shell n={len(state2.get('zoomParticipantIds', []))} core n={len(people2)}")
    info("slots before", in_show_zoom_slots(state0))
    info("slots after", in_show_zoom_slots(state2))
    buffer_a = (core2.get("programBuffer") or {}).get("underruns")
    time.sleep(5)
    buffer_b = (core().get("programBuffer") or {}).get("underruns")
    if buffer_a is not None and buffer_b is not None:
        check("no Program underruns once settled", buffer_b == buffer_a, f"underruns {buffer_a} -> {buffer_b} over 5s")

    # RECORDING: reported, not gated. A core that dies cannot finalize its own file; what this
    # says is whether the operator still has a recording running, and whether the old file opens.
    if options.record:
        recording = core().get("recording") or {}
        info("recording after restart", {k: recording.get(k) for k in ("status", "active", "sessionId", "programPath", "warning")})
        info("shell believes it is recording", get("/state").get("recording"))
        if recording_path and os.path.exists(recording_path):
            probe = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0",
                                    recording_path], capture_output=True, text=True)
            duration = probe.stdout.strip()
            info("file from before the kill", {"path": recording_path, "bytes": os.path.getsize(recording_path),
                                              "playable seconds": duration or f"UNREADABLE ({probe.stderr.strip()[:120]})",
                                              "recorded seconds at the kill": round(recorded_seconds, 1),
                                              "lost seconds": round(recorded_seconds - float(duration), 1) if duration else "all"})
        else:
            info("file from before the kill", f"path not found: {recording_path}")
        invoke("transport.record.set", False)

    return finish(results, report, options)


def finish(results, report, options):
    passed = all(results) and bool(results)
    report["passed"] = passed
    if options.json:
        with open(options.json, "w", encoding="utf-8") as handle:
            json.dump(report, handle, indent=2)
    print("CORE RESTART DRILL " + ("PASSED" if passed else "FAILED"))
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
