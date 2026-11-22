#!/usr/bin/env python3
"""The core's graphics fonts when the core runs OUTSIDE the app bundle.

On a pkg install the shell runs the core from
/Library/Application Support/CoreVideo Pro/corevideo-native: a byte-identical
copy with nothing beside it (make-macos-pkg.sh stages only the binary). This
copies a built core into an empty temporary folder, which is that situation,
and drives it over stdio:

  * with COREVIDEO_GRAPHICS_FONTS_DIR naming the fonts (what the shell now sets,
    CoreLaunchEnvironment), a Segoe UI graphic prepares, draws on Program and
    warns "using Selawik";
  * without it, the copy cannot find Selawik and the strict Program graphic fails
    with the Windows wording: the defect the variable fixes. If this half ever
    passes, the test no longer reproduces the install layout and proves nothing.

Usage: test_core_graphics_fonts_location.py [build-dir]  (default native/build-metal)
Exit 0 on pass, 1 on failure, 2 when no Metal core is built.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


class Core:
    def __init__(self, path, env):
        self.proc = subprocess.Popen([path], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True, env=env)
        self.sequence = 0

    def request(self, body, timeout=10.0):
        self.sequence += 1
        body["id"] = f"fonts-{self.sequence}"
        self.proc.stdin.write(json.dumps(body) + "\n")
        self.proc.stdin.flush()
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                return {}
            try:
                reply = json.loads(line)
            except json.JSONDecodeError:
                continue
            if reply.get("id") == body["id"]:
                return reply
        return {}

    def snapshot(self):
        return self.request({"type": "media-core-sync", "commands": []}).get("snapshot", {})

    def close(self):
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def segoe_graphic(core_path, fonts_dir):
    env = {k: v for k, v in os.environ.items() if k != "COREVIDEO_GRAPHICS_FONTS_DIR"}
    if fonts_dir:
        env["COREVIDEO_GRAPHICS_FONTS_DIR"] = fonts_dir
    core = Core(core_path, env)
    try:
        epoch = core.snapshot()["graphics"]["coreEpoch"]

        def command(kind, payload):
            payload.update({"schemaVersion": 1, "requestId": f"op-{core.sequence}", "clientId": "fonts-location",
                            "coreEpoch": epoch, "instanceId": "fonts"})
            return core.request({"type": kind, "graphics": payload})

        command("graphics-instance-create", {"templateId": "compact-solid", "templateVersion": 1, "scope": "global",
                                             "fields": {"name": "Installed core"},
                                             "style": {"fontFamily": "Segoe UI"}})
        command("graphics-instance-trigger", {"expectedRevision": 1, "action": "show"})
        row = {}
        deadline = time.time() + 10
        while time.time() < deadline:
            rows = core.snapshot().get("graphics", {}).get("instances", [])
            if rows:
                row = rows[0]
                if row.get("firstDrawSubmittedFrame", 0) > 0 or row.get("preparation") == "failed":
                    break
            time.sleep(0.02)
        return row
    finally:
        core.close()


def main():
    build = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "native", "build-metal")
    built_core = os.path.join(build, "corevideo-native")
    fonts = os.path.join(build, "GraphicsFonts")
    if not os.access(built_core, os.X_OK) or not os.path.isdir(fonts):
        print(f"SKIP: no Metal core and GraphicsFonts in {build}")
        return 2
    failures = []
    with tempfile.TemporaryDirectory(prefix="cvp-installed-core-") as folder:
        core = os.path.join(folder, "corevideo-native")  # nothing beside it, like /Library
        shutil.copy2(built_core, core)

        with_env = segoe_graphic(core, fonts)
        print("with COREVIDEO_GRAPHICS_FONTS_DIR:", with_env.get("preparation"), "|",
              with_env.get("firstDrawSubmittedFrame"), "|", with_env.get("warning"))
        if with_env.get("preparation") != "ready" or not with_env.get("firstDrawSubmittedFrame"):
            failures.append("with the variable, the Segoe UI graphic did not reach Program")
        if with_env.get("warning") != "Font Segoe UI is not installed on this Mac; using Selawik.":
            failures.append("with the variable, the substitution warning is missing")

        without = segoe_graphic(core, None)
        print("without it:", without.get("preparation"), "|", without.get("warning"))
        if without.get("preparation") != "failed" or "Requested graphics font is unavailable" not in (
                without.get("warning") or ""):
            failures.append("without the variable the copied core still found Selawik: "
                            "this test no longer reproduces the install layout")
    for failure in failures:
        print("FAIL:", failure)
    print("PASS" if not failures else "FAILED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
