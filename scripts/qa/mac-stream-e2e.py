#!/usr/bin/env python3
"""End-to-end proof for macOS streaming (mac streaming Phase 2, F1 + P1).

Runs a headless core (by default the one inside an assembled app bundle, so
the BUNDLED ffmpeg and child guard are what it finds), streams Program from the
fake Zoom engine to LOCAL receivers only (never a real platform), then:

  1. proves the sender used the bundled ffmpeg under the child guard
     (the senders' runtimeDetail + the live process table);
  2. decodes what each receiver wrote and checks the pictures are real
     (frames decoded, mean luma not flat, not black);
  3. SIGKILLs the core mid-stream and proves no ffmpeg / child guard survives.

  python3 scripts/qa/mac-stream-e2e.py --app "<path>/CoreVideo Pro.app" \
      --rtmp-receiver /opt/homebrew/bin/ffmpeg --srt-receiver /opt/homebrew/bin/srt-live-transmit \
      [--fake-engine native/build-metal/corevideo-zoom-engine-fake] [--seconds 20] [--out DIR]

The core runs with PATH=/usr/bin:/bin:/usr/sbin:/sbin and no ffmpeg
environment override. The resolver's Homebrew fallback would still find
/opt/homebrew/bin/ffmpeg if the bundled copy were missing, so the proof that
the bundled copy ran is the executable path in runtimeDetail and in `ps`, never
the mere fact that streaming worked.
"""
import argparse
import json
import os
import signal
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


class Core:
    def __init__(self, path, env):
        self.proc = subprocess.Popen([path], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, text=True, bufsize=1, env=env)
        self.responses, self.events, self.stderr, self.next_id = {}, [], [], 1
        threading.Thread(target=self._out, daemon=True).start()
        threading.Thread(target=self._err, daemon=True).start()

    def _out(self):
        for line in self.proc.stdout:
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue
            if obj.get("id"):
                self.responses[obj["id"]] = obj
            elif obj.get("type"):
                self.events.append(obj)

    def _err(self):
        for line in self.proc.stderr:
            self.stderr.append(line.rstrip())

    def request(self, body, timeout=10.0):
        rid = f"e2e-{self.next_id}"
        self.next_id += 1
        self.proc.stdin.write(json.dumps(dict(body, id=rid)) + "\n")
        self.proc.stdin.flush()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if rid in self.responses:
                return self.responses.pop(rid)
            time.sleep(0.02)
        return None

    def sync(self, commands, elapsed_ms):
        return self.request({"type": "media-core-sync", "elapsedMs": elapsed_ms, "commands": commands})


def find_key(node, key):
    """First value named `key` anywhere in a JSON tree (the snapshot nests differently per path)."""
    if isinstance(node, dict):
        if key in node:
            return node[key]
        for value in node.values():
            found = find_key(value, key)
            if found is not None:
                return found
    elif isinstance(node, list):
        for value in node:
            found = find_key(value, key)
            if found is not None:
                return found
    return None


def processes_matching(fragment):
    out = subprocess.run(["ps", "-axo", "pid=,ppid=,pgid=,command="], capture_output=True, text=True).stdout
    rows = []
    for line in out.splitlines():
        parts = line.split(None, 3)
        if len(parts) == 4 and fragment in parts[3]:
            rows.append({"pid": int(parts[0]), "ppid": int(parts[1]), "pgid": int(parts[2]), "command": parts[3]})
    return rows


