#!/usr/bin/env python3
"""Real-core proof of the Metal private grade preview, Compare and scopes (grading Phase 2, Task 10).

Runs its OWN core (never the operator's app) with the synthetic Zoom engine: participant 101 on
Program, full frame. It opens an editor exactly as the Mac shell does, over the core's stdio
(`set-grade-preview`, renewed every 500 ms, a gain-0 advanced document registered first), reads
the `grade-preview` events off stdout (proving they reach a stdio client: spec risk R1), looks up
the published IOSurfaces BY GLOBAL ID through ctypes (what the shell's SurfaceView does) and
judges their pixels:

  1. graded preview: the centre is black (gain 0), while Program's centre is not and stays so
     (a private draft never reaches air);
  2. Compare original: the preview centre matches Program's centre within 6 levels;
  3. scopes: the scope surface is 1536x512 and carries a trace (pixels above the 0.012 background);
  4. close (`enabled:false`): no further events for the instance.

  python3 scripts/qa/mac-grade-preview-proof.py [--out DIR]

Exit 0 = pass; 1 = fail; 2 = harness (core/fake engine not built, no frames, IOSurface lookup
failed in this process). Missing pieces are LOUD, never a silent pass.
"""
import argparse
import ctypes
import hashlib
import importlib.util
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

BLACK_MAX = 24          # gain 0 is exact black; leave room for nothing but rounding
COMPARE_TOLERANCE = 6   # preview vs Program centre, per channel (same source texel, same shader)
SCOPE_TRACE_MIN = 40    # a scope pixel this bright is a trace, not the 0.012 (3/255) background

ADVANCED_BLACK = {
    "version": 2, "colorSpace": "rec709-sdr", "intensity": 1, "bypass": False,
    "operations": [{
        "id": "black", "kind": "primaries", "enabled": True, "intensity": 1,
        "exposureStops": 0, "contrast": 1, "pivot": 0.5, "saturation": 1, "temperature": 0,
        "tint": 0, "lift": 0, "gamma": 1, "gain": 0,
        "curves": [[{"x": 0, "y": 0}, {"x": 1, "y": 1}]] * 4,
        "cubeText": "", "cubeSha256": "", "name": ""}]}


def judge(program_before, graded, program_during, compare, program_after, scope_bright_pixels):
    """Pixels are (b, g, r) tuples or None. Returns {"verdict": pass|fail|harness, "reasons": [...]}."""
    if None in (program_before, graded, program_during, compare, program_after):
        return {"verdict": "harness", "reasons": ["a surface could not be read"]}
    if max(program_before) <= BLACK_MAX:
        return {"verdict": "harness", "reasons": ["Program is already dark; the run proved nothing"]}
    reasons = []
    if max(graded) > BLACK_MAX:
        reasons.append(f"graded preview is not black: {graded}")
    for name, pixel in (("during", program_during), ("after", program_after)):
        if max(abs(a - b) for a, b in zip(pixel, program_before)) > COMPARE_TOLERANCE:
            reasons.append(f"Program changed {name} the private preview: {program_before} -> {pixel}")
    if max(abs(a - b) for a, b in zip(compare, program_before)) > COMPARE_TOLERANCE:
        reasons.append(f"Compare original {compare} does not match Program {program_before}")
    if scope_bright_pixels <= 0:
        reasons.append("the scope image carries no trace")
    return {"verdict": "fail" if reasons else "pass", "reasons": reasons}


