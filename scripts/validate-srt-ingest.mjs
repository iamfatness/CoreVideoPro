/**
 * Headless SRT INGEST proof.
 *
 * The previous ingest adapter opened a libsrt socket and threw the packets away
 * â€” it counted bytes and emitted frames with NO PIXELS. It would have passed any
 * "is the source connected" check while showing nothing, which is exactly why
 * this harness judges on DECODED PIXELS reaching the compositor, not on status
 * strings.
 *
 * Pushes a known test pattern into the core over real SRT (ffmpeg publisher),
 * then asserts the core's capture source reports a connected feed AND that the
 * program it composites from that source is not blank.
 *
 * Usage: node ./scripts/validate-srt-ingest.mjs [--seconds 18] [--port 9040] [--keep]
 */
import { spawn, spawnSync } from "node:child_process";
import { existsSync, rmSync, statSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(here, "..");
const buildDir = join(repoRoot, "native", "build-dev");
const exeSuffix = process.platform === "win32" ? ".exe" : "";
const nativeCore = join(buildDir, `corevideo-native${exeSuffix}`);

const args = process.argv.slice(2);
const argValue = (name, fallback) => {
  const index = args.indexOf(`--${name}`);
  return index >= 0 && args[index + 1] ? args[index + 1] : fallback;
};
const seconds = Number(argValue("seconds", 18));
const port = Number(argValue("port", 9040));
const keep = args.includes("--keep");
const deviceId = "srt-ingest-1";

const ffBin = (name) => {
  for (const bin of [name, `C:\\ffmpeg\\bin\\${name}.exe`]) {
    const probe = spawnSync(bin, ["-version"], { encoding: "utf8", timeout: 10000 });
    if (!probe.error && probe.status === 0) return bin;
  }
  return null;
};
const ffmpeg = ffBin("ffmpeg");
const ffprobe = ffBin("ffprobe");
if (!ffmpeg || !ffprobe) {
  console.error("ffmpeg/ffprobe are required for the SRT ingest proof.");
  process.exit(1);
}
if (!existsSync(nativeCore)) {
  console.error(`Missing ${nativeCore}.`);
  process.exit(1);
}

const child = spawn(nativeCore, [], {
  cwd: buildDir,
  env: { ...process.env, COREVIDEO_FFMPEG_DIR: "C:\\ffmpeg\\bin" },
  stdio: ["pipe", "pipe", "pipe"],
});

const startedAt = Date.now();
let nextId = 1;
let stdoutBuffer = "";
let handshake;
const pending = new Map();

child.stdout.on("data", (chunk) => {
  stdoutBuffer += chunk.toString();
  let idx;
  while ((idx = stdoutBuffer.indexOf("\n")) >= 0) {
    const line = stdoutBuffer.slice(0, idx).trim();
    stdoutBuffer = stdoutBuffer.slice(idx + 1);
    if (!line) continue;
    let msg;
    try { msg = JSON.parse(line); } catch { continue; }
    if (msg.type === "handshake" && msg.ok === true) handshake = msg;
    if (typeof msg.id === "string" && pending.has(msg.id)) {
      const item = pending.get(msg.id);
      pending.delete(msg.id);
      clearTimeout(item.timer);
      item.resolve(msg);
    }
  }
});
child.stderr.on("data", () => {});

function send(type, payload = {}) {
  const id = `ingest-${nextId++}`;
  return new Promise((resolvePromise, reject) => {
    const timer = setTimeout(() => { pending.delete(id); reject(new Error(`${type} timed out`)); }, 30000);
    pending.set(id, { resolve: resolvePromise, reject, timer });
    child.stdin.write(`${JSON.stringify({ id, type, ...payload })}\n`);
  }).then((response) => {
    if (response.ok === false) throw new Error(`${type} failed: ${response.error?.message ?? "unknown"}`);
    return response;
  });
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const failures = [];
let publisher = null;
let artifact = null;
try {
  for (let i = 0; i < 200 && !handshake; i += 1) await sleep(50);
  if (!handshake) throw new Error("no native-core handshake");

  // Arm the SRT source as a LISTENER so the publisher can connect into it.
  await send("media-core-sync", {
    elapsedMs: Date.now() - startedAt,
    commands: [{
      type: "configure-srt-ingest-sources",
      sources: [{
        id: "input-1", deviceId, name: "SRT Ingest 1",
        mode: "listener", host: "0.0.0.0", port, latencyMs: 120,
      }],
    }],
  });
  // connect-capture-device is a TOP-LEVEL rpc, not a media-core-sync command.
  await send("connect-capture-device", { payload: { deviceId, outputSourceId: deviceId } });
  await sleep(2000);

  // Push a known pattern in over real SRT.
  publisher = spawn(ffmpeg,
    ["-hide_banner", "-loglevel", "error", "-re",
     "-f", "lavfi", "-i", "testsrc=size=1280x720:rate=30",
     "-t", String(seconds + 6), "-c:v", "h264_nvenc", "-pix_fmt", "yuv420p",
     "-f", "mpegts", `srt://127.0.0.1:${port}?mode=caller&transtype=live`],
    { stdio: ["ignore", "ignore", "pipe"] });
  let publisherErr = "";
  publisher.stderr.on("data", (c) => { publisherErr += c.toString(); });
  console.log(`publisher     : pushing testsrc into srt://127.0.0.1:${port} ...`);

  // Put the SRT source on program and record, so the proof is that decoded
  // pixels reach the compositor â€” not merely that a status flipped.
  await sleep(4000);
  await send("media-core-sync", {
    elapsedMs: Date.now() - startedAt,
    commands: [
      {
        type: "load-scene-graph",
        sceneId: "srt-ingest",
        routes: [{ routeId: "program", mode: "capture-input", audioRole: "mix", captureDeviceId: deviceId }],
      },
      { type: "sync-virtual-camera", on: true, mirror: false, deviceName: "srt-ingest-proof" },
      { type: "start-program-output", destinations: ["recording"], isoParticipantIds: [] },
      {
        type: "set-recording-targets",
        targetFolder: "Recordings/CoreVideoPro/validate-srt-ingest",
        filenamePrefix: "srt-ingest", format: "mp4", quality: "high", isoParticipantIds: [],
      },
    ],
  });
  await send("media-core-sync", {
    elapsedMs: Date.now() - startedAt,
    commands: [{
      type: "start-recording-session", sessionId: "srt-ingest", startedAtMs: Date.now(),
      targetFolder: "Recordings/CoreVideoPro/validate-srt-ingest",
      filenamePrefix: "srt-ingest", format: "mp4", quality: "high", isoParticipantIds: [],
    }],
  });

  let device = null;
  const deadline = Date.now() + seconds * 1000;
  while (Date.now() < deadline) {
    await sleep(3000);
    const sync = await send("media-core-sync", { elapsedMs: Date.now() - startedAt, commands: [] });
    const devices = sync.snapshot?.captureDevices ?? [];
    device = devices.find((d) => d.id === deviceId) ?? null;
    if (device) {
      console.log(`source        : state=${device.connectionState} signal=${device.signalPresent} ` +
                  `warning=${device.warning || "none"}`);
    }
  }

  if (!device) failures.push("the SRT ingest device never appeared in captureDevices");
  else if (!device.signalPresent) {
    failures.push(`SRT source never reported signal (state=${device.connectionState}, ` +
                  `warning=${device.warning || "none"})` +
                  (publisherErr ? ` | publisher: ${publisherErr.trim().split("\n").pop()}` : ""));
  }

  const stop = await send("media-core-sync", {
    elapsedMs: Date.now() - startedAt,
    commands: [{ type: "stop-recording-session", reason: "srt ingest proof complete" }],
  });
  const path = stop.snapshot?.recording?.artifactPath ?? null;
  if (path) {
    artifact = resolve(buildDir, path);
    for (let i = 0; i < 40; i += 1) {
      await sleep(250);
      let size = 0;
      try { size = statSync(artifact).size; } catch { continue; }
      if (size > 1024) break;
    }
  }
} catch (error) {
  failures.push(error.message);
} finally {
  try { child.stdin.end(); } catch {}
  child.kill();
  if (publisher) { try { publisher.kill(); } catch {} }
}

await sleep(1500);

// The decisive check: did the ingested feed actually become PIXELS on program?
if (artifact && existsSync(artifact)) {
  const out = spawnSync(ffmpeg,
    ["-v", "error", "-i", artifact, "-vf", "scale=8:8", "-f", "rawvideo", "-pix_fmt", "gray", "-"],
    { encoding: "buffer", maxBuffer: 1 << 28, timeout: 120000 });
  const cells = 64;
  const frames = Math.floor((out.stdout?.length ?? 0) / cells);
  let best = 0;
  for (let f = Math.min(frames - 1, 30); f < frames; f += 1) {
    let sum = 0;
    for (let i = 0; i < cells; i += 1) sum += out.stdout[f * cells + i];
    best = Math.max(best, sum / cells);
  }
  console.log(`program luma  : peak ${best.toFixed(1)} over ${frames} frames`);
  if (frames === 0) failures.push("program recording produced no frames");
  else if (best < 12) failures.push(`program stayed black (peak luma ${best.toFixed(1)}) â€” the ingested SRT feed never became pixels`);
  if (!keep) { try { rmSync(artifact); } catch {} }
} else if (failures.length === 0) {
  failures.push("no program recording artifact to inspect");
}

if (failures.length) {
  console.error("\nSRT INGEST VALIDATION FAIL");
  for (const f of failures) console.error(`  - ${f}`);
  process.exit(1);
}
console.log("\nSRT INGEST VALIDATION PASS");

