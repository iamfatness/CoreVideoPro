#!/usr/bin/env python3
"""Owned synthetic Program counter through the real OS camera receiver; reference workload only."""
import argparse
import hashlib
import json
import math
import os
import pathlib
import shutil
import subprocess
import sys
import time
from monitor_evidence import Core, judge
from camera_serving_evidence import ServeLogCapture, file_digest, service_identity, judge_serving_identity, read_correlation


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core", type=pathlib.Path, required=True)
    parser.add_argument("--receiver", type=pathlib.Path, required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--expected-camera-dll", type=pathlib.Path,
        help="Installed candidate DLL to hash; defaults to corevideo-virtualcam.dll beside --core")
    parser.add_argument("--duration", type=int, default=75)
    parser.add_argument("--monitors", action="store_true", help="Also exercise the Program/Preview multiview handoff; sparse observations are not display pixel proof")
    args = parser.parse_args()
    node = shutil.which("node")
    if sys.platform != "win32" or not args.core.is_file() or not args.receiver.is_file() or not node:
        parser.error("Windows, existing core/receiver binaries and Node are required")
    if not 65 <= args.duration <= 7200:
        parser.error("duration must be 65..7200 seconds (30-second receiver warmup plus measured reference)")
    expected_dll = (args.expected_camera_dll or args.core.resolve().with_name("corevideo-virtualcam.dll")).resolve()
    if not expected_dll.is_file():
        parser.error("Expected installed camera DLL is missing; provide --expected-camera-dll")
    expected_sha = file_digest(expected_dll)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    flags = {"COREVIDEO_CPU_SOURCE_PREPARATION": "1", "COREVIDEO_ISOLATE_MONITORS": "1",
             "COREVIDEO_PROGRAM_BUFFER_FRAMES": "2", "COREVIDEO_GPU_CAPTURE": "0",
             "COREVIDEO_QA_PROGRAM_COUNTER": "1", "COREVIDEO_DELIVERY_TRACE": "1"}
    report = {"sourceCommit": args.source_commit, "monitorsRequested": args.monitors,
              "coreSha256": hashlib.sha256(args.core.read_bytes()).hexdigest(),
              "receiverSha256": hashlib.sha256(args.receiver.read_bytes()).hexdigest(),
              "harnessSha256": hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest(),
              "servingEvidenceHelperSha256": file_digest(pathlib.Path(__file__).with_name("camera_serving_evidence.py")),
              "flags": flags, "duration": args.duration, "warmup": 30, "osEnumerationSettleSeconds": 2,
              "scope": "Synthetic counter-only reference through the OS camera with serving-module evidence; no full input workload, physical display, content latency, decoded A/V, memory-image digest or fleet qualification."}
    core = receiver = None
    service_before = service_after = None
    service_errors = []
    evidence_dir = pathlib.Path(os.environ.get("ProgramData", "C:/ProgramData")) / "CoreVideoPro"
    capture = ServeLogCapture(evidence_dir / "vcam-serve.log", output / "serve.log")
    started = time.monotonic()
    raw_path = output / "receiver.ndjson"
    try:
        core = Core(str(args.core.resolve()), flags, output / "core.stderr.log")
        commands = [{"type": "set-output-profile", "width": 1920, "height": 1080, "fps": 60},
                    {"type": "sync-virtual-camera", "on": True, "mirror": False,
                     "deviceName": "CoreVideo Pro Camera"},
                    {"type": "start-program-output", "destinations": ["virtual-camera"], "isoParticipantIds": []}]
        if args.monitors:
            commands.insert(1, {"type": "configure-multiviewer", "layoutMode": "pgmPvwTop", "showLabels": False})
        response = core.sync(commands, 0)
        (output / "setup.json").write_text(json.dumps(response, indent=2))
        if not response.get("ok"):
            raise RuntimeError("reference output setup refused")
        camera = response.get("snapshot", {}).get("virtualCamera", {})
        if camera.get("status") == "failed":
            raise RuntimeError("OS camera activation failed: " + str(camera.get("warning", "unknown reason")))
        if not camera.get("enabled"):
            raise RuntimeError("OS camera output did not become enabled; setup evidence is incomplete")
        # MFCreateVirtualCamera::Start can precede the endpoint notification.
        # Fixed setup settle; the receiver still gets its full pinned warmup.
        time.sleep(2)
        capture.poll()
        try:
            service_before = service_identity()
        except Exception as error:
            service_errors.append(str(error))
        with raw_path.open("wb") as raw, (output / "receiver.stderr.log").open("wb") as error:
            receiver = subprocess.Popen([str(args.receiver.resolve()), str(args.duration)],
                stdout=raw, stderr=error, stdin=subprocess.DEVNULL,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            measured_start = time.monotonic()
            first = last = None
            with (output / "samples.jsonl").open("w") as samples:
                while receiver.poll() is None:
                    now = time.monotonic()
                    if now - measured_start > args.duration + 40:
                        raise RuntimeError("bounded receiver deadline exceeded")
                    response = core.sync([], int((now - started) * 1000))
                    if not response.get("ok"):
                        raise RuntimeError("reference snapshot refused")
                    current = response["snapshot"]
                    capture.poll()
                    measured = now - measured_start >= 30
                    samples.write(json.dumps({"elapsed": now - measured_start, "measured": measured,
                        "snapshot": {key: current.get(key) for key in ["programBuffer", "realtimeEvidence", "compositor", "virtualCamera"]},
                        "cameraCorrelation": read_correlation(evidence_dir / "vcam-correlation-v1.shm")}) + "\n")
                    if measured:
                        first = first or current
                        last = current
                    time.sleep(.25)
            report["receiverExitCode"] = receiver.returncode
        # This reference demands camera output only; optional monitor progress
        # is not inferred from the isolation launch flag.
        if receiver.returncode != 0:
            raise RuntimeError("OS camera receiver exited " + str(receiver.returncode) + ": " +
                (output / "receiver.stderr.log").read_text(errors="replace")[-2000:])
        if first is None or last is None:
            raise RuntimeError("receiver ended before a measured native interval was available")
        report["nativeBuffer"] = judge([first, last], args.monitors)
        if args.monitors:
            begin_display = first.get("realtimeEvidence", {}).get("monitorWorker", {}).get("programDisplay", {})
            end_display = last.get("realtimeEvidence", {}).get("monitorWorker", {}).get("programDisplay", {})
            valid = all(node.get("observationVersion") == "program-display-snapshot-v1" and node.get("observed") is True
                        for node in (begin_display, end_display))
            fields = ("copies", "completedCopies", "reused", "refused", "skipped", "reordered")
            if not valid or any(type(node.get(key)) not in (int, float) or not math.isfinite(node[key]) or
                                node[key] < 0 or node[key] != math.floor(node[key])
                                for node in (begin_display, end_display) for key in fields):
                raise ValueError("Program display copy evidence is missing or malformed")
            if begin_display.get("generation") != end_display.get("generation"):
                raise ValueError("Program display generation changed during the reference")
            delta = {name: end_display.get(name, 0) - begin_display.get(name, 0)
                     for name in fields}
            if any(value < 0 for value in delta.values()):
                raise ValueError("Program display counters reset during the reference")
            report["programDisplayObservation"] = {
                "valid": valid, "delta": delta, "last": end_display,
                "displayPixelsVerified": False,
                "scope": "Sparse native copy identities only; separate GPU pixel phase tests are required"}
        verdict = subprocess.run([node, str(pathlib.Path(__file__).with_name("camera-pixel-receiver.mjs")), str(raw_path)],
            capture_output=True, text=True, timeout=30,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        if verdict.returncode not in (0, 1):
            raise RuntimeError("receiver evidence judge failed: " + verdict.stderr[-1000:])
        report["cameraPixels"] = json.loads(verdict.stdout)
        report["result"] = "PASS" if receiver.returncode == 0 and report["nativeBuffer"]["programBufferVerdict"] == "PASS" and report["cameraPixels"]["osCameraContinuityVerified"] else "FAIL"
        if args.monitors and (not valid or delta.get("copies", 0) <= 0 or delta.get("completedCopies", 0) <= 0 or
                              any(delta.get(key, 0) != 0 for key in ("reused", "refused", "skipped", "reordered"))):
            report["result"] = "FAIL"
    except Exception as error:
        report["result"] = "INVALID"
        report["error"] = str(error)
    finally:
        if receiver and receiver.poll() is None:
            receiver.kill()
            receiver.wait(timeout=10)
        try:
            service_after = service_identity()
        except Exception as error:
            service_errors.append(str(error))
        if core:
            try:
                core.close()
            except Exception as error:
                report["cleanupError"] = str(error)
                report["result"] = "INVALID"
        capture.poll()
        report["servingIdentity"] = judge_serving_identity(capture, service_before, service_after, expected_dll, expected_sha)
        report["servingIdentity"]["serviceQueryErrors"] = service_errors
        if service_errors:
            report["servingIdentity"]["servingIdentityVerified"] = False
        report["servingIdentityVerified"] = report["servingIdentity"]["servingIdentityVerified"]
        if report.get("result") == "PASS" and not report["servingIdentityVerified"]:
            report["result"] = "INVALID"
            report["qualificationError"] = "Candidate camera serving identity was not verified; pixel verdict is retained separately."
        (output / "report.json").write_text(json.dumps(report, indent=2))
    print(json.dumps({"result": report["result"], "report": str(output / "report.json")}))
    return 0 if report["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