class Surfaces:
    """IOSurfaceLookup by global id, read-only lock, read one pixel / count bright pixels."""

    def __init__(self):
        self.io = ctypes.cdll.LoadLibrary("/System/Library/Frameworks/IOSurface.framework/IOSurface")
        self.cf = ctypes.cdll.LoadLibrary("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")
        self.io.IOSurfaceLookup.restype = ctypes.c_void_p
        self.io.IOSurfaceLookup.argtypes = [ctypes.c_uint32]
        for name in ("IOSurfaceGetWidth", "IOSurfaceGetHeight", "IOSurfaceGetBytesPerRow"):
            getattr(self.io, name).restype = ctypes.c_size_t
            getattr(self.io, name).argtypes = [ctypes.c_void_p]
        self.io.IOSurfaceGetBaseAddress.restype = ctypes.c_void_p
        self.io.IOSurfaceGetBaseAddress.argtypes = [ctypes.c_void_p]
        self.io.IOSurfaceLock.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p]
        self.io.IOSurfaceUnlock.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p]
        self.cf.CFRelease.argtypes = [ctypes.c_void_p]

    def _with(self, surface_id, body):
        ref = self.io.IOSurfaceLookup(int(surface_id)) if surface_id else None
        if not ref:
            return None
        self.io.IOSurfaceLock(ref, 1, None)
        try:
            w, h = self.io.IOSurfaceGetWidth(ref), self.io.IOSurfaceGetHeight(ref)
            stride, base = self.io.IOSurfaceGetBytesPerRow(ref), self.io.IOSurfaceGetBaseAddress(ref)
            data = ctypes.string_at(base, stride * h)
            return body(w, h, stride, data)
        finally:
            self.io.IOSurfaceUnlock(ref, 1, None)
            self.cf.CFRelease(ref)

    def centre(self, surface_id):
        def read(w, h, stride, data):
            at = (h // 2) * stride + (w // 2) * 4
            return tuple(data[at:at + 3])
        return self._with(surface_id, read)

    def size(self, surface_id):
        return self._with(surface_id, lambda w, h, stride, data: (w, h))

    def bright(self, surface_id, threshold=SCOPE_TRACE_MIN):
        def count(w, h, stride, data):
            n = 0
            for y in range(0, h, 4):
                row = data[y * stride:y * stride + w * 4]
                n += sum(1 for x in range(0, w * 4, 16) if max(row[x:x + 3]) >= threshold)
            return n
        return self._with(surface_id, count)


def _drill():
    spec = importlib.util.spec_from_file_location("show_drill", os.path.join(REPO, "scripts", "mac-show-drill.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default=os.path.join(REPO, "artifacts", "grade-preview-proof"))
    args = parser.parse_args()
    drill = _drill()
    if not os.path.exists(drill.DEFAULT_CORE) or not os.path.exists(drill.FAKE_ENGINE):
        print(f"HARNESS core or fake engine not built: {drill.DEFAULT_CORE} / {drill.FAKE_ENGINE}")
        return 2
    surfaces = Surfaces()
    os.makedirs(args.out, exist_ok=True)
    env = drill.load_env(1)
    core = drill.Core(drill.DEFAULT_CORE, env=env, evidence_path=os.path.join(args.out, "command-timing.json"))
    result = {}
    instance = "proof-editor"

    def latest(kind, predicate=lambda e: True):
        for event in reversed(core.events):
            if event.get("type") == kind and predicate(event):
                return event
        return None

    def preview(revision, enabled=True, compare=False):
        return core.request({"type": "set-grade-preview", "instanceId": instance, "sourceId": "101",
                             "revision": revision, "enabled": enabled, "scopesEnabled": True,
                             "scopesOriginal": False, "histogramMode": 1, "waveformMode": 1, "scopeView": 0,
                             "compareOriginal": compare,
                             "scopeRoi": {"enabled": False, "x": 0, "y": 0, "width": 1, "height": 1,
                                          "revision": 0, "shape": "rectangle"},
                             "grade": {"lut": "none", "exposure": 0, "contrast": 0, "saturation": 0,
                                       "temperature": 0, "advanced": {"documentRef": ref}}})

    def await_ready(revision, seconds=8.0):
        deadline = time.time() + seconds
        last_renew = 0.0
        while time.time() < deadline:
            core.sync([], 600, step="preview-wait")
            if time.time() - last_renew >= 0.5:      # the shell's renew cadence
                core.request({"type": "set-grade-preview", "instanceId": instance, "sourceId": "101",
                              "revision": revision, "renewOnly": True})
                last_renew = time.time()
            event = latest("grade-preview", lambda e: e.get("instanceId") == instance and e.get("revision") == revision
                           and e.get("status") == "ready" and (e.get("texture") or {}).get("iosurfaceId"))
            if event and ((event.get("scopes") or {}).get("texture") or {}).get("iosurfaceId"):
                return event
            time.sleep(0.1)
        return None

    try:
        time.sleep(1.5)
        if core.request({"type": "zoom-join", "payload": {"meetingNumber": "1234567890",
                                                          "displayName": "grade-preview-proof"}}, timeout=20.0) is None:
            print("HARNESS zoom-join timed out")
            return 2
        core.sync([{"type": "load-scene-graph", "sceneId": "grade-preview-proof",
                    "routes": [{"routeId": "full", "mode": "fixed", "participantId": "101", "audioRole": "mix",
                                "fitMode": "fill", "opacity": 1.0, "zIndex": 0,
                                "rect": {"x": 0, "y": 0, "width": 1, "height": 1}}]}], 500, step="scene")
        deadline = time.time() + 15
        while time.time() < deadline and not (core.request({"type": "query-source-grade", "sourceId": "101"}) or {}).get("present"):
            core.sync([], 600, step="wait")
            time.sleep(0.25)
        program_event = None
        deadline = time.time() + 5
        while time.time() < deadline and not program_event:
            core.sync([], 600, step="program-wait")
            program_event = latest("program-shared-texture", lambda e: (e.get("texture") or {}).get("iosurfaceId"))
            time.sleep(0.1)
        if not program_event:
            print("HARNESS no program-shared-texture with an iosurfaceId")
            return 2
        program_id = program_event["texture"]["iosurfaceId"]
        program_before = surfaces.centre(program_id)

        text = json.dumps(ADVANCED_BLACK, separators=(",", ":"), sort_keys=True)
        ref = hashlib.sha256(text.encode()).hexdigest()
        if not (core.request({"type": "register-grade-document", "documentRef": ref}) or {}).get("present"):
            if not (core.request({"type": "register-grade-document", "documentRef": ref, "documentJson": text}) or {}).get("present"):
                print("FAIL register-grade-document refused the gain-0 document")
                return 1
        accepted = preview(1)
        result["setGradePreview"] = accepted
        graded_event = await_ready(1)
        if not graded_event:
            print(json.dumps(dict(result, verdict="fail", reasons=["no ready grade-preview event with IOSurfaces"],
                                  lastEvent=latest("grade-preview"))))
            return 1
        graded = surfaces.centre(graded_event["texture"]["iosurfaceId"])
        program_during = surfaces.centre(program_id)
        scope_id = graded_event["scopes"]["texture"]["iosurfaceId"]
        scope_size = surfaces.size(scope_id)
        scope_bright = surfaces.bright(scope_id) or 0
        preview(2, compare=True)
        compare_event = await_ready(2)
        compare = surfaces.centre(compare_event["texture"]["iosurfaceId"]) if compare_event else None
        preview(2, enabled=False)
        time.sleep(0.5)
        closed_from = len(core.events)
        time.sleep(1.0)
        core.sync([], 600, step="closed")
        after_close = [e for e in core.events[closed_from:] if e.get("type") == "grade-preview" and e.get("instanceId") == instance]
        program_after = surfaces.centre(program_id)
        result.update({"programSurface": program_id, "previewSurface": graded_event["texture"]["iosurfaceId"],
                       "scopeSurface": scope_id, "scopeSize": scope_size, "scopeBrightSamples": scope_bright,
                       "pixels": {"programBefore": program_before, "graded": graded, "programDuring": program_during,
                                  "compare": compare, "programAfter": program_after},
                       "scopes": {k: graded_event["scopes"].get(k) for k in ("status", "sampleWidth", "sampleHeight",
                                                                             "sampleCount", "revision", "original")},
                       "eventsAfterClose": len(after_close)})
    finally:
        core.kill()

    verdict = judge(program_before, graded, program_during, compare, program_after, scope_bright)
    if scope_size != (1536, 512):
        verdict["reasons"].append(f"scope surface is {scope_size}, not 1536x512")
        verdict["verdict"] = "fail"
    if result.get("eventsAfterClose"):
        verdict["reasons"].append(f"{result['eventsAfterClose']} events after enabled:false")
        verdict["verdict"] = "fail"
    result.update(verdict)
    print(json.dumps(result, separators=(",", ":")))
    return {"pass": 0, "fail": 1}.get(verdict["verdict"], 2)


if __name__ == "__main__":
    sys.exit(main())
