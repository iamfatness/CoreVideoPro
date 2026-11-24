#!/usr/bin/env python3
"""Direct-engine probe for macOS Zoom talkback ("Comms").

Drives the bundled macOS corevideo-zoom-engine over Pro's token-spliced IPC,
the same way scripts/macos-engine-live-test.py does, so the native comms glue
(engine-comms-macos.mm) can be proven without the shell or the core.

Modes:
  idle     No init, no join. Passes only if the engine:
             1. reports comms_state with available:false and a reason,
             2. refuses comms_key outside a meeting (log has "refused:"),
             3. treats a comms_mic line naming "join quit" as comms only
                (no joined / auth_* line, process still alive),
             4. exits 0 on quit within 5 s.
  meeting  The live spike (plan Task 5). Needs the owner and a co-host in a
           real Zoom meeting; see docs/qa/mac-comms-live-runbook.md. Inits
           with the public app key, joins, prints every comms_state's
           available/reason/lamps plus comms_audio_join, comms_send_hop and
           meeting_status lines, waits for available:true (the owner makes the
           engine co-host), then: tone on, key slot 0 with a 1500 ms lease
           renewed every 500 ms for --minutes, key off, tone off; optional
           Private comms on/off; then leaves and quits. Reports framesSent,
           channelSends, underruns and sendErrors at start and end.

Usage:
  python3 scripts/qa/macos-engine-comms-probe.py --engine <bin> idle
  python3 scripts/qa/macos-engine-comms-probe.py --engine <bin> meeting \\
      <meeting-id> [passcode] --minutes 10 --send-mode direct|main \\
      [--slot 0] [--private] [--log out.jsonl]

The engine never joins a meeting in idle mode. In meeting mode it joins ONLY
the meeting id given on the command line; run it only with the owner present.
"""
import argparse
import json
import os
import socket
import subprocess
import sys
import time

PUBLIC_APP_KEY = "y6sIWSwiTZe1JygMx4C9EQ"


