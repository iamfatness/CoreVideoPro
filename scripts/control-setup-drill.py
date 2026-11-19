#!/usr/bin/env python3
"""control-setup-drill.py: join-free show setup over the control API, judged by the host's own state.

    python3 scripts/control-setup-drill.py --base http://127.0.0.1:8011 [--token T] \
        --slot 1 --source capture:<id> --scene <sceneId> [--record-seconds 5]

Steps, in order, each judged from GET /state and GET /snapshot (never from the invoke reply alone):
  1. input.assign            -> /state inputs[slot-1].sourceId == source
  2. scene.select            -> /state previewSceneId == scene
  3. transport.transition.set "cut", then transport.take
                             -> /state activeSceneId == scene AND /snapshot programFrame.sceneId == scene
                                with sceneIdAttribution == "live" (the core's own attribution), within 3 s
  4. transport.record.set true, wait --record-seconds, then false
                             -> /snapshot recording.lifecycle.state reaches "completed" within 30 s
It sends no zoom.join: the drill must run without a meeting.

Exit: 0 all steps passed; 1 the first failed step (named); 2 usage or connection error.
Tests: python3 -m unittest scripts/tests/test_control_setup_drill.py
Plan: docs/superpowers/plans/2026-11-16-mac-remote-control-phase1.md (Task 12).
"""
import argparse
import json
import sys
import time
import urllib.error
import urllib.request


class ConnectionFailure(Exception):
    pass


class Drill:
    def __init__(self, base, token):
        self.base = base.rstrip("/")
        self.token = token

    def _request(self, method, path, body=None):
        data = None if body is None else json.dumps(body).encode()
        request = urllib.request.Request(self.base + path, data=data, method=method)
        if data is not None:
            request.add_header("Content-Type", "application/json")
        if self.token:
            request.add_header("Authorization", "Bearer " + self.token)
        try:
            with urllib.request.urlopen(request, timeout=20) as response:
                return response.status, json.loads(response.read() or b"{}")
        except urllib.error.HTTPError as error:
            try:
                return error.code, json.loads(error.read() or b"{}")
            except ValueError:
                return error.code, {}
        except (urllib.error.URLError, OSError, ValueError) as error:
            raise ConnectionFailure(f"{method} {path}: {error}") from error

    def invoke(self, action, *args):
        status, body = self._request("POST", "/invoke", {"action": action, "args": list(args)})
        if status != 200 or not body.get("ok"):
            return f"{action} answered {status}: {body.get('error', body)}"
        return None

    def state(self):
        return self._request("GET", "/state")[1]

    def snapshot(self):
        envelope = self._request("GET", "/snapshot")[1]
        return envelope.get("snapshot") or {}

    @staticmethod
    def wait(check, seconds):
        deadline = time.monotonic() + seconds
        while True:
            why = check()
            if why is None or time.monotonic() >= deadline:
                return why
            time.sleep(0.1)


def run(drill, slot, source, scene, record_seconds):
    def assigned():
        inputs = drill.state().get("inputs") or []
        got = inputs[slot - 1].get("sourceId") if 0 < slot <= len(inputs) else None
        return None if got == source else f"inputs[{slot - 1}].sourceId is {got!r}, expected {source!r}"

    def cued():
        got = drill.state().get("previewSceneId")
        return None if got == scene else f"previewSceneId is {got!r}, expected {scene!r}"

    def taken():
        active = drill.state().get("activeSceneId")
        if active != scene:
            return f"activeSceneId is {active!r}, expected {scene!r}"
        frame = drill.snapshot().get("programFrame") or {}
        if frame.get("sceneId") != scene or frame.get("sceneIdAttribution") != "live":
            return (f"/snapshot programFrame is sceneId {frame.get('sceneId')!r} "
                    f"({frame.get('sceneIdAttribution')!r}), expected {scene!r} (live)")
        return None

    def finalized():
        lifecycle = (drill.snapshot().get("recording") or {}).get("lifecycle") or {}
        got = lifecycle.get("state")
        return None if got == "completed" else f"recording.lifecycle.state is {got!r}, expected 'completed'"

    def record():
        why = drill.invoke("transport.record.set", True)
        if why:
            return why
        time.sleep(record_seconds)
        return drill.invoke("transport.record.set", False) or Drill.wait(finalized, 30)

    steps = [
        ("input.assign", lambda: drill.invoke("input.assign", slot, source) or Drill.wait(assigned, 3)),
        ("scene.select", lambda: drill.invoke("scene.select", scene) or Drill.wait(cued, 3)),
        ("transport.take", lambda: drill.invoke("transport.transition.set", "cut")
            or drill.invoke("transport.take") or Drill.wait(taken, 3)),
        ("transport.record.set", record),
    ]
    for name, step in steps:
        why = step()
        if why:
            print(f"FAIL {name}: {why}")
            return 1
        print(f"PASS {name}")
    print(f"{len(steps)}/{len(steps)} steps passed against {drill.base}")
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--base", required=True)
    parser.add_argument("--token")
    parser.add_argument("--slot", type=int, required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--scene", required=True)
    parser.add_argument("--record-seconds", type=float, default=5)
    try:
        options = parser.parse_args(argv)
    except SystemExit as exit_:
        return 2 if exit_.code else 0
    try:
        return run(Drill(options.base, options.token), options.slot, options.source, options.scene, options.record_seconds)
    except ConnectionFailure as error:
        print(f"connection error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
