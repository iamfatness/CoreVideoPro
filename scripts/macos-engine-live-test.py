#!/usr/bin/env python3
"""Live-meeting acceptance test for the macOS corevideo-zoom-engine.

Drives the bundled engine over Pro's token-spliced IPC exactly like the core
would: init (public app key) -> join -> start_media -> subscribe video+audio
for the active speaker, then independently verifies the SHM wire format by
reading the frame region and the 128-slot audio ring directly (FNV-1a hashed
/ZOP names, same math as shm_platform_name).

Usage:
  python3 scripts/macos-engine-live-test.py <engine-binary> <meeting-id> [passcode]

The Zoom SDK meeting UI will appear (the engine is a Regular-activation app).
If raw recording permission is prompted in the host's Zoom client, click Allow.
Ctrl-C leaves the meeting and quits the engine cleanly.
"""
import json
import signal
import socket
import struct
import subprocess
import sys
import time
from multiprocessing import shared_memory

if len(sys.argv) < 3:
    print(__doc__)
    sys.exit(2)

ENGINE = sys.argv[1]
MEETING_ID = sys.argv[2]
PASSCODE = sys.argv[3] if len(sys.argv) > 3 else ""
PUBLIC_APP_KEY = "y6sIWSwiTZe1JygMx4C9EQ"
TOKEN = f"livetest{int(time.time()) % 100000}"
PREFIX = f"ZoomObsPlugin_{TOKEN}_"
P2E = f"/tmp/ZoomObsPlugin_{TOKEN}_P2E.sock"
E2P = f"/tmp/ZoomObsPlugin_{TOKEN}_E2P.sock"


def shm_platform_name(logical: str) -> str:
    """Python twin of engine-ipc.h shm_platform_name (Apple branch)."""
    h = 1469598103934665603
    for b in logical.encode():
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return f"ZOP{h:016x}"  # SharedMemory prepends the leading '/'