class Engine:
    def __init__(self, binary, env_extra=None, log_path=None):
        self.token = f"commsprobe{int(time.time()) % 100000}"
        p2e = f"/tmp/ZoomObsPlugin_{self.token}_P2E.sock"
        e2p = f"/tmp/ZoomObsPlugin_{self.token}_E2P.sock"
        env = dict(os.environ)
        env.update(env_extra or {})
        self.proc = subprocess.Popen([binary, "--ipc-token", self.token], env=env,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.p2e = self._connect(p2e)
        self.e2p = self._connect(e2p)
        self.e2p.settimeout(0.2)
        self.buf = b""
        self.log = open(log_path, "a") if log_path else None

    def _connect(self, path, tries=100):
        for _ in range(tries):
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                s.connect(path)
                return s
            except OSError:
                s.close()
                if self.proc.poll() is not None:
                    raise SystemExit(f"FAIL: engine exited rc={self.proc.returncode} before {path}")
                time.sleep(0.1)
        raise SystemExit(f"FAIL: could not connect {path}")

    def send(self, obj):
        # Compact separators are REQUIRED: the engine's scanner matches
        # "key":"value" byte-for-byte.
        self.p2e.sendall((json.dumps(obj, separators=(",", ":")) + "\n").encode())

    def events(self, seconds):
        """Yield parsed E2P events for `seconds`."""
        end = time.time() + seconds
        while time.time() < end:
            try:
                data = self.e2p.recv(65536)
                if not data:
                    return
                self.buf += data
            except socket.timeout:
                pass
            except OSError:
                return
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                if not line.strip():
                    continue
                if self.log:
                    self.log.write(line.decode(errors="replace") + "\n")
                    self.log.flush()
                try:
                    yield json.loads(line)
                except json.JSONDecodeError:
                    continue

    def wait_exit(self, seconds):
        try:
            return self.proc.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            return None

    def kill(self):
        if self.proc.poll() is None:
            self.proc.kill()


def ok(passed, label, detail=""):
    print(f"{'PASS' if passed else 'FAIL'} {label}" + (f": {detail}" if detail else ""))
    return passed


def run_idle(args):
    eng = Engine(args.engine)
    results = []
    try:
        # 1. available:false with a reason, before any init.
        first = None
        for e in eng.events(5):
            if e.get("cmd") == "comms_state":
                first = e
                break
        results.append(ok(first is not None and first.get("available") is False and bool(first.get("reason")),
                          "idle comms_state available:false with a reason",
                          json.dumps({"available": first.get("available"), "reason": first.get("reason")})
                          if first else "no comms_state within 5 s"))

        # 2. A key outside a meeting is refused and the refusal is logged.
        eng.send({"cmd": "comms_key", "slot": 0, "on": True, "lease_ms": 1500})
        refused = None
        for e in eng.events(1.0):
            if e.get("cmd") == "comms_state" and any("refused:" in l for l in e.get("log", [])):
                refused = [l for l in e["log"] if "refused:" in l][-1]
                break
        results.append(ok(refused is not None, "comms_key outside a meeting is refused", refused or "no refusal"))

        # 3. A comms line naming engine verbs stays a comms line.
        eng.send({"cmd": "comms_mic", "device": "join quit"})
        bad = None
        for e in eng.events(2.0):
            if e.get("cmd") in ("joined", "auth_ok", "auth_fail", "auth_success", "left"):
                bad = e
                break
        alive = eng.proc.poll() is None
        results.append(ok(bad is None and alive, "comms_mic \"join quit\" reaches comms only",
                          f"engine alive={alive} stray={bad}"))

        # 4. Quit exits 0 within 5 s.
        eng.send({"cmd": "quit"})
        rc = eng.wait_exit(5)
        results.append(ok(rc == 0, "quit exits 0 within 5 s", f"rc={rc}"))
    finally:
        eng.kill()
    return 0 if all(results) else 1


def summarize(state):
    s = state.get("stats", {}) or {}
    return {k: s.get(k) for k in ("frames_sent", "channel_sends", "underruns", "send_errors")}


def run_meeting(args):
    env = {"COREVIDEO_COMMS_SEND_MODE": args.send_mode}
    eng = Engine(args.engine, env, args.log)
    last_state = {}
    first_up_stats = None
    seen_available = False
    printed = set()

    def pump(seconds):
        nonlocal last_state, seen_available
        for e in eng.events(seconds):
            cmd = e.get("cmd")
            if cmd == "comms_state":
                last_state = e
                key = (e.get("available"), e.get("reason"), json.dumps(e.get("lamps"), sort_keys=True))
                if key not in printed:
                    printed.add(key)
                    print(f"[comms_state] available={e.get('available')} reason={e.get('reason')!r} "
                          f"lamps={e.get('lamps')} private={e.get('private')}")
                if e.get("available"):
                    seen_available = True
            elif cmd == "debug" and e.get("stage") in (
                    "meeting_status", "comms_audio_join", "comms_send_hop", "comms_silent_mic",
                    "comms_silent_mic_removed", "comms_mic_revoked", "comms_tick_exception",
                    "comms_audio_shutdown_timeout"):
                print(f"[{e.get('stage')}] {json.dumps(e)}")
            elif cmd in ("joined", "left", "error", "auth_fail"):
                print(f"[{cmd}] {json.dumps(e)}")
        if eng.proc.poll() is not None:
            raise SystemExit(f"FAIL: engine exited rc={eng.proc.returncode}")

    try:
        eng.send({"cmd": "init", "public_app_key": PUBLIC_APP_KEY})
        pump(20)
        join = {"cmd": "join", "meeting_id": args.meeting_id, "display_name": "CoreVideo Pro comms probe"}
        if args.passcode:
            join["passcode"] = args.passcode
        eng.send(join)
        print("waiting for talkback (make 'CoreVideo Pro comms probe' co-host in Zoom)...")
        deadline = time.time() + args.wait_minutes * 60
        while not seen_available and time.time() < deadline:
            pump(1)
        if not seen_available:
            print("FAIL: talkback never came up; last reason:", last_state.get("reason"))
            return 1
        # Let auto-assign settle so the guest lands on the slot.
        pump(10)
        first_up_stats = summarize(last_state)
        print("start stats:", first_up_stats)
        eng.send({"cmd": "comms_tone", "on": True})
        end = time.time() + args.minutes * 60
        while time.time() < end:
            eng.send({"cmd": "comms_key", "slot": args.slot, "on": True, "lease_ms": 1500})
            pump(0.5)
        eng.send({"cmd": "comms_key", "slot": args.slot, "on": False})
        eng.send({"cmd": "comms_tone", "on": False})
        pump(2)
        print("end stats:", summarize(last_state))
        if args.private:
            eng.send({"cmd": "comms_private", "on": True})
            pump(8)
            print("private on log tail:", last_state.get("log", [])[-5:])
            eng.send({"cmd": "comms_private", "on": False})
            pump(4)
            print("private off log tail:", last_state.get("log", [])[-5:],
                  "meeting_mic lamp:", (last_state.get("lamps") or {}).get("meeting_mic"))
        print("log tail:", last_state.get("log", [])[-10:])
    finally:
        try:
            eng.send({"cmd": "leave"})
            time.sleep(2)
            eng.send({"cmd": "quit"})
        except OSError:
            pass
        rc = eng.wait_exit(8)
        print(f"engine exit rc={rc}")
        eng.kill()
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--engine", required=True, help="bundle binary: .../corevideo-zoom-engine.app/Contents/MacOS/corevideo-zoom-engine")
    sub = ap.add_subparsers(dest="mode", required=True)
    sub.add_parser("idle")
    m = sub.add_parser("meeting")
    m.add_argument("meeting_id")
    m.add_argument("passcode", nargs="?", default="")
    m.add_argument("--minutes", type=float, default=10)
    m.add_argument("--wait-minutes", type=float, default=10)
    m.add_argument("--send-mode", choices=["direct", "main"], default="direct")
    m.add_argument("--slot", type=int, default=0)
    m.add_argument("--private", action="store_true")
    m.add_argument("--log", help="append every E2P line here (JSONL)")
    args = ap.parse_args()
    sys.exit(run_idle(args) if args.mode == "idle" else run_meeting(args))


if __name__ == "__main__":
    main()
