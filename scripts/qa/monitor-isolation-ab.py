#!/usr/bin/env python3
"""Maintained #794 A/B harness. Run --help; see monitor-isolation-ab.md."""

import argparse, pathlib, json, time, hashlib, mmap, struct, threading, uuid, sys, math, subprocess
from contextlib import ExitStack
from monitor_evidence import Core, judge, counters, judge_recording


def fixed(route_id, pid):
    return {
        "routeId": route_id,
        "mode": "fixed",
        "participantId": pid,
        "audioRole": "mix",
        "fitMode": "fill",
        "opacity": 1.0,
        "zIndex": 0,
        "rect": {"x": 0, "y": 0, "width": 1, "height": 1},
    }


def main():
    ap = argparse.ArgumentParser(
        description="Synthetic mixed-source monitor A/B; not release qualification"
    )
    ap.add_argument("--core", required=True, type=pathlib.Path)
    ap.add_argument("--fake", required=True, type=pathlib.Path)
    ap.add_argument("--output", required=True, type=pathlib.Path)
    ap.add_argument(
        "--source-commit",
        required=True,
        help="Commit that built the supplied Release core; never infer from checkout",
    )
    ap.add_argument("--duration", type=float, default=120)
    ap.add_argument("--pairs", type=int, default=3)
    ap.add_argument("--warmup", type=float, default=10)
    ap.add_argument(
        "--cpu-source-preparation", choices=("0", "1"), default="0",
        help="Explicit SHM BGRA GPU preparation override; held constant across both monitor modes",
    )
    a = ap.parse_args()
    if sys.platform != "win32":
        ap.error("Windows named mappings and D3D11 required")
    if (
        not math.isfinite(a.duration)
        or not math.isfinite(a.warmup)
        or a.duration <= 0
        or a.pairs < 1
        or not 0 <= a.warmup <= 30
    ):
        ap.error("positive duration/pairs and warmup 0..30 required")
    if not a.core.is_file() or not a.fake.is_file():
        ap.error("core and fake executables must exist")
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    exe = a.core.resolve()
    fake = a.fake.resolve()
    results = []
    manifest = {
        "harnessSha256": hashlib.sha256(
            pathlib.Path(__file__).read_bytes()
        ).hexdigest(),
        "driverSha256": hashlib.sha256(
            pathlib.Path(__file__).with_name("monitor_evidence.py").read_bytes()
        ).hexdigest(),
        "sourceCommit": a.source_commit,
        "buildConfiguration": "Release (operator supplied binary)",
        "flags": {
            "COREVIDEO_PROGRAM_BUFFER_FRAMES": "2",
            "COREVIDEO_GPU_CAPTURE": "0",
            "COREVIDEO_CPU_SOURCE_PREPARATION": a.cpu_source_preparation,
            "COREVIDEO_FAKE_ENGINE_AUTOSUBSCRIBE": "0",
            "COREVIDEO_ISOLATE_MONITORS": "0 and 1",
        },
        "corePath": str(exe),
        "coreSha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
        "fakePath": str(fake),
        "fakeSha256": hashlib.sha256(fake.read_bytes()).hexdigest(),
        "duration": a.duration,
        "pairs": a.pairs,
        "warmup": a.warmup,
        "programBufferFrames": 2,
        "sourceFormats": [
            "8 fake Zoom I420 1920x1080 30fps",
            "BGRA mapping 1920x1080 60Hz",
            "BGRA mapping 2560x1440 60Hz",
        ],
        "output": "local Program MP4",
        "scope": "Synthetic ingress, no physical capture, display reader, receiver, real meeting, streaming or camera publication",
        "trials": results,
    }
    try:
        gpu = subprocess.run(
            [
                "powershell",
                "-NoProfile",
                "-Command",
                "Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,PNPDeviceID | ConvertTo-Json -Compress",
            ],
            capture_output=True,
            text=True,
            timeout=20,
            creationflags=subprocess.CREATE_NO_WINDOW,
        )
        manifest["hardware"] = (
            json.loads(gpu.stdout) if gpu.returncode == 0 else "MISSING_EVIDENCE"
        )
    except Exception:
        manifest["hardware"] = "MISSING_EVIDENCE"
    (out / "report.json").write_text(json.dumps(manifest, indent=2))
    orders = [
        (False, True) if pair % 2 == 0 else (True, False) for pair in range(a.pairs)
    ]
    for pair, order in enumerate(orders):
        for isolated in order:
            label = f"pair-{pair+1}-" + ("isolated" if isolated else "inline")
            run_trial(a, exe, fake, out, label, isolated, results)
            (out / "report.json").write_text(json.dumps(manifest, indent=2))
    return (
        1
        if any(
            r.get("programBufferVerdict") != "PASS"
            or r.get("recordingVerdict") != "PASS"
            for r in results
        )
        else 0
    )


