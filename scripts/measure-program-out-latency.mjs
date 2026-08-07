/**
 * PROGRAM-OUT CADENCE + LATENCY BUDGET.
 *
 * The product target is hardware-switcher behaviour: ~1 frame of processing from
 * source to output. `[zoom-latency] ingest->render` already measures the FIRST
 * half (a decoded frame waiting to be fetched by the compositor). Nothing
 * measured the SECOND half — compositor -> program output — which is where the
 * asynchronous hops live:
 *
 *   render thread 60Hz  ->  vcam tap thread (own D3D device, GPU convert +
 *   readback)  ->  output worker ~50Hz (takeVcamNv12 + publishNv12 + encoder
 *   submit)  ->  SHM  ->  Frame Server  ->  the consuming app
 *
 * Each hop is free-running against the next, so each can cost up to a frame.
 * This measures what actually comes out the other end: the rate at which NEW
 * program frames reach the virtual-camera SHM, read straight from the seqlock
 * header the DLL reads. A 60fps program that publishes at 50fps is not just
 * 10 lost frames a second — it means every output is sampled through a 20ms
 * gate, which is latency the switcher budget cannot afford.
 *
 * Usage: node ./scripts/measure-program-out-latency.mjs [--seconds 20]
 */
import { spawn } from "node:child_process";
import { existsSync, openSync, readSync, closeSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(here, "..");
const buildDir = join(repoRoot, "native", "build-dev");
const exeSuffix = process.platform === "win32" ? ".exe" : "";
const nativeCore = join(buildDir, `corevideo-native${exeSuffix}`);
const fakeEngine = join(buildDir, `corevideo-zoom-engine-fake${exeSuffix}`);
const SHM = "C:\\ProgramData\\CoreVideoPro\\vcam-frame.shm";

const args = process.argv.slice(2);
const argValue = (name, fallback) => {
  const i = args.indexOf(`--${name}`);
  return i >= 0 && args[i + 1] ? args[i + 1] : fallback;
};
const seconds = Number(argValue("seconds", 20));

if (!existsSync(nativeCore)) {
  console.error(`Missing ${nativeCore}`);
  process.exit(1);
}

const child = spawn(nativeCore, [], {
  cwd: buildDir,
  env: {
    ...process.env,
    COREVIDEO_ZOOM_ENGINE_PATH: existsSync(fakeEngine) ? fakeEngine : undefined,
    COREVIDEO_FFMPEG_DIR: "C:\\ffmpeg\\bin",
  },
  stdio: ["pipe", "pipe", "pipe"],
});

const startedAt = Date.now();
let nextId = 1;
let stdoutBuffer = "";
let handshake;
const pending = new Map();
// The core reports its own worker cadences on stderr; capture them so the
// published rate can be attributed to a stage rather than guessed at.
const renderFps = [];
const audioTicks = [];

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
child.stderr.on("data", (chunk) => {
  for (const line of chunk.toString().split("\n")) {
    let m = line.match(/\[render\]\s+([\d.]+)fps/);
    if (m) renderFps.push(Number(m[1]));
    m = line.match(/\[audioOut\]\s+([\d.]+)\s+ticks\/s/);
    if (m) audioTicks.push(Number(m[1]));
  }
});

function send(type, payload = {}) {
  const id = `lat-${nextId++}`;
  return new Promise((res, rej) => {
    const timer = setTimeout(() => { pending.delete(id); rej(new Error(`${type} timed out`)); }, 30000);
    pending.set(id, { resolve: res, reject: rej, timer });
    child.stdin.write(`${JSON.stringify({ id, type, ...payload })}\n`);
  }).then((r) => {
    if (r.ok === false) throw new Error(`${type} failed: ${r.error?.message ?? "unknown"}`);
    return r;
  });
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// Read the vcam seqlock header the DLL reads: magic, seq, w, h, fps, byteLen,
// then frameNumber. Only frameNumber and the dims are needed here.
function readShmHeader() {
  try {
    const fd = openSync(SHM, "r");
    const buf = Buffer.alloc(40);
    readSync(fd, buf, 0, 40, 0);
    closeSync(fd);
    return {
      magic: buf.readUInt32LE(0),
      width: buf.readUInt32LE(8),
      height: buf.readUInt32LE(12),
      declaredFps: buf.readUInt32LE(16),
      frameNumber: Number(buf.readBigUInt64LE(24)),
    };
  } catch {
    return null;
  }
}

try {
  for (let i = 0; i < 200 && !handshake; i += 1) await sleep(50);
  if (!handshake) throw new Error("no native-core handshake");

  await send("zoom-join", { payload: { meetingNumber: "1234567890", displayName: "latency-probe" } });
  await sleep(3000);

  await send("media-core-sync", {
    elapsedMs: Date.now() - startedAt,
    commands: [
      {
        type: "load-scene-graph",
        sceneId: "latency-probe",
        routes: [{ routeId: "program", mode: "fixed", audioRole: "mix", participantId: "speaker-1" }],
      },
      { type: "sync-virtual-camera", on: true, mirror: false, deviceName: "latency-probe" },
    ],
  });
  await sleep(3000);  // vcam publisher spin-up

  // Sample the published frame counter across the window. Deltas per sample are
  // what expose a beat pattern (50Hz sampling a 60Hz program), which a single
  // average would hide.
  const samples = [];
  let previous = readShmHeader();
  if (!previous || previous.magic !== 0x43564643) {
    throw new Error(`vcam SHM not initialised (magic=${previous?.magic?.toString(16) ?? "none"})`);
  }
  let previousAt = Date.now();
  for (let i = 0; i < seconds * 2; i += 1) {
    await sleep(500);
    const now = readShmHeader();
    const at = Date.now();
    if (now) {
      samples.push((now.frameNumber - previous.frameNumber) / ((at - previousAt) / 1000));
      previous = now;
      previousAt = at;
    }
  }

  const sorted = [...samples].sort((a, b) => a - b);
  const median = sorted[Math.floor(sorted.length / 2)] ?? 0;
  const worst = sorted[0] ?? 0;
  const head = readShmHeader();

  const avg = (xs) => (xs.length ? xs.reduce((a, b) => a + b, 0) / xs.length : 0);
  console.log(`program        : ${head?.width}x${head?.height}, DLL media type declares ${head?.declaredFps}fps`);
  console.log(`render thread  : ${avg(renderFps).toFixed(1)} fps (composites the program)`);
  console.log(`output worker  : ${avg(audioTicks).toFixed(1)} ticks/s (samples the program tap)`);
  console.log(`vcam PUBLISHED : ${median.toFixed(1)} fps median, ${worst.toFixed(1)} worst window`);
  const gate = avg(audioTicks);
  if (gate > 0 && median <= gate * 1.05 && gate < 58) {
    console.log(
      `\nThe published rate tracks the OUTPUT WORKER (${gate.toFixed(1)}Hz), not the render\n` +
      `thread (${avg(renderFps).toFixed(1)}fps). Every output — vcam, stream, recording — is sampled\n` +
      `through that gate, so the program is quantised to ${(1000 / gate).toFixed(1)}ms regardless of\n` +
      `how fast it is composited.`);
  }
} catch (error) {
  console.error(`FAILED: ${error.message}`);
  process.exitCode = 1;
} finally {
  try { child.stdin.end(); } catch {}
  child.kill();
}
