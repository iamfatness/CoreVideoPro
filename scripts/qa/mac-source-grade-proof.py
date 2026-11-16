#!/usr/bin/env python3
"""Real-core proof that a per-source grade changes ONLY its source (grading plan 2026-11-16, Task 7).

Runs its own core (never the operator's app) with the synthetic Zoom engine: two fake
participants, 101 left and 102 right, both on Program through `fixed` routes. It records
Program ungraded, grades 101 to black through the same requests the Mac store sends
(query-source-grade, then set-source-grade; in --mode advanced, register-grade-document
first), waits until the core's `sourceGrades` node reports 101 active and does not list 102,
records again, decodes both recordings and judges the two halves of the picture.

  python3 scripts/qa/mac-source-grade-proof.py [--mode basic|advanced] [--seconds 3] [--out DIR]

Exit 0 = pass; 1 = fail; 2 = the harness could not prove anything (no ffmpeg, no roster, a
dark "before" window). Missing ffmpeg is LOUD, never a silent skip.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

BLACK_Y = 24          # limited-range black is 16; leave room for encoder noise
JUDGED_FRAMES = 30

# gain 0 makes pow(max(v*0,0),1) = 0 in the primaries shader: black for any input.
ADVANCED_BLACK = {
    "version": 2, "colorSpace": "rec709-sdr", "intensity": 1, "bypass": False,
    "operations": [{
        "id": "black", "kind": "primaries", "enabled": True, "intensity": 1,
        "exposureStops": 0, "contrast": 1, "pivot": 0.5, "saturation": 1, "temperature": 0,
        "tint": 0, "lift": 0, "gamma": 1, "gain": 0,
        "curves": [[{"x": 0, "y": 0}, {"x": 1, "y": 1}]] * 4,
        "cubeText": "", "cubeSha256": "", "name": ""}]}


def black_fraction(y_plane: bytes, threshold: int = BLACK_Y) -> float:
    """Fraction of luma samples at or below `threshold` (inclusive)."""
    if not y_plane:
        return 0.0
    return sum(1 for value in y_plane if value <= threshold) / len(y_plane)


def judge(before_left, before_right, after_left, after_right) -> dict:
    """Each argument is a list of Y planes (bytes). Returns {"verdict": "pass"|"fail"|"harness", "reasons": [str]}."""
    windows = {"before-left": before_left, "before-right": before_right,
               "after-left": after_left, "after-right": after_right}
    empty = [name for name, frames in windows.items() if not frames]
    if empty:
        return {"verdict": "harness", "reasons": [f"no frames decoded for {', '.join(empty)}"]}
    reasons = []
    for name in ("before-left", "before-right"):
        mean = sum(black_fraction(f) for f in windows[name]) / len(windows[name])
        if mean >= 0.05:
            reasons.append(f"{name} is already dark (mean black fraction {mean:.3f}); the run proved nothing")
    if reasons:
        return {"verdict": "harness", "reasons": reasons}
    worst_left = min(black_fraction(f) for f in after_left)
    if worst_left < 0.98:
        reasons.append(f"graded source did not change (a graded frame is only {worst_left:.3f} black)")
    worst_right = max(black_fraction(f) for f in after_right)
    if worst_right >= 0.05:
        reasons.append(f"ungraded neighbour changed (a neighbour frame is {worst_right:.3f} black)")
    return {"verdict": "fail" if reasons else "pass", "reasons": reasons}


def _drill():
    spec = importlib.util.spec_from_file_location("show_drill", os.path.join(REPO, "scripts", "mac-show-drill.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _route(route_id, pid, x):
    return {"routeId": route_id, "mode": "fixed", "participantId": pid, "audioRole": "mix",
            "fitMode": "fill", "opacity": 1.0, "zIndex": 0,
            "rect": {"x": x, "y": 0, "width": 0.5, "height": 1}}


def _record(core, folder, session, seconds):
    start = core.sync([
        {"type": "start-program-output", "destinations": ["recording"], "isoSourceIds": [], "isoParticipantIds": []},
        {"type": "set-recording-targets", "targetFolder": folder, "filenamePrefix": session, "format": "mp4",
         "quality": "high", "isoSourceIds": [], "isoParticipantIds": []},
        {"type": "start-recording-session", "sessionId": session}], 60000, step=f"{session}-start")
    time.sleep(seconds)
    stop = core.sync([{"type": "stop-recording-session", "reason": "grade proof"}], 66000, step=f"{session}-stop")
    core.sync([{"type": "stop-encoder-session", "reason": "grade proof"}], 67000, step=f"{session}-encoder-stop")
    path = ""
    # The artifact path is published once the writer has finalised.
    for _ in range(40):
        time.sleep(0.25)
        snap = (core.sync([], 68000, step=f"{session}-poll") or {}).get("snapshot", {})
        recording = snap.get("recording", {})
        path = recording.get("artifactPath", "") or path
        if path and recording.get("status") not in ("stopping", "recording", "starting"):
            break
    if path and not os.path.isabs(path):
        path = os.path.join(REPO, path)
    return path if start is not None and stop is not None else ""


def _planes(ffmpeg, path, left):
    probe = subprocess.run([ffmpeg.replace("ffmpeg", "ffprobe"), "-v", "error", "-select_streams", "v:0",
                            "-show_entries", "stream=width,height", "-of", "csv=p=0", path],
                           capture_output=True, text=True)
    try:
        width, height = (int(v) for v in probe.stdout.strip().split(",")[:2])
    except ValueError:
        return []
    crop = "crop=iw/2:ih:0:0" if left else "crop=iw/2:ih:iw/2:0"
    out = subprocess.run([ffmpeg, "-v", "error", "-sseof", "-1.5", "-i", path, "-vf", f"{crop},format=gray",
                          "-f", "rawvideo", "-"], capture_output=True)
    size = (width // 2) * height
    frames = [out.stdout[i:i + size] for i in range(0, len(out.stdout) - size + 1, size)]
    return frames[-JUDGED_FRAMES:]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("basic", "advanced"), default="basic")
    parser.add_argument("--seconds", type=float, default=3.0)
    parser.add_argument("--out", default=os.path.join(REPO, "artifacts", "source-grade-proof"))
    args = parser.parse_args()

    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg or not shutil.which("ffprobe"):
        print("HARNESS ffmpeg/ffprobe not found on PATH: the recordings cannot be judged, so this run proves nothing")
        return 2
    drill = _drill()
    if not os.path.exists(drill.DEFAULT_CORE) or not os.path.exists(drill.FAKE_ENGINE):
        print(f"HARNESS core or fake engine not built: {drill.DEFAULT_CORE} / {drill.FAKE_ENGINE}")
        return 2
    os.makedirs(args.out, exist_ok=True)
    env = drill.load_env(2)
    print(f"core {drill.DEFAULT_CORE}; fake engine fps pinned at {env['COREVIDEO_FAKE_ENGINE_FPS']} "
          f"({env['COREVIDEO_FAKE_ENGINE_PARTICIPANTS']} participants)")
    core = drill.Core(drill.DEFAULT_CORE, env=env, evidence_path=os.path.join(args.out, "command-timing.json"))
    result = {"mode": args.mode}
    try:
        time.sleep(1.5)
        if core.request({"type": "zoom-join", "payload": {"meetingNumber": "1234567890",
                                                          "displayName": "grade-proof"}}, timeout=20.0) is None:
            print("HARNESS zoom-join timed out")
            return 2
        core.sync([{"type": "load-scene-graph", "sceneId": "grade-proof",
                    "routes": [_route("left", "101", 0), _route("right", "102", 0.5)]}], 500, step="scene")
        deadline = time.time() + 15
        present = {}
        while time.time() < deadline:
            core.sync([], 600, step="wait")
            for pid in ("101", "102"):
                answer = core.request({"type": "query-source-grade", "sourceId": pid}) or {}
                present[pid] = answer
            if all(a.get("present") for a in present.values()):
                break
            time.sleep(0.25)
        if not all(a.get("present") for a in present.values()):
            print(f"HARNESS participants 101/102 never produced frames: {present}")
            return 2
        time.sleep(1.0)
        before = _record(core, args.out, f"proof-{args.mode}-before", args.seconds)

        query = core.request({"type": "query-source-grade", "sourceId": "101"}) or {}
        grade = {"lut": "none", "exposure": -100}
        if args.mode == "advanced":
            text = json.dumps(ADVANCED_BLACK, separators=(",", ":"), sort_keys=True)
            ref = hashlib.sha256(text.encode()).hexdigest()
            asked = core.request({"type": "register-grade-document", "documentRef": ref}) or {}
            if not asked.get("present"):
                uploaded = core.request({"type": "register-grade-document", "documentRef": ref, "documentJson": text}) or {}
                if not uploaded.get("present"):
                    print(f"FAIL register-grade-document refused: {uploaded}")
                    return 1
            grade = {"lut": "none", "advanced": {"documentRef": ref}}
        applied = core.request({"type": "set-source-grade", "sourceId": "101", "sourceEpoch": query.get("sourceEpoch", 0),
                                "expectedRevision": query.get("revision", 0), "grade": grade}) or {}
        neighbour = core.request({"type": "query-source-grade", "sourceId": "102"}) or {}
        result["epochs"] = {"101": query.get("sourceEpoch"), "102": neighbour.get("sourceEpoch")}
        result["applied"] = applied
        if not applied.get("accepted"):
            print(json.dumps(dict(result, verdict="fail", reasons=["set-source-grade was refused"])))
            return 1
        rows = []
        deadline = time.time() + 2.0
        while time.time() < deadline:
            rows = ((core.sync([], 70000, step="grade-active") or {}).get("snapshot", {}) or {}).get("sourceGrades") or []
            ids = {r.get("sourceId"): r for r in rows}
            if ids.get("101", {}).get("active") and "102" not in ids:
                break
            time.sleep(0.1)
        result["sourceGrades"] = rows
        ids = {r.get("sourceId"): r for r in rows}
        if not (ids.get("101", {}).get("active") and "102" not in ids):
            print(json.dumps(dict(result, verdict="fail", reasons=["sourceGrades never reported 101 active alone within 2 s"])))
            return 1
        after = _record(core, args.out, f"proof-{args.mode}-after", args.seconds)
    finally:
        core.kill()

    result["recordings"] = {"before": before, "after": after}
    verdict = judge(_planes(ffmpeg, before, True) if before else [], _planes(ffmpeg, before, False) if before else [],
                    _planes(ffmpeg, after, True) if after else [], _planes(ffmpeg, after, False) if after else [])
    result.update(verdict)
    print(json.dumps(result, separators=(",", ":")))
    return {"pass": 0, "fail": 1}.get(verdict["verdict"], 2)


if __name__ == "__main__":
    sys.exit(main())