def luma_series(ffmpeg, path):
    """Per-frame mean luma (signalstats YAVG) of a received file, decoded by `ffmpeg`."""
    proc = subprocess.run([ffmpeg, "-hide_banner", "-nostats", "-i", path, "-vf",
                           "signalstats,metadata=print:key=lavfi.signalstats.YAVG", "-f", "null", "-"],
                          capture_output=True, text=True, timeout=120)
    values = []
    for line in (proc.stderr + proc.stdout).splitlines():
        if "lavfi.signalstats.YAVG=" in line:
            try:
                values.append(float(line.split("=", 1)[1]))
            except ValueError:
                pass
    return values


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", required=True)
    parser.add_argument("--rtmp-receiver", default="/opt/homebrew/bin/ffmpeg")
    parser.add_argument("--srt-receiver", default="")
    parser.add_argument("--fake-engine", default=os.path.join(ROOT, "native/build-metal/corevideo-zoom-engine-fake"))
    parser.add_argument("--seconds", type=float, default=20.0)
    parser.add_argument("--out", default="")
    parser.add_argument("--lone-core", action="store_true",
                        help="run a COPY of the core outside the bundle (the pkg installs it in "
                             "/Library/Application Support) with the shell's environment")
    parser.add_argument("--no-shell-env", action="store_true",
                        help="with --lone-core: omit the shell's guard/ffmpeg variables (the "
                             "pre-fix behaviour; expected to FAIL)")
    args = parser.parse_args()

    resources = os.path.join(args.app, "Contents", "Resources")
    core_path = os.path.join(resources, "corevideo-native")
    bundled_ffmpeg = os.path.join(resources, "ffmpeg")
    guard = os.path.join(resources, "corevideo-child-guard")
    for path in (core_path, bundled_ffmpeg, guard, args.fake_engine, args.rtmp_receiver):
        if not os.path.exists(path):
            print(f"FAIL missing {path}")
            return 1
    out = args.out or tempfile.mkdtemp(prefix="cvp-stream-e2e-")
    os.makedirs(out, exist_ok=True)
    if args.lone_core:
        # Nothing sits beside this copy: exactly the pkg's
        # /Library/Application Support/CoreVideo Pro/corevideo-native.
        lone_dir = os.path.join(out, "lone-core")
        os.makedirs(lone_dir, exist_ok=True)
        lone = os.path.join(lone_dir, "corevideo-native")
        subprocess.run(["cp", "-p", core_path, lone], check=True)
        core_path = lone
    failures, notes = [], []

    rtmp_file = os.path.join(out, "received-rtmp.flv")
    srt_file = os.path.join(out, "received-srt.ts")
    receivers = []
    receive_seconds = str(int(args.seconds + 15))
    receivers.append(("rtmp", rtmp_file, subprocess.Popen(
        [args.rtmp_receiver, "-hide_banner", "-loglevel", "warning", "-y", "-listen", "1",
         "-i", "rtmp://127.0.0.1:19350/live/e2e", "-c", "copy", "-t", receive_seconds, rtmp_file],
        stdout=subprocess.DEVNULL, stderr=open(os.path.join(out, "receiver-rtmp.log"), "w"))))
    if args.srt_receiver and os.path.basename(args.srt_receiver) == "srt-live-transmit":
        # libsrt's own tool (Homebrew `srt`): an SRT receiver that is not ffmpeg at all.
        receivers.append(("srt", srt_file, subprocess.Popen(
            [args.srt_receiver, "-q", "srt://127.0.0.1:19360?mode=listener", "file://con"],
            stdout=open(srt_file, "wb"), stderr=open(os.path.join(out, "receiver-srt.log"), "w"))))
    elif args.srt_receiver:
        receivers.append(("srt", srt_file, subprocess.Popen(
            [args.srt_receiver, "-hide_banner", "-loglevel", "warning", "-y",
             "-i", "srt://127.0.0.1:19360?mode=listener", "-c", "copy", "-t", receive_seconds, srt_file],
            stdout=subprocess.DEVNULL, stderr=open(os.path.join(out, "receiver-srt.log"), "w"))))
    time.sleep(1.0)

    env = {
        "PATH": "/usr/bin:/bin:/usr/sbin:/sbin",   # a Finder/Dock launch's PATH: no Homebrew
        "HOME": os.environ.get("HOME", ""),
        "TMPDIR": os.environ.get("TMPDIR", "/tmp"),
        "COREVIDEO_ZOOM_ENGINE_PATH": args.fake_engine,
        "COREVIDEO_FAKE_ENGINE_PARTICIPANTS": "2",
        "COREVIDEO_FAKE_ENGINE_RES": "2",
        "COREVIDEO_FAKE_ENGINE_FPS": "30",
        "COREVIDEO_FAKE_NO_CHURN": "1",
    }
    if args.lone_core and not args.no_shell_env:
        # What AppModel's launch block adds (ShellPaths.streamingEnvironment).
        env["COREVIDEO_CHILD_GUARD_PATH"] = guard
        env["COREVIDEO_FFMPEG_BIN_DIR"] = resources
    core = Core(core_path, env)
    time.sleep(1.5)
    joined = core.request({"type": "zoom-join", "payload": {"meetingNumber": "1234567890",
                                                            "displayName": "stream-e2e"}}, timeout=20)
    if joined is None:
        failures.append("zoom-join timed out")
    time.sleep(3.0)
    route = {"routeId": "pgm-0", "mode": "fixed", "participantId": "101", "audioRole": "mix",
             "fitMode": "fill", "opacity": 1.0, "zIndex": 0, "rect": {"x": 0, "y": 0, "width": 1, "height": 1}}
    core.sync([{"type": "load-scene-graph", "sceneId": "pgm", "routes": [route]}], 0)
    destinations = ["rtmp"] + (["srt"] if args.srt_receiver else [])
    settings = [{"id": "rtmp", "label": "Stream", "protocol": "rtmp", "url": "rtmp://127.0.0.1:19350/live",
                 "streamKey": "e2e", "fps": 30, "targetBitrateMbps": 6.0, "audioBitrateKbps": 160,
                 "videoCodec": "h264", "encoderMode": "auto", "keyframeIntervalSeconds": 2.0,
                 "rateControl": "cbr", "h264Profile": "high", "bFrames": 2, "allowEnhancedRtmp": False}]
    if args.srt_receiver:
        settings.append({"id": "srt", "label": "SRT", "mode": "caller", "host": "127.0.0.1", "port": 19360,
                         "latencyMs": 120, "latencyUs": 120000, "passphrase": "", "keyLength": 0,
                         "fps": 30, "targetBitrateMbps": 6.0, "videoCodec": "h264", "encoderMode": "auto"})
    started = core.sync([{"type": "start-program-output", "destinations": destinations,
                          "streamOutputProfile": {"width": 1920, "height": 1080, "fps": 30,
                                                  "targetBitrateMbps": 6.0, "codec": "h264"},
                          "destinationSettings": settings}], 1000)
    if started is None:
        failures.append("start-program-output timed out")

    # Stream; sample the senders' truthful state while it runs.
    t0 = time.monotonic()
    last_senders = []
    while time.monotonic() - t0 < args.seconds:
        snap = core.sync([], 2000 + (time.monotonic() - t0) * 1000)
        session = find_key(snap, "outputSenderSession") if snap else None
        if session:
            last_senders = session.get("senders", [])
        time.sleep(1.0)
    for sender in last_senders:
        if sender.get("destination") not in destinations:
            continue
        lifecycle = (sender.get("lifecycle") or {}).get("state")
        sup = sender.get("supervisor") or {}
        line = (f"{sender['destination']}: lifecycle={lifecycle} healthy={sup.get('healthy')} "
                f"framesSent={sender.get('framesSent')} status={sender.get('status')}")
        notes.append(line)
        if lifecycle != "producing" or not sup.get("healthy"):
            failures.append(f"sender not producing+healthy after {args.seconds:.0f}s: {line}")

    # Which ffmpeg ran: the sender publishes its resolved path as runtimeDetail at
    # spawn (the ffmpeg-process-start event goes to the send-proof file, not stdout).
    for sender in last_senders:
        if sender.get("destination") not in destinations:
            continue
        detail = sender.get("runtimeDetail", "")
        notes.append(f"{sender['destination']}: runtimeDetail={detail} childGuard={sender.get('childGuard')}")
        if sender.get("childGuard") is not True:
            failures.append(f"{sender['destination']} does not report childGuard=true")
        # "ffmpeg:<path>" after spawn, "available:<path>" once the runtime probe republishes it.
        if os.path.realpath(detail.split(":", 1)[-1]) != os.path.realpath(bundled_ffmpeg):
            failures.append(f"{sender['destination']} did not use the bundled ffmpeg: {detail!r}")

    # The process table: every bundled ffmpeg must be the child of a bundled
    # guard, and every guard a child of the core. (A guard's own command line
    # contains the ffmpeg path, so split the two by argv[0].)
    guards = [p for p in processes_matching(guard) if p["command"].startswith(guard + " ")]
    ffmpegs = [p for p in processes_matching(bundled_ffmpeg) if p["command"].startswith(bundled_ffmpeg + " ")]
    notes.append(f"while streaming: {len(ffmpegs)} bundled ffmpeg, {len(guards)} child guard process(es)")
    guard_pids = {g["pid"] for g in guards}
    for g in guards:
        notes.append(f"  guard  pid={g['pid']} ppid={g['ppid']} pgid={g['pgid']}")
        if g["ppid"] != core.proc.pid:
            failures.append(f"guard {g['pid']} is not the core's child")
    for proc in ffmpegs:
        notes.append(f"  ffmpeg pid={proc['pid']} ppid={proc['ppid']} pgid={proc['pgid']}")
        if proc["ppid"] not in guard_pids:
            failures.append(f"ffmpeg {proc['pid']} is not a child of a child guard (ppid {proc['ppid']})")
        if proc["pgid"] == os.getpgid(core.proc.pid):
            failures.append(f"ffmpeg {proc['pid']} shares the core's process group")
    if len(ffmpegs) < len(destinations):
        failures.append(f"expected {len(destinations)} running bundled ffmpeg, saw {len(ffmpegs)}")

    # P1: SIGKILL the core mid-stream; nothing it started may survive.
    os.kill(core.proc.pid, signal.SIGKILL)
    core.proc.wait()
    killed_at = time.monotonic()
    survivors = []
    while time.monotonic() - killed_at < 5.0:
        survivors = processes_matching(bundled_ffmpeg + " ") + processes_matching(guard) + \
            processes_matching(args.fake_engine)
        if not survivors:
            break
        time.sleep(0.1)
    gone_after = time.monotonic() - killed_at
    if survivors:
        failures.append("survived a SIGKILLed core: " + "; ".join(f"{s['pid']} {s['command'][:80]}" for s in survivors))
        for s in survivors:
            try:
                os.kill(s["pid"], signal.SIGKILL)
            except ProcessLookupError:
                pass
    else:
        notes.append(f"after SIGKILL of the core: no bundled ffmpeg, child guard or engine left ({gone_after:.2f}s)")

    # The receivers finish when their input ends.
    for name, path, proc in receivers:
        try:
            # srt-live-transmit keeps listening after its caller is gone.
            proc.wait(timeout=5 if "srt-live-transmit" in str(proc.args) else 30)
        except subprocess.TimeoutExpired:
            proc.send_signal(signal.SIGINT)
            proc.wait(timeout=10)
    decoder = args.rtmp_receiver
    for name, path, _ in receivers:
        if not os.path.exists(path) or os.path.getsize(path) == 0:
            failures.append(f"{name}: receiver wrote nothing ({path})")
            continue
        probe = subprocess.run([decoder, "-hide_banner", "-i", path], capture_output=True, text=True).stderr
        stream_lines = [l.strip() for l in probe.splitlines() if "Stream #" in l]
        notes.append(f"{name}: {os.path.getsize(path)} bytes; " + " | ".join(stream_lines))
        luma = luma_series(decoder, path)
        if len(luma) < 30:
            failures.append(f"{name}: only {len(luma)} decoded frames")
            continue
        mean, lo, hi = sum(luma) / len(luma), min(luma), max(luma)
        notes.append(f"{name}: {len(luma)} decoded frames, mean luma {mean:.1f}, range {lo:.1f}..{hi:.1f}")
        if hi - lo < 2.0:
            failures.append(f"{name}: luma is flat ({lo:.1f}..{hi:.1f}): not a moving picture")
        if mean < 20.0:
            failures.append(f"{name}: mean luma {mean:.1f}: black")

    for line in notes:
        print("  " + line)
    with open(os.path.join(out, "core-stderr.log"), "w") as f:
        f.write("\n".join(core.stderr))
    if failures:
        for line in failures:
            print("FAIL " + line)
        print(f"artifacts: {out}")
        return 1
    print(f"PASS mac-stream-e2e ({', '.join(destinations)}); artifacts: {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
