/**
 * Quick Sprint 1 auth smoke: native media core + vendored Zoom engine.
 * Uses the embedded public app key from src/config/zoomMeetingSdk.json.
 *
 * Usage (PowerShell):
 *   $env:COREVIDEO_ZOOM_ENGINE_PATH = "...\native\build-dev\corevideo-zoom-engine.exe"
 *   node .\scripts\test-zoom-auth.mjs
 */
import { spawn } from "node:child_process";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = join(here, "..");
const buildDir = join(repoRoot, "native", "build-dev");
const nativeCore = join(buildDir, "corevideo-native.exe");
const embeddedKey = JSON.parse(
  readFileSync(join(repoRoot, "src", "config", "zoomMeetingSdk.json"), "utf8")
).publicAppKey;

if (!process.env.COREVIDEO_ZOOM_ENGINE_PATH) {
  process.env.COREVIDEO_ZOOM_ENGINE_PATH = join(buildDir, "corevideo-zoom-engine.exe");
}
if (!process.env.COREVIDEO_ZOOM_PUBLIC_APP_KEY) {
  process.env.COREVIDEO_ZOOM_PUBLIC_APP_KEY = embeddedKey;
}
if (!process.env.COREVIDEO_ZOOM_JOIN_WAIT_MS) {
  process.env.COREVIDEO_ZOOM_JOIN_WAIT_MS = "45000";
}

const child = spawn(nativeCore, [], {
  cwd: buildDir,
  env: process.env,
  stdio: ["pipe", "pipe", "pipe"],
});

let buf = "";
const fail = (message) => {
  console.error(message);
  child.kill();
  process.exit(1);
};

child.stdout.on("data", (chunk) => {
  buf += chunk.toString();
  let idx;
  while ((idx = buf.indexOf("\n")) >= 0) {
    const line = buf.slice(0, idx).trim();
    buf = buf.slice(idx + 1);
    if (!line) continue;
    console.log("<<", line);
    try {
      const msg = JSON.parse(line);
      if (msg.type === "handshake") {
        console.log("handshake profile:", msg.profile?.name, msg.profile?.capabilities?.slice(0, 5));
        const join = JSON.stringify({
          id: "auth-test",
          type: "zoom-join",
          payload: {
            meetingUrl: process.env.COREVIDEO_TEST_MEETING_URL ?? "https://zoom.us/j/123456789",
            displayName: "CoreVideo Auth Probe",
            webinar: false,
          },
        });
        console.log(">>", join);
        child.stdin.write(join + "\n");
      }
      if (msg.type === "zoom-join") {
        console.log("zoom-join snapshot:", JSON.stringify(msg.snapshot, null, 2));
        child.kill();
        process.exit(0);
      }
      if (msg.ok === false) fail(`request failed: ${line}`);
    } catch {
      // ignore non-json
    }
  }
});

child.stderr.on("data", (chunk) => process.stderr.write(chunk));
setTimeout(() => fail("timeout after 120s"), 120000);