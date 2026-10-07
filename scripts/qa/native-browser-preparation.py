#!/usr/bin/env python3
"""Owned real WebView2/SHM trial; sampled ingress and Program counters, not receiver qualification."""
import argparse
import hashlib
import json
import math
import pathlib
import sys
import time
from urllib.parse import quote
from monitor_evidence import Core, SourceAdmissionJudge, NativeCaptureInputJudge, judge


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core", type=pathlib.Path, required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--cpu-source-preparation", choices=("0", "1"), default="1")
    parser.add_argument("--duration", type=float, default=120)
    parser.add_argument("--warmup", type=float, default=15)
    args = parser.parse_args()
    if sys.platform != "win32" or not args.core.is_file() or not math.isfinite(args.duration) or args.duration <= 0 or not math.isfinite(args.warmup) or not 0 <= args.warmup <= 30:
        parser.error("Windows, a Release core, positive finite duration and warmup 0..30 required")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    flags = {"COREVIDEO_CPU_SOURCE_PREPARATION": args.cpu_source_preparation,
             "COREVIDEO_ISOLATE_MONITORS": "1", "COREVIDEO_PROGRAM_BUFFER_FRAMES": "2",
             "COREVIDEO_GPU_CAPTURE": "0"}
    report = {"sourceCommit": args.source_commit, "corePath": str(args.core.resolve()),
              "coreSha256": hashlib.sha256(args.core.read_bytes()).hexdigest(),
              "harnessSha256": hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest(),
              "driverSha256": hashlib.sha256(pathlib.Path(__file__).with_name("monitor_evidence.py").read_bytes()).hexdigest(),
              "flags": flags, "duration": args.duration, "warmup": args.warmup,
              "scope": "Actual WebView2 host/SHM at 1920x1080@60 requested, sampled CPU ingress/GPU admission and native delivery counters. Not per-frame host acquisition, pixel/display/receiver delivery, A/V, installed or fleet qualification."}
    core = None
    browser_id = None
    start = time.monotonic()

    def request(body):
        response = core.request(body, timeout=20)
        if not response.get("ok"):
            raise RuntimeError("request refused: " + str(body.get("type")))
        return response

    try:
        core = Core(str(args.core.resolve()), flags, output / "core.stderr.log")
        page = "<body style='margin:0;background:rgb(40,90,180)'><script>let n=0;function tick(){document.body.style.backgroundColor='rgb('+((++n)%200)+',90,180)';requestAnimationFrame(tick)}tick()</script>"
        reply = request({"type": "browser-add", "payload": {
            "url": "data:text/html," + quote(page, safe=""), "width": 1920, "height": 1080, "fps": 60}})
        rows = reply.get("browserSources")
        if not isinstance(rows, list) or len(rows) != 1 or not isinstance(rows[0].get("id"), str):
            raise RuntimeError("one owned browser source was not created")
        browser_id = rows[0]["id"]
        source_id = "capture:" + browser_id
        report["browserId"] = browser_id
        commands = [{"type": "set-verbose-diagnostics", "enabled": True},
                    {"type": "set-output-profile", "width": 1920, "height": 1080, "fps": 60},
                    {"type": "load-scene-graph", "sceneId": "browser-preparation-proof", "routes": [{
                        "routeId": "browser", "mode": "capture-input", "captureDeviceId": browser_id,
                        "fitMode": "fill", "opacity": 1.0, "zIndex": 0,
                        "rect": {"x": 0, "y": 0, "width": 1, "height": 1}}]}]
        admission = SourceAdmissionJudge([source_id], args.cpu_source_preparation == "1")
        ingress = NativeCaptureInputJudge(source_id)
        first = last = None
        previous_host = None
        host_errors = set()
        samples_count = 0
        measured_start = None
        with (output / "samples.jsonl").open("w") as samples:
            while measured_start is None or time.monotonic() - measured_start < args.duration:
                now = time.monotonic()
                response = core.sync(commands, int((now - start) * 1000))
                commands = []
                if not response.get("ok"):
                    raise RuntimeError("snapshot refused")
                current = response["snapshot"]
                measured = now - start >= args.warmup
                samples.write(json.dumps({"elapsed": now - start, "measured": measured,
                    "snapshot": {key: current.get(key) for key in ["browserSources", "sources", "programSourceAdmission", "programBuffer", "realtimeEvidence", "compositor"]}}) + "\n")
                samples.flush()
                if measured:
                    if first is None:
                        first = current
                        measured_start = now
                    last = current
                    admission.observe(current)
                    ingress.observe(current)
                    samples_count += 1
                    hosts = current.get("browserSources")
                    host = next((row for row in hosts if isinstance(row, dict) and row.get("id") == browser_id), None) if isinstance(hosts, list) else None
                    count = host.get("framesReceived") if host else None
                    if not host or host.get("running") is not True or host.get("health") != "live" or host.get("width") != 1920 or host.get("height") != 1080 or host.get("fps") != 60:
                        host_errors.add("browser-host-not-live-reference-format")
                    if type(count) not in (int, float) or not math.isfinite(count) or count < 1 or count != math.floor(count):
                        host_errors.add("browser-host-counter-unknown")
                    else:
                        if previous_host is not None and count <= previous_host:
                            host_errors.add("browser-host-did-not-advance")
                        previous_host = count
                time.sleep(.25)
        report["nativeBuffer"] = judge([first, last], True)
        report["sourceAdmission"] = admission.result()
        report["captureInput"] = ingress.result()
        if samples_count < 2:
            host_errors.add("insufficient-host-observations")
        report["hostInput"] = {"verdict": "FAIL" if host_errors else "PASS", "errors": sorted(host_errors), "observations": samples_count}
        report["result"] = "PASS" if report["nativeBuffer"]["programBufferVerdict"] == "PASS" and report["captureInput"]["captureInputVerdict"] == "PASS" and report["sourceAdmission"]["sampledSourceAdmissionVerdict"] != "FAIL" and not host_errors else "FAIL"
    except Exception as error:
        report["result"] = "INVALID"
        report["error"] = str(error)
    finally:
        if core:
            try:
                if browser_id:
                    request({"type": "browser-remove", "payload": {"browserId": browser_id}})
            except Exception as error:
                report["cleanupError"] = str(error)
                report["result"] = "INVALID"
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
