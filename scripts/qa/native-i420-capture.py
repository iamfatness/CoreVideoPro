#!/usr/bin/env python3
"""Owned-core physical MF capture test; not receiver/display qualification."""
import argparse
import hashlib
import json
import math
import pathlib
import sys
import time
import uuid
from monitor_evidence import Core, SourceAdmissionJudge, NativeCaptureInputJudge, judge


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core", required=True, type=pathlib.Path)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--device-id", required=True, help="Exact native list-capture-devices id")
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--duration", type=float, default=120)
    parser.add_argument("--warmup", type=float, default=15)
    parser.add_argument("--reconnects", type=int, default=0)
    parser.add_argument("--cpu-source-preparation", choices=("0", "1"), default="1")
    args = parser.parse_args()
    if sys.platform != "win32" or not args.core.is_file():
        parser.error("Windows and an existing Release core are required")
    if not math.isfinite(args.duration) or args.duration <= 0 or not math.isfinite(args.warmup) or not 0 <= args.warmup <= 30 or not 0 <= args.reconnects <= 10:
        parser.error("positive finite duration, warmup 0..30 and reconnects 0..10 required")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    flags = {"COREVIDEO_CPU_SOURCE_PREPARATION": args.cpu_source_preparation,
             "COREVIDEO_ISOLATE_MONITORS": "1", "COREVIDEO_PROGRAM_BUFFER_FRAMES": "2",
             "COREVIDEO_GPU_CAPTURE": "0"}
    report = {"sourceCommit": args.source_commit, "buildConfiguration": "Release (operator supplied)",
              "corePath": str(args.core.resolve()), "coreSha256": hashlib.sha256(args.core.read_bytes()).hexdigest(),
              "harnessSha256": hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest(),
              "driverSha256": hashlib.sha256(pathlib.Path(__file__).with_name("monitor_evidence.py").read_bytes()).hexdigest(),
              "flags": flags, "deviceId": args.device_id, "durationPerPhase": args.duration,
              "warmupPerPhase": args.warmup, "phases": [],
              "scope": "Physical MF capture into owned development core. Sampled GPU admission and native buffer counters; no tagged input pixels, acquisition clock, installed/display/receiver, A/V or fleet qualification."}
    core = None
    connected = False
    start = time.monotonic()
    source_alias = "qa-native-uvc-" + uuid.uuid4().hex
    source_id = "capture:" + source_alias

    def request(body):
        reply = core.request(body, timeout=20)
        if not reply.get("ok"):
            raise RuntimeError("request refused: " + str(body.get("type")))
        return reply

    def snapshot(commands=None):
        reply = core.sync(commands or [], int((time.monotonic() - start) * 1000))
        if not reply.get("ok"):
            raise RuntimeError("snapshot refused")
        return reply["snapshot"]

    try:
        core = Core(str(args.core.resolve()), flags, output / "core.stderr.log")
        devices = request({"type": "list-capture-devices"})
        (output / "devices.json").write_text(json.dumps(devices, indent=2))
        device = next((item for item in devices.get("devices", []) if item.get("id") == args.device_id), None)
        if not device or device.get("vendor") != "uvc":
            raise RuntimeError("selected native MF/UVC device is unavailable")
        report["device"] = device
        commands = [{"type": "set-verbose-diagnostics", "enabled": True},
                    {"type": "set-output-profile", "width": 1920, "height": 1080, "fps": 60},
                    {"type": "load-scene-graph", "sceneId": "native-uvc-proof", "routes": [
                        {"routeId": "uvc", "mode": "capture-input", "captureDeviceId": source_alias,
                         "fitMode": "fill", "opacity": 1.0, "zIndex": 0,
                         "rect": {"x": 0, "y": 0, "width": 1, "height": 1}}]}]
        previous_epoch = None
        previous_native = None
        for index in range(args.reconnects + 1):
            connect = request({"type": "connect-capture-device", "payload": {"deviceId": args.device_id, "outputSourceId": source_alias}})
            connected = True
            (output / f"phase-{index}-connect.json").write_text(json.dumps(connect, indent=2))
            phase = {"index": index, "result": "INVALID"}
            report["phases"].append(phase)
            admission = SourceAdmissionJudge([source_id], args.cpu_source_preparation == "1")
            capture_input = NativeCaptureInputJudge(source_id)
            first = last = None
            measured_started = None
            warmup_end = time.monotonic() + args.warmup
            with (output / f"phase-{index}-samples.jsonl").open("w") as samples:
                while measured_started is None or time.monotonic() - measured_started < args.duration:
                    current = snapshot(commands); commands = []
                    now = time.monotonic()
                    measured = now >= warmup_end
                    samples.write(json.dumps({"elapsed": now - start, "measured": measured,
                        "snapshot": {key: current.get(key) for key in ["captureDevices", "sources", "programSourceAdmission", "programBuffer", "realtimeEvidence", "compositor"]}}) + "\n")
                    samples.flush()
                    if measured:
                        if first is None:
                            first = current; measured_started = now
                        last = current
                        admission.observe(current)
                        capture_input.observe(current)
                    time.sleep(.25)
            phase["nativeBuffer"] = judge([first, last], True)
            if previous_native is not None:
                phase["transitionNativeBuffer"] = judge([previous_native, first], True)
            previous_native = last
            phase["sourceAdmission"] = admission.result()
            phase["captureInput"] = capture_input.result()
            phase["captureDevices"] = last.get("captureDevices")
            rows = last.get("programSourceAdmission", {}).get("sources", [])
            row = next((item for item in rows if item.get("sourceId") == source_id), None)
            phase["lastAdmission"] = row
            if args.cpu_source_preparation == "1":
                epoch = row.get("actualEpoch") if row else None
                phase["reconnectEpochVerdict"] = "PASS" if type(epoch) in (int, float) and epoch > 0 and (previous_epoch is None or epoch > previous_epoch) else "FAIL"
                previous_epoch = epoch
            else:
                phase["reconnectEpochVerdict"] = "NOT_REQUESTED"
            phase["result"] = "PASS" if phase["nativeBuffer"]["programBufferVerdict"] == "PASS" and phase.get("transitionNativeBuffer", {}).get("programBufferVerdict", "PASS") == "PASS" and phase["captureInput"]["captureInputVerdict"] == "PASS" and phase["sourceAdmission"]["sampledSourceAdmissionVerdict"] != "FAIL" and phase["reconnectEpochVerdict"] != "FAIL" else "FAIL"
            request({"type": "disconnect-capture-device", "payload": {"deviceId": args.device_id}})
            connected = False
            (output / "report.json").write_text(json.dumps(report, indent=2))
        report["result"] = "PASS" if all(phase["result"] == "PASS" for phase in report["phases"]) else "FAIL"
    except Exception as error:
        report["result"] = "INVALID"
        report["error"] = str(error)
    finally:
        if core:
            if connected:
                try:
                    request({"type": "disconnect-capture-device", "payload": {"deviceId": args.device_id}})
                except Exception as error:
                    report["cleanupError"] = str(error); report["result"] = "INVALID"
            try:
                core.close()
            except Exception as error:
                report["cleanupError"] = str(error); report["result"] = "INVALID"
        (output / "report.json").write_text(json.dumps(report, indent=2))
    print(json.dumps({"result": report["result"], "report": str(output / "report.json")}))
    return 0 if report["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
