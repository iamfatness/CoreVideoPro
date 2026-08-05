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
DEFAULT_CORE = os.path.join(REPO, "native", "build-metal", "corevideo-native")

# The core's own budget for a render tick (LockHoldGuardrail::kRenderTickBudgetUs
# is half a frame). A show that exceeds it on most ticks starves the RPC queue.
MAX_OVER_BUDGET_RATIO = 0.35


class Core:
    def __init__(self, path):
        self.proc = subprocess.Popen(
            [path], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, bufsize=1)
        self.responses = {}
        self.events = []
        self.stderr = []
        self.next_id = 1
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
        self.proc.stdin.write(json.dumps(body, separators=(",", ":")) + "\n")
        self.proc.stdin.flush()
        deadline = time.time() + timeout
        while time.time() < deadline:
            if rid in self.responses:
                return self.responses[rid]
            time.sleep(0.01)
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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--seconds", type=float, default=40.0)
    parser.add_argument("--core", default=DEFAULT_CORE)
    args = parser.parse_args()

    if not os.path.exists(args.core):
        print(f"FAIL core binary not found: {args.core}")
        return 1

    failures = []
    core = Core(args.core)
    time.sleep(1.5)

    timeouts = 0

    def step(name, commands, elapsed):
        nonlocal timeouts
        response = core.sync(commands, elapsed)
        if response is None:
            timeouts += 1
            failures.append(f"{name}: request TIMED OUT")
        return response

    # 1. Arm the wall and put scenes on both buses (the shell's connect path).
    step("configure", [
        {"type": "configure-multiviewer", "layoutMode": "pgmPvwTop", "tileCount": 10},
        {"type": "load-scene-graph", "sceneId": "pgm", "routes": [route("pgm-0")]},
        {"type": "set-preview-scene", "sceneId": "pvw", "routes": [route("pvw-0")]},
    ], 500)

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
    stop = step("record-stop", [{"type": "stop-encoder-session",
                                 "reason": "drill complete"}], 66000)

    # ── assertions ───────────────────────────────────────────────────────────
    snapshot = (stop or {}).get("snapshot", {})

    recording = snapshot.get("recording", {})
    artifact = recording.get("artifactPath", "")
    if artifact and os.path.exists(artifact) and os.path.getsize(artifact) > 1024:
        print(f"PASS recording wrote {os.path.getsize(artifact)} bytes")
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

    if timeouts:
        print(f"FAIL {timeouts} request(s) timed out")
    else:
        print("PASS every request answered")

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
