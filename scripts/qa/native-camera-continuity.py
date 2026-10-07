#!/usr/bin/env python3
"""Owned synthetic Program counter through the real OS camera receiver; reference workload only."""
import argparse
import hashlib
import json
import math
import pathlib
import shutil
import subprocess
import sys
import time
from monitor_evidence import Core, judge


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core", type=pathlib.Path, required=True)
    parser.add_argument("--receiver", type=pathlib.Path, required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--duration", type=int, default=75)
    args = parser.parse_args()
    node = shutil.which("node")
    if sys.platform != "win32" or not args.core.is_file() or not args.receiver.is_file() or not node:
        parser.error("Windows, existing core/receiver binaries and Node are required")
    if not 65 <= args.duration <= 7200:
        parser.error("duration must be 65..7200 seconds (30-second receiver warmup plus measured reference)")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    flags = {"COREVIDEO_CPU_SOURCE_PREPARATION": "1", "COREVIDEO_ISOLATE_MONITORS": "1",
             "COREVIDEO_PROGRAM_BUFFER_FRAMES": "2", "COREVIDEO_GPU_CAPTURE": "0",
             "COREVIDEO_QA_PROGRAM_COUNTER": "1"}
    report = {"sourceCommit": args.source_commit,
              "coreSha256": hashlib.sha256(args.core.read_bytes()).hexdigest(),
              "receiverSha256": hashlib.sha256(args.receiver.read_bytes()).hexdigest(),
              "harnessSha256": hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest(),
              "flags": flags, "duration": args.duration, "warmup": 30,
              "scope": "Synthetic counter-only reference through the OS camera; no full input workload, physical display, content latency, decoded A/V, installed-module or fleet qualification."}
    core = receiver = None
    started = time.monotonic()
    raw_path = output / "receiver.ndjson"
    try:
        core = Core(str(args.core.resolve()), flags, output / "core.stderr.log")
        commands = [{"type": "set-output-profile", "width": 1920, "height": 1080, "fps": 60},
                    {"type": "sync-virtual-camera", "on": True, "mirror": False,
                     "deviceName": "CoreVideo Pro Camera"},
                    {"type": "start-program-output", "destinations": ["virtual-camera"], "isoParticipantIds": []}]
        response = core.sync(commands, 0)
        if not response.get("ok"):
            raise RuntimeError("reference output setup refused")
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
                    measured = now - measured_start >= 30
                    samples.write(json.dumps({"elapsed": now - measured_start, "measured": measured,
                        "snapshot": {key: current.get(key) for key in ["programBuffer", "realtimeEvidence", "compositor", "virtualCamera"]}}) + "\n")
                    if measured:
                        first = first or current
                        last = current
                    time.sleep(.25)
            report["receiverExitCode"] = receiver.returncode
        # This reference demands camera output only; optional monitor progress
        # is not inferred from the isolation launch flag.
        report["nativeBuffer"] = judge([first, last], False)
        verdict = subprocess.run([node, str(pathlib.Path(__file__).with_name("camera-pixel-receiver.mjs")), str(raw_path)],
            capture_output=True, text=True, timeout=30,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        if verdict.returncode not in (0, 1):
            raise RuntimeError("receiver evidence judge failed: " + verdict.stderr[-1000:])
        report["cameraPixels"] = json.loads(verdict.stdout)
        report["result"] = "PASS" if receiver.returncode == 0 and report["nativeBuffer"]["programBufferVerdict"] == "PASS" and report["cameraPixels"]["osCameraContinuityVerified"] else "FAIL"
    except Exception as error:
        report["result"] = "INVALID"
        report["error"] = str(error)
    finally:
        if receiver and receiver.poll() is None:
            receiver.kill()
            receiver.wait(timeout=10)
        if core:
            try:
                core.close()
            except Exception as error:
                report["cleanupError"] = str(error)
                report["result"] = "INVALID"
        (output / "report.json").write_text(json.dumps(report, indent=2))
    print(json.dumps({"result": report["result"], "report": str(output / "report.json")}))
    return 0 if report["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