proc = subprocess.Popen([ENGINE, "--ipc-token", TOKEN],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def connect(path, tries=50):
    for _ in range(tries):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            s.connect(path)
            return s
        except OSError:
            s.close()
            time.sleep(0.1)
    raise SystemExit(f"FAIL: could not connect {path}")


p2e = connect(P2E)
e2p = connect(E2P)
e2p.settimeout(0.5)
buf = b""


def send(obj):
    # Compact separators are REQUIRED: the engine's minimal JSON scanner
    # matches `"key":"value"` byte-for-byte and a space after the colon
    # makes every field invisible (the core's writer is compact too).
    p2e.sendall((json.dumps(obj, separators=(",", ":")) + "\n").encode())


def recv_events():
    global buf
    out = []
    try:
        data = e2p.recv(65536)
        if data:
            buf += data
    except socket.timeout:
        pass
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        if line.strip():
            try:
                out.append(json.loads(line))
            except json.JSONDecodeError:
                print(f"  [unparsed] {line[:120]!r}")
    return out


state = {
    "authed": False, "joined": False, "raw_ready": False, "last_start": 0.0,
    "subscribed_pid": None, "video_uuid": None, "audio_uuid": None,
    "video_shm": None, "audio_shm": None,
    "last_seq": 0, "last_seq_t": 0.0, "last_ctr": 0, "last_ctr_t": 0.0,
}

stopping = False


def cleanup(*_):
    global stopping
    if stopping:
        return
    stopping = True
    print("\nleaving meeting + quitting engine...")
    try:
        send({"cmd": "stop_media"})
        time.sleep(0.5)
        send({"cmd": "leave"})
        time.sleep(1.5)
        send({"cmd": "quit"})
        time.sleep(1.0)
    except OSError:
        pass
    if proc.poll() is None:
        proc.terminate()
    sys.exit(0)


signal.signal(signal.SIGINT, cleanup)
signal.signal(signal.SIGTERM, cleanup)

print(f"engine pid={proc.pid} token={TOKEN}")
send({"cmd": "init", "public_app_key": PUBLIC_APP_KEY})

start = time.time()
last_stats = 0.0
while True:
    if proc.poll() is not None:
        print(f"engine exited rc={proc.returncode}")
        break
    for e in recv_events():
        cmd = e.get("cmd")
        if cmd == "ping":
            continue
        if cmd == "debug":
            interesting = {"raw_media_ready", "raw_media_resubscribe", "after_sdk_auth",
                           "can_start_raw_recording", "local_recording_privilege_status",
                           "record_privilege_change", "request_recording_privilege",
                           "video_frame_received", "audio_frame_received",
                           "video_subscribe_ok", "meeting_status"}
            if e.get("stage") in interesting or "fail" in str(e.get("stage", "")):
                print(f"  [debug] {e}")
            continue
        print(f"  [{cmd}] { {k: v for k, v in e.items() if k != 'cmd'} }")

        if cmd == "auth_ok":
            state["authed"] = True
            print(f"-> joining meeting {MEETING_ID} ...")
            send({"cmd": "join", "meeting_id": MEETING_ID, "passcode": PASSCODE,
                  "display_name": "CoreVideo Pro (mac test)"})
        elif cmd == "auth_fail":
            cleanup()
        elif cmd == "joined":
            # Fires again after a mid-meeting reconnect; always (re)start raw
            # media — handle_start_media re-attaches the record delegate to the
            # CURRENT meeting session's record controller each call.
            state["joined"] = True
            print("-> joined; starting raw media (host may need to Allow recording)")
            send({"cmd": "start_media"})
            state["last_start"] = time.time()
        elif cmd == "raw_media_status":
            state["raw_ready"] = bool(e.get("active"))
        elif cmd == "participants" and state["joined"] and state["subscribed_pid"] is None:
            parts = e.get("participants", [])
            active = e.get("active_speaker_id") or 0
            pick = None
            for p in parts:
                if p.get("id") == active and p.get("has_video"):
                    pick = p
                    break
            if pick is None:
                pick = next((p for p in parts if p.get("has_video")), None)
            if pick:
                pid = pick["id"]
                state["subscribed_pid"] = pid
                state["video_uuid"] = f"livetest_{pid}-active-speaker"
                state["audio_uuid"] = state["video_uuid"]  # mix mirrors onto the video target
                print(f"-> subscribing video+mix-audio for {pick.get('name')!r} (id {pid})")
                send({"cmd": "subscribe", "source_uuid": state["video_uuid"],
                      "participant_id": pid, "resolution": 2})
        elif cmd == "frame" and state["video_shm"] is None and state["video_uuid"]:
            try:
                name = shm_platform_name(PREFIX + state["video_uuid"])
                state["video_shm"] = shared_memory.SharedMemory(name=name)
                print(f"-> video SHM region open ({name})")
            except FileNotFoundError:
                pass
        elif cmd == "audio" and state["audio_shm"] is None and state["audio_uuid"]:
            try:
                name = shm_platform_name(PREFIX + state["audio_uuid"] + "_audio")
                state["audio_shm"] = shared_memory.SharedMemory(name=name)
                magic, = struct.unpack_from("<I", state["audio_shm"].buf, 0)
                ok = "OK" if magic == 0x43564152 else f"BAD MAGIC {magic:#x}"
                print(f"-> audio ring open ({name}) magic={ok}")
            except FileNotFoundError:
                pass

    now = time.time()
    # Keep asking until raw media is live: the grant can land at any time
    # (host clicks Allow), and each attempt re-attaches the record delegate.
    if state["joined"] and not state["raw_ready"] and now - state["last_start"] >= 5.0:
        send({"cmd": "start_media"})
        state["last_start"] = now
    if now - last_stats >= 2.0:
        last_stats = now
        lines = []
        if state["video_shm"]:
            seq, w, h, ylen = struct.unpack_from("<IIII", state["video_shm"].buf, 0)
            fps = ((seq - state["last_seq"]) / 2.0) / 2.0  # seq +2 per frame
            state["last_seq"] = seq
            lines.append(f"video {w}x{h} ~{fps:.1f} fps (seq={seq})")
        if state["audio_shm"]:
            _, ctr, slots, payload = struct.unpack_from("<IIII", state["audio_shm"].buf, 0)
            pkts = (ctr - state["last_ctr"]) / 2.0
            state["last_ctr"] = ctr
            stride = 16 + payload
            off = 16 + ((ctr - 1) % slots) * stride if ctr else 16
            _, blen, rate, ch, _ = struct.unpack_from("<IIIHH", state["audio_shm"].buf, off)
            lines.append(f"audio ~{pkts:.0f} pkt/s ({rate} Hz {ch}ch {blen}B) ring={ctr}")
        if lines:
            print("  STATS: " + " | ".join(lines))
    time.sleep(0.1)

cleanup()