def run_trial(a, exe, fake, out, label, isolated, results):
    print("START " + label, flush=True)
    core = None
    resources = ExitStack()
    sample_file = None
    start = time.monotonic()
    samples = []
    captures = []
    stop = threading.Event()
    publisher = None

    def make_core():
        return Core(
            str(exe),
            {
                "COREVIDEO_ZOOM_ENGINE_PATH": str(fake),
                "COREVIDEO_ISOLATE_MONITORS": "1" if isolated else "0",
                "COREVIDEO_PROGRAM_BUFFER_FRAMES": "2",
                "COREVIDEO_FAKE_ENGINE_PARTICIPANTS": "8",
                "COREVIDEO_FAKE_ENGINE_RES": "2",
                "COREVIDEO_FAKE_ENGINE_FPS": "30",
                "COREVIDEO_FAKE_NO_CHURN": "1",
                "COREVIDEO_FAKE_ENGINE_AUTOSUBSCRIBE": "0",
                "COREVIDEO_GPU_CAPTURE": "0",
                "COREVIDEO_CPU_SOURCE_PREPARATION": a.cpu_source_preparation,
            },
            out / (label + ".stderr.log"),
        )

    def sync(commands=None):
        response = core.sync(commands or [], int((time.monotonic() - start) * 1000))
        assert response and response.get("ok"), "sync failed " + str(response)
        return response["snapshot"]

    def select(s):
        return {
            k: s.get(k)
            for k in [
                "programBuffer",
                "realtimeEvidence",
                "compositor",
                "sources",
                "programFrameCount",
                "outputProfile",
                "previewSharedTexture",
                "multiviewSharedTexture",
                "recording",
            ]
        }

    try:
        sample_file = resources.enter_context(open(out / (label + ".jsonl"), "w"))
        core = make_core()
        join = core.request(
            {
                "type": "zoom-join",
                "payload": {
                    "meetingNumber": "1234567890",
                    "displayName": "Synthetic isolation QA",
                },
            },
            timeout=20,
        )
        if not join.get("ok"):
            raise RuntimeError("fake engine join refused")
        pids = [str(101 + i) for i in range(8)]
        response = core.request(
            {
                "type": "zoom-media-spine-sync",
                "elapsedMs": 0,
                "payload": {
                    "startCapture": True,
                    "subscriptions": [
                        {
                            "participantId": p,
                            "kind": "participant-video",
                            "purpose": "program" if i == 0 else "preview",
                            "priority": i,
                        }
                        for i, p in enumerate(pids)
                    ],
                    "sourceParticipantIds": pids,
                },
            }
        )
        assert response and response.get("ok"), "spine failed"
        for device, w, h in [("qa-webcam", 1920, 1080), ("qa-screen", 2560, 1440)]:
            name = "Local\\CoreVideo-QA-" + uuid.uuid4().hex
            mapping = resources.enter_context(
                mmap.mmap(-1, 16 + w * h * 4, tagname=name)
            )
            mapping[16:] = bytes([40, 80, 120, 255]) * (w * h)
            struct.pack_into("<IIII", mapping, 0, 2, w, h, 0)
            captures.append((device, mapping))
            reply = core.request(
                {
                    "type": "register-capture-shm",
                    "payload": {
                        "deviceId": device,
                        "shmName": name,
                        "width": w,
                        "height": h,
                    },
                }
            )
            assert reply and reply.get("ok"), "capture registration failed"

        def publish():
            sequence = 2
            deadline = time.monotonic()
            while not stop.is_set():
                sequence = (sequence + 2) & 0xFFFFFFFE
                for _, mapping in captures:
                    struct.pack_into("<I", mapping, 0, sequence - 1)
                    struct.pack_into("<I", mapping, 16, sequence)
                    struct.pack_into("<I", mapping, 0, sequence)
                deadline = max(deadline + 1 / 60, time.monotonic())
                stop.wait(max(0, deadline - time.monotonic()))

        publisher = threading.Thread(target=publish)
        publisher.start()
        sync(
            [
                {"type": "set-verbose-diagnostics", "enabled": True},
                {
                    "type": "set-output-profile",
                    "width": 1920,
                    "height": 1080,
                    "fps": 60,
                },
                {
                    "type": "load-scene-graph",
                    "sceneId": "ab-program",
                    "routes": [fixed("pgm", "101")],
                },
                {
                    "type": "set-preview-scene",
                    "sceneId": "ab-preview",
                    "routes": [fixed("pvw", "102")],
                },
                {
                    "type": "set-multiview-layout",
                    "canvasWidth": 1920,
                    "canvasHeight": 1080,
                    "sources": [
                        {
                            "sourceId": p,
                            "kind": "zoom",
                            "participantId": p,
                            "slot": i,
                            "label": "Synthetic " + p,
                        }
                        for i, p in enumerate(pids)
                    ]
                    + [
                        {
                            "sourceId": "capture:" + d,
                            "kind": "capture",
                            "captureDeviceId": d,
                            "slot": 8 + i,
                            "label": d,
                        }
                        for i, (d, _) in enumerate(captures)
                    ],
                },
            ]
        )
        folder = out / label
        folder.mkdir(exist_ok=True)
        sync(
            [
                {
                    "type": "start-program-output",
                    "destinations": ["recording"],
                    "isoSourceIds": [],
                    "isoParticipantIds": [],
                },
                {
                    "type": "set-recording-targets",
                    "targetFolder": str(folder),
                    "filenamePrefix": "qa",
                    "format": "mp4",
                    "quality": "high",
                    "isoSourceIds": [],
                    "isoParticipantIds": [],
                },
                {"type": "start-recording-session", "sessionId": label},
            ]
        )
        warm = time.monotonic() + a.warmup
        while time.monotonic() < warm:
            sync()
            time.sleep(0.25)
        first = sync()
        before = time.monotonic()
        previous_progress = before
        formats = [
            (s["width"], s["height"])
            for s in first["sources"]
            if s.get("kind") == "zoom"
        ]
        if len(formats) != 8 or any(f != (1920, 1080) for f in formats):
            raise RuntimeError("Zoom formats differ from required 8 x 1080p")
        assert first.get("recording", {}).get(
            "active"
        ), "recording failed " + json.dumps(first.get("recording"))
        assert (
            first["realtimeEvidence"]["monitorWorker"]["enabled"] == isolated
        ), "flag not applied"
        assert (
            len(
                [
                    s
                    for s in first.get("sources", [])
                    if s.get("kind") == "zoom" and s.get("framesIngested", 0) > 0
                ]
            )
            >= 8
        ), "sources not flowing"
        assert (
            len(
                [
                    s
                    for s in first.get("sources", [])
                    if s.get("kind") == "capture" and s.get("framesIngested", 0) > 0
                ]
            )
            == 2
        ), "capture sources not flowing"

        def retain(snapshot):
            if (
                snapshot["programBuffer"]["generation"]
                != first["programBuffer"]["generation"]
            ):
                raise RuntimeError("delivery generation changed")
            if any(
                snapshot["programBuffer"][k] != 2
                for k in ["requestedFrames", "activeFrames"]
            ):
                raise RuntimeError("two-frame buffer not active")
            if any(
                snapshot["outputProfile"][k] != v
                for k, v in [("width", 1920), ("height", 1080), ("fps", 60)]
            ):
                raise RuntimeError("Program profile changed")
            row = select(snapshot)
            sample_file.write(
                json.dumps(
                    {"elapsedSeconds": time.monotonic() - before, "snapshot": row}
                )
                + "\n"
            )
            sample_file.flush()
            # Keep only evidence counters in RAM; texture/source metadata stays on disk.
            current = {k: row[k] for k in ["programBuffer", "realtimeEvidence"]}
            current["recording"] = {"proof": row["recording"]["proof"]}
            if samples:
                # Validate every adjacent observation, then keep first and most recent.
                old, new = counters(samples[-1]), counters(current)
                if any(new[k] < old[k] for k in old):
                    raise ValueError("counter reset during trial")
            if len(samples) == 2:
                samples.pop()
            samples.append(current)
            if not snapshot.get("recording", {}).get("active"):
                raise RuntimeError("recording stopped during trial")
            if snapshot["realtimeEvidence"]["monitorWorker"]["enabled"] != isolated:
                raise RuntimeError("isolation mode changed")

        retain(first)
        while time.monotonic() - before < a.duration:
            retain(sync())
            time.sleep(0.25)
            if time.monotonic() - previous_progress >= 30:
                print(
                    label + " measuring " + str(round(time.monotonic() - before)) + "s",
                    flush=True,
                )
                previous_progress = time.monotonic()
        last = sync()
        retain(last)
        seconds = time.monotonic() - before
        result = {
            "label": label,
            "isolated": isolated,
            "seconds": seconds,
            **judge(samples, isolated),
            **judge_recording(first, last),
        }
        result["observedSourceFormats"] = [
            {
                k: s.get(k)
                for k in ["sourceId", "kind", "width", "height", "framesIngested"]
            }
            for s in first["sources"]
        ]
        results.append(result)
        print(json.dumps(result), flush=True)
        sync(
            [
                {"type": "stop-recording-session", "reason": "QA complete"},
                {"type": "stop-encoder-session", "reason": "QA complete"},
            ]
        )
    except Exception as error:
        failure = str(error)
        if results and results[-1]["label"] == label:
            results[-1].update(programBufferVerdict="INVALID", error=failure)
        else:
            results.append(
                {
                    "label": label,
                    "isolated": isolated,
                    "programBufferVerdict": "INVALID",
                    "error": failure,
                }
            )
        print(label + " INVALID " + failure, flush=True)
    finally:
        stop.set()
        if publisher:
            publisher.join(timeout=3)
        try:
            if core:
                core.close()
        finally:
            resources.close()
        (out / (label + ".json")).write_text(json.dumps(results[-1], indent=2))


if __name__ == "__main__":
    sys.exit(main())
